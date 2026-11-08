"""Verified cold storage relocation with retained originals and journaled Undo."""

from __future__ import annotations

import contextlib
import os
from pathlib import Path
import re
import shutil
import time
import uuid

from . import disks
from .avdlock import AvdLock
from .core import atomic_bytes, atomic_json, lease, private_directory, read_json, validate_metadata
from .fileops import ReadLocks, Tree, check_cancel, copy_file
from .host import allocations
from .phone import bounded_process
from .recovery import stamp
from .vault import BASES, DISKS, IMAGE, Progress, read_value, require


TOKEN = re.compile(r"[a-f0-9]{32}\Z")
SKIP = frozenset(
    (
        "avd/running",
        "running",
        "avd/Gaming_Phone.avd/tmpAdbCmds",
        "avd/Gaming_Phone.avd/snapshots",
        "avd/Gaming_Phone.avd/multiinstance.lock",
    )
)


class Relocation:
    def __init__(self, storage):
        self.storage = storage
        self.phone, self.paths, self.cancel = storage.phone, storage.paths, storage.cancel
        self.root = private_directory(self.paths.root / "StorageMoves")

    def owned(self, path):
        path = Path(path)
        require(
            path.is_absolute()
            and path.resolve(strict=True) == path
            and path != self.paths.root
            and self.paths.sdk not in path.parents,
            "Invalid relocated phone folder.",
        )
        stamp(path)
        with Tree(path) as tree:
            marker = read_value(tree, ".velox-owner.json", 65536)
        require(
            marker
            == {"application": "velox", "phone": self.phone.identity, "root": str(self.paths.root)},
            "This storage folder does not belong to the selected phone.",
        )
        return path

    def record(self, token):
        require(isinstance(token, str) and TOKEN.fullmatch(token), "Invalid storage move identity.")
        value = read_json(self.root / (token + ".json"), None, 65536)
        require(
            isinstance(value, dict)
            and value.get("format") == "velox-linux-storage-move"
            and type(value.get("version")) is int
            and value["version"] == 1
            and value.get("token") == token
            and value.get("phone") == self.phone.identity
            and value.get("root") == str(self.paths.root)
            and value.get("phase")
            in ("copying", "prepared", "active", "undoing", "undone", "cancelled")
            and type(value.get("created")) is int
            and 0 < value["created"] <= 4102444800
            and isinstance(value.get("original"), dict),
            "This storage move record is damaged.",
        )
        validate_metadata(self.phone.identity, value["original"])
        for key in ("desktop", "old_directory", "new_directory"):
            require(
                isinstance(value.get(key), list)
                and len(value[key]) == 2
                and all(type(number) is int and number >= 0 for number in value[key]),
                "This storage move record is damaged.",
            )
        for key in ("old_home", "new_home"):
            path = value.get(key)
            require(
                isinstance(path, str)
                and 0 < len(path) <= 4096
                and Path(path).is_absolute()
                and not any(ord(char) < 32 for char in path),
                "Invalid relocated phone folder.",
            )
        require(
            Path(value["new_home"]).name == self.folder_name(token),
            "Invalid storage move destination.",
        )
        expected = value["original"].get("home") or str(self.phone.folder / "Android")
        require(
            value["old_home"] == expected and value["old_home"] != value["new_home"],
            "This storage move record is damaged.",
        )
        return value

    def folder_name(self, token):
        return "Velox-Phone-" + self.phone.identity + "-" + token

    def origin(self):
        # A recovered phone may be moved more than once. Follow only validated
        # same-phone receipts, keeping the original recovery base check intact.
        records = []
        for path in self.root.iterdir():
            if not TOKEN.fullmatch(path.stem) or path.suffix != ".json":
                continue
            require(len(records) < 1000, "Too many storage moves. Review the storage folder first.")
            value = read_json(path, {}, 65536)
            if isinstance(value, dict) and value.get("phone") == self.phone.identity:
                records.append(self.record(path.stem))
        current = str(self.phone.home)
        seen = set()
        while current not in seen:
            seen.add(current)
            matching = [
                value
                for value in records
                if value["new_home"] == current
                and value["phase"] in ("prepared", "active", "undoing")
                and value["desktop"] == stamp(self.phone.folder)
            ]
            if not matching:
                return Path(current)
            require(len(matching) == 1, "This storage move record is damaged.")
            value = matching[0]
            require(
                stamp(self.owned(current)) == value["new_directory"]
                and stamp(self.owned(value["old_home"])) == value["old_directory"],
                "A retained phone storage folder changed.",
            )
            current = value["old_home"]
        require(False, "This storage move record is damaged.")

    def write(self, value):
        atomic_json(self.root / (value["token"] + ".json"), value)

    def active(self):
        result, visited = [], 0
        for path in self.root.iterdir():
            if not TOKEN.fullmatch(path.stem) or path.suffix != ".json":
                continue
            visited += 1
            require(visited <= 1000, "Too many storage moves. Review the storage folder first.")
            value = read_json(path, {}, 65536)
            if not isinstance(value, dict) or value.get("phone") != self.phone.identity:
                continue
            value = self.record(path.stem)
            if (
                value["phase"] in ("prepared", "active", "undoing")
                and stamp(self.phone.folder) == value["desktop"]
                and str(self.phone.home) == value["new_home"]
            ):
                result.append(value)
        return max(result, key=lambda value: value["created"], default=None)

    def choose_parent(self, selected):
        parent = Path(selected).expanduser().resolve(strict=True)
        require(
            parent.is_dir()
            and parent.stat().st_uid == os.getuid()
            and os.access(parent, os.W_OK | os.X_OK),
            "Choose a writable storage folder owned by your account.",
        )
        protected = [
            self.paths.sdk,
            self.paths.phones,
            self.paths.vault,
            self.paths.cache,
            self.paths.root / "Removed",
            self.root,
        ]
        for item in self.phone.hub.list():
            if not item.get("error"):
                protected.append(self.paths.data_home(item["id"]))
        require(
            not any(parent == path or path in parent.parents for path in protected),
            "Choose storage outside existing phones, backups and shared Android tools.",
        )
        return parent

    def move(self, selected, report=lambda _message, _fraction: None):
        check_cancel(self.cancel)
        phone = self.phone
        progress = Progress(report)
        with (
            lease(self.paths.locks / (phone.identity + ".maintenance")),
            lease(self.paths.locks / "sdk.lock", shared=True),
        ):
            self.storage.cold()
            require(phone.avd.is_dir(), "Start this phone once before moving its storage.")
            old = self.owned(phone.home)
            parent_path = self.choose_parent(selected)
            token = uuid.uuid4().hex
            destination = parent_path / self.folder_name(token)
            with AvdLock(phone.avd), Tree(old) as source, Tree(parent_path) as parent:
                entries = source.entries(lambda name, _directory: name not in SKIP)
                require(
                    shutil.disk_usage(parent_path).free
                    >= sum(entry.allocated for entry in entries) + 256 * 1024**2,
                    "Free more space before moving this phone.",
                )
                os.mkdir(destination.name, 0o700, dir_fd=parent.descriptor)
                created = stamp(destination)
                value = {
                    "format": "velox-linux-storage-move",
                    "version": 1,
                    "token": token,
                    "phone": phone.identity,
                    "root": str(self.paths.root),
                    "created": int(time.time()),
                    "phase": "copying",
                    "original": phone.metadata,
                    "desktop": stamp(phone.folder),
                    "old_home": str(old),
                    "new_home": str(destination),
                    "old_directory": stamp(old),
                    "new_directory": created,
                }
                committed = False
                try:
                    self.write(value)
                    with Tree(destination, writable=True) as copied, ReadLocks(source, entries):
                        total = sum(entry.size for entry in entries) or 1
                        completed = 0
                        for entry in entries:
                            copy_file(
                                source,
                                copied,
                                entry,
                                cancel=self.cancel,
                                progress=lambda amount, done=completed: progress(
                                    "Copying phone storage", 0.05 + 0.75 * (done + amount) / total
                                ),
                            )
                            completed += entry.size
                        self.prepare_data(destination, old, progress)
                        copied.sync([entry.name for entry in copied.entries()])
                    # Verify that path lookup still reaches the open chosen parent.
                    parent_info = os.fstat(parent.descriptor)
                    require(
                        parent_path.resolve() == parent_path
                        and (parent_path.stat().st_dev, parent_path.stat().st_ino)
                        == (parent_info.st_dev, parent_info.st_ino),
                        "The destination folder changed during copying.",
                    )
                    require(
                        stamp(destination) == created and stamp(old) == value["old_directory"],
                        "Phone storage changed during copying.",
                    )
                    check_cancel(self.cancel)
                    value["phase"] = "prepared"
                    self.write(value)
                    with lease(self.paths.locks / "catalog.lock", blocking=True):
                        require(
                            phone.metadata == value["original"]
                            and stamp(phone.folder) == value["desktop"],
                            "The selected phone changed while its storage was copied.",
                        )
                        atomic_json(phone.folder / "snapshot.json", {"clean": False})
                        atomic_json(
                            phone.folder / "phone.json",
                            dict(value["original"], home=str(destination)),
                        )
                        committed = True
                    value["phase"] = "active"
                    self.write(value)
                finally:
                    # A commit may succeed just before an fsync or acknowledgement fails.
                    # In that case retain both copies and let the metadata identify it.
                    current = read_json(phone.folder / "phone.json", {})
                    committed = committed or current.get("home") == str(destination)
                    if not committed:
                        with contextlib.suppress(OSError):
                            if stamp(destination) == created:
                                shutil.rmtree(destination)
                        value["phase"] = "cancelled"
                        self.write(value)
            progress("Phone storage moved. The original is retained for Undo.", 1, force=True)
            return value

    def prepare_data(self, destination, old, progress):
        avd_path = destination / "avd/Gaming_Phone.avd"
        with Tree(avd_path, writable=True) as avd, Tree(self.paths.sdk / IMAGE) as image:
            names = {entry.name for entry in avd.entries() if entry.name in DISKS}
            require(
                {"userdata-qemu.img", "encryptionkey.img"} <= names,
                "The phone's data or encryption keys are missing.",
            )
            capacity = 0
            tool = self.phone.hub.setup.verify_executable("emulator/qemu-img")
            for name in sorted(names):
                check_cancel(self.cancel)
                virtual = disks.rebase(
                    avd,
                    name,
                    str(old / "avd/Gaming_Phone.avd"),
                    str(image.root),
                    image,
                    names,
                    BASES,
                )
                if name.startswith("userdata-qemu.img"):
                    capacity = max(capacity, virtual or (avd_path / name).stat().st_size)
                descriptor = avd.open(name)
                try:
                    header = disks.inspect(descriptor)
                finally:
                    os.close(descriptor)
                if header:
                    progress("Checking copied disks", 0.85, force=True)
                    bounded_process(
                        [str(tool), "check", "-r", "leaks", "-f", "qcow2", str(avd_path / name)],
                        environment=self.phone.environment(),
                        timeout=180,
                        maximum=2 * 1024**2,
                        cancel=self.cancel,
                    )
            settings = self.paths.settings(self.phone.identity)
            cpu, ram = allocations(settings, self.phone.hub.hardware)
            self.phone._configure(settings, cpu, ram, home=destination)
            config = avd_path / "config.ini"
            text = config.read_text(encoding="utf-8")
            text = re.sub(
                r"^disk.dataPartition.size=.*$",
                f"disk.dataPartition.size={(capacity + 1024**2 - 1) // 1024**2}M",
                text,
                flags=re.M,
            )
            atomic_bytes(config, text.encode())

    def undo(self, report=lambda _message, _fraction: None):
        phone = self.phone
        with (
            lease(self.paths.locks / (phone.identity + ".maintenance")),
            lease(self.paths.locks / "sdk.lock", shared=True),
        ):
            self.storage.cold()
            value = self.active()
            require(value is not None, "This phone has no storage move to undo.")
            old, current = self.owned(value["old_home"]), self.owned(value["new_home"])
            require(
                stamp(old) == value["old_directory"] and stamp(current) == value["new_directory"],
                "A retained phone storage folder changed.",
            )
            with (
                AvdLock(old / "avd/Gaming_Phone.avd"),
                AvdLock(current / "avd/Gaming_Phone.avd"),
                lease(self.paths.locks / "catalog.lock", blocking=True),
            ):
                require(
                    stamp(phone.folder) == value["desktop"] and str(phone.home) == str(current),
                    "The selected phone changed while undoing its storage move.",
                )
                check_cancel(self.cancel)
                value["phase"] = "undoing"
                self.write(value)
                metadata = phone.metadata
                metadata["home"] = value["original"].get("home", "")
                atomic_json(phone.folder / "snapshot.json", {"clean": False})
                atomic_json(phone.folder / "phone.json", metadata)
                value["phase"] = "undone"
                self.write(value)
        report("Previous storage restored. The moved copy is also retained.", 1)
        return value
