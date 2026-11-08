"""Prepare offline copies, then atomically exchange a phone with a retained Undo."""

from __future__ import annotations

import contextlib
import ctypes as C
import errno
import os
from pathlib import Path
import re
import shutil
import stat
import time
from types import SimpleNamespace
import uuid

from . import disks
from .avdlock import AvdLock
from .controls import Controls
from .core import (
    INSTANCE,
    PACKAGE,
    Settings,
    VeloxError,
    atomic_bytes,
    atomic_json,
    lease,
    private_directory,
    read_json,
    safe_name,
    validate_metadata,
)
from .fileops import Tree, ReadLocks, check_cancel, copy_file, hash_descriptor, relative_name
from .host import allocations
from .phone import bounded_process
from .vault import BASES, DISKS, HEX, IMAGE, Progress, Vault, desktop_file, read_value, require


TOKEN = re.compile(r"[a-f0-9]{32}\Z")
MARKERS = frozenset(("config.ini", "qemu-version.txt", "version_num.cache"))


def stamp(path):
    info = Path(path).lstat()
    require(
        stat.S_ISDIR(info.st_mode)
        and info.st_uid == os.getuid()
        and stat.S_IMODE(info.st_mode) == 0o700,
        "Recovery requires a private folder owned by your account.",
    )
    return [info.st_dev, info.st_ino]


def exchange(parent, first, second):
    """No missing-phone interval and no cross-filesystem rename fallback."""
    require(
        INSTANCE.fullmatch(first)
        and TOKEN.fullmatch(second.removeprefix(".recovery-"))
        and second.startswith(".recovery-"),
        "Invalid recovery exchange names.",
    )
    opened = os.fstat(parent.descriptor)
    require(
        stamp(parent.root) == [opened.st_dev, opened.st_ino],
        "Phone storage changed during recovery.",
    )
    library = C.CDLL(None, use_errno=True)
    function = getattr(library, "renameat2", None)
    require(function is not None, "This Linux runtime does not support atomic phone recovery.")
    function.argtypes = [C.c_int, C.c_char_p, C.c_int, C.c_char_p, C.c_uint]
    function.restype = C.c_int
    if (
        function(parent.descriptor, os.fsencode(first), parent.descriptor, os.fsencode(second), 2)
        != 0
    ):
        code = C.get_errno()
        if code in (errno.ENOSYS, errno.EINVAL, errno.EOPNOTSUPP, errno.EXDEV):
            raise VeloxError(
                "This storage does not support atomic recovery. Use a local Linux filesystem."
            )
        raise OSError(code, os.strerror(code))
    os.fsync(parent.descriptor)


def payload(name):
    parts = relative_name(name)
    return name in (".velox-owner.json", "avd/Gaming_Phone.ini") or (
        len(parts) == 3 and parts[:2] == ("avd", "Gaming_Phone.avd") and parts[2] in DISKS | MARKERS
    )


def data_entries(tree):
    return tree.entries(
        lambda name, directory: directory or name != "avd/Gaming_Phone.avd/multiinstance.lock"
    )


class Recovery:
    def __init__(self, phone):
        self.phone, self.paths = phone, phone.paths
        self.root = private_directory(self.paths.vault / "Prepared")
        self.vault = Vault(phone)
        self.cancel = self.vault.cancel

    def location(self, token):
        require(
            isinstance(token, str) and TOKEN.fullmatch(token), "Invalid prepared recovery identity."
        )
        return self.root / token, self.paths.phones / (".recovery-" + token)

    def record(self, token):
        folder, _slot = self.location(token)
        with Tree(folder) as tree:
            value = read_value(tree, "prepared.json")
        require(
            isinstance(value, dict)
            and value.get("format") == "velox-linux-recovery"
            and value.get("version") == 1
            and type(value["version"]) is int
            and value.get("id") == token
            and value.get("phone") == self.phone.identity
            and value.get("root") == str(self.paths.root)
            and value.get("phase") in ("prepared", "activating", "active", "undoing", "undone"),
            "This recovery record is damaged or belongs to another phone.",
        )
        require(
            type(value.get("created")) is int
            and 0 < value["created"] <= 4102444800
            and isinstance(value.get("label"), str)
            and safe_name(value["label"]) == value["label"],
            "Invalid recovery label or date.",
        )
        for name in ("old_directory", "new_directory"):
            require(
                isinstance(value.get(name), list)
                and len(value[name]) == 2
                and all(type(number) is int and number >= 0 for number in value[name]),
                "Invalid recovery directory identity.",
            )
        require(
            isinstance(value.get("original"), dict)
            and value["original"].get("id") == self.phone.identity,
            "Invalid original phone registration.",
        )
        validate_metadata(self.phone.identity, value["original"])
        files = value.get("files")
        require(
            isinstance(files, dict)
            and 3 <= len(files) <= 4000
            and isinstance(value.get("bases"), dict)
            and set(value["bases"]) == BASES,
            "Invalid prepared recovery inventory.",
        )
        seen, total = set(), 0
        for name, row in files.items():
            parts = relative_name(name)
            supported = (parts[0] == "Android" and payload("/".join(parts[1:]))) or (
                parts[0] == "desktop"
                and (
                    desktop_file("/".join(parts[1:]))
                    or name in ("desktop/phone.json", "desktop/snapshot.json")
                )
            )
            require(
                supported
                and name.casefold() not in seen
                and isinstance(row, dict)
                and type(row.get("bytes")) is int
                and 0 <= row["bytes"] <= 512 * 1024**3
                and isinstance(row.get("sha256"), str)
                and HEX.fullmatch(row["sha256"]),
                "Invalid prepared recovery file.",
            )
            total += row["bytes"]
            seen.add(name.casefold())
        require(
            total <= 1024**4
            and {
                "desktop/phone.json",
                "desktop/settings.json",
                "Android/avd/Gaming_Phone.avd/userdata-qemu.img",
                "Android/avd/Gaming_Phone.avd/encryptionkey.img",
            }
            <= set(files),
            "The prepared recovery is incomplete.",
        )
        return value

    def _write(self, record):
        folder, _slot = self.location(record["id"])
        atomic_json(folder / "prepared.json", record)

    def reconcile(self, token):
        value = self.record(token)
        _folder, slot = self.location(token)
        if value["phase"] not in ("activating", "undoing"):
            return value
        current, previous = stamp(self.phone.folder), stamp(slot)
        old, new = value["old_directory"], value["new_directory"]
        require(
            (current, previous) in ((old, new), (new, old)),
            "Recovery storage changed. Both phone copies are retained for inspection.",
        )
        value["phase"] = (
            ("active" if current == new else "prepared")
            if value["phase"] == "activating"
            else ("undone" if current == old else "active")
        )
        self._write(value)
        return value

    def _cold(self):
        require(
            not self.phone.alive() and self.phone.state == "stopped",
            "Stop this phone before preparing or activating recovery.",
        )

    @contextlib.contextmanager
    def _avd_lock(self, path):
        if path.exists():
            with AvdLock(path):
                yield
        else:
            yield

    @contextlib.contextmanager
    def _desktop_lock(self, slot):
        with contextlib.ExitStack() as stack:
            stack.enter_context(
                lease(self.paths.locks / (self.phone.identity + ".settings"), blocking=True)
            )
            stack.enter_context(
                lease(self.paths.locks / (self.phone.identity + ".wallpaper"), blocking=True)
            )
            packages = set()
            for root in (self.phone.folder, slot):
                folder = root / "controls"
                if folder.is_dir() and not folder.is_symlink():
                    packages.update(
                        path.stem for path in folder.glob("*.json") if PACKAGE.fullmatch(path.stem)
                    )
            for package in sorted(packages):
                stack.enter_context(
                    lease(self.paths.locks / ("controls-" + package + ".lock"), blocking=True)
                )
            yield

    def _validate_desktop(self, tree):
        settings = Settings.from_dict(read_value(tree, "settings.json", 65536))
        controls = Controls(SimpleNamespace(folder=tree.root))
        for entry in tree.entries():
            if entry.name.startswith("controls/"):
                controls._read(Path(entry.name).stem)
        pending = tree.root / "wallpaper-pending.json"
        if pending.exists():
            value = read_value(tree, "wallpaper-pending.json", 65536)
            require(
                isinstance(value, dict)
                and isinstance(value.get("file"), str)
                and re.fullmatch(r"pending-[a-f0-9]{32}\.png", value.get("file", ""))
                and isinstance(value.get("sha256"), str)
                and HEX.fullmatch(value["sha256"]),
                "The queued wallpaper is invalid.",
            )
            descriptor = tree.open("wallpapers/" + value["file"])
            try:
                require(
                    hash_descriptor(descriptor, self.cancel) == value["sha256"],
                    "The queued wallpaper failed its checksum.",
                )
            finally:
                os.close(descriptor)
        if settings.root:
            descriptor = tree.open("root/ramdisk-root.img")
            try:
                require(
                    4096 <= os.fstat(descriptor).st_size <= 256 * 1024**2,
                    "The rooted phone's ramdisk is missing or invalid.",
                )
            finally:
                os.close(descriptor)
            from .rooting import Root

            Root(SimpleNamespace(folder=tree.root, paths=self.paths), self.cancel).verify()
        return settings

    def _inventory(self, home, slot, report):
        records = {}
        for prefix, root in (("desktop", slot), ("Android", home)):
            with Tree(root) as tree:
                entries = data_entries(tree) if prefix == "Android" else tree.entries()
                for index, entry in enumerate(entries):
                    check_cancel(self.cancel)
                    descriptor = tree.open(entry.name)
                    try:
                        records[prefix + "/" + entry.name] = {
                            "bytes": entry.size,
                            "sha256": hash_descriptor(descriptor, self.cancel),
                        }
                    finally:
                        os.close(descriptor)
                    report(
                        "Sealing recovered phone", 0.85 + 0.14 * (index + 1) / max(1, len(entries))
                    )
                tree.sync([entry.name for entry in entries])
        return records

    def prepare(self, backup, report=lambda _message, _fraction: None, *, for_clone=False):
        check_cancel(self.cancel)
        progress = Progress(report)
        phone = self.phone
        with (
            lease(self.paths.locks / (phone.identity + ".maintenance")),
            lease(self.paths.locks / "sdk.lock", shared=True),
        ):
            self._cold()
            original = phone.metadata
            old_stamp = stamp(phone.folder)
            with self._avd_lock(phone.avd):
                value = self.vault.verify(
                    backup, lambda message, fraction: progress(message, fraction * 0.2)
                )
                with Tree(self.paths.sdk / IMAGE) as image:
                    require(
                        self.vault._bases(
                            image,
                            Progress(lambda message, fraction: progress(message, 0.2 + fraction)),
                        )
                        == value["bases"],
                        "This backup needs a different Android base image. The original phone is unchanged.",
                    )
                    token = uuid.uuid4().hex
                    folder, slot = self.location(token)
                    os.mkdir(folder, 0o700)
                    published = False
                    slot_created = False
                    try:
                        os.mkdir(slot, 0o700)
                        slot_created = True
                        home = private_directory(folder / "Android")
                        avd_path = private_directory(home / "avd/Gaming_Phone.avd")
                        total = sum(row["bytes"] for row in value["files"].values()) or 1
                        with (
                            Tree(Path(backup)) as source,
                            Tree(home, writable=True) as data,
                            Tree(slot, writable=True) as desktop,
                        ):
                            entries = source.entries()
                            require(
                                {entry.name for entry in entries}
                                == set(value["files"]) | {"manifest.json"},
                                "The backup changed during preparation.",
                            )
                            allocated = sum(entry.allocated for entry in entries)
                            require(
                                shutil.disk_usage(folder).free >= allocated + 256 * 1024**2,
                                "Free more space before preparing recovery.",
                            )
                            completed = 0
                            with ReadLocks(source, entries):
                                for entry in entries:
                                    if entry.name == "manifest.json":
                                        continue
                                    check_cancel(self.cancel)
                                    require(
                                        entry.size == value["files"][entry.name]["bytes"],
                                        "A backup file changed during preparation.",
                                    )
                                    kind, name = entry.name.split("/", 1)
                                    destination, target = (
                                        (data, "avd/Gaming_Phone.avd/" + name)
                                        if kind == "avd"
                                        else (desktop, name)
                                    )
                                    digest = copy_file(
                                        source,
                                        destination,
                                        entry,
                                        target=target,
                                        cancel=self.cancel,
                                        progress=lambda count, done=completed: progress(
                                            "Preparing recovered phone",
                                            0.3 + 0.4 * (done + count) / total,
                                        ),
                                    )
                                    require(
                                        digest == value["files"][entry.name]["sha256"],
                                        "A backup file changed during preparation.",
                                    )
                                    completed += entry.size
                            settings = self._validate_desktop(desktop)
                            if for_clone:
                                settings.auto_start = False
                                settings.number = ""
                                atomic_json(slot / "settings.json", settings.as_dict())
                            # Cached host/guest revisions must be negotiated on the restored guest.
                            (slot / "experience.json").unlink(missing_ok=True)
                        with Tree(avd_path, writable=True) as avd:
                            names = {name[4:] for name in value["files"] if name.startswith("avd/")}
                            capacity = 0
                            for name in sorted(names):
                                virtual = disks.rebase(
                                    avd,
                                    name,
                                    value["source"]["avd"],
                                    value["source"]["image"],
                                    image,
                                    names,
                                    BASES,
                                )
                                if name.startswith("userdata-qemu.img"):
                                    capacity = max(
                                        capacity, virtual or value["files"]["avd/" + name]["bytes"]
                                    )
                            descriptor = image.open("build.prop")
                            try:
                                properties = os.pread(descriptor, 1024 * 1024, 0).decode("utf-8")
                            finally:
                                os.close(descriptor)
                            incremental = re.search(
                                r"^ro.build.version.incremental=([0-9]{1,20})\s*$", properties, re.M
                            )
                            require(
                                incremental is not None,
                                "The Android base build cannot be identified.",
                            )
                            atomic_bytes(avd_path / "qemu-version.txt", b"2")
                            atomic_bytes(
                                avd_path / "version_num.cache", incremental.group(1).encode()
                            )
                            cpu, ram = allocations(settings, phone.hub.hardware)
                            phone._configure(settings, cpu, ram, home=home)
                            config = avd_path / "config.ini"
                            text = config.read_text(encoding="utf-8")
                            text = re.sub(
                                r"^disk.dataPartition.size=.*$",
                                f"disk.dataPartition.size={(capacity + 1024**2 - 1) // 1024**2}M",
                                text,
                                flags=re.M,
                            )
                            atomic_bytes(config, text.encode())
                            tool = phone.hub.setup.verify_executable("emulator/qemu-img")
                            with AvdLock(avd_path):
                                for name in sorted(names):
                                    descriptor = avd.open(name)
                                    try:
                                        header = disks.inspect(descriptor)
                                    finally:
                                        os.close(descriptor)
                                    if header:
                                        check_cancel(self.cancel)
                                        progress("Checking recovered disks", 0.75, force=True)
                                        bounded_process(
                                            [
                                                str(tool),
                                                "check",
                                                "-r",
                                                "leaks",
                                                "-f",
                                                "qcow2",
                                                str(avd_path / name),
                                            ],
                                            environment=phone.environment(),
                                            timeout=180,
                                            maximum=2 * 1024**2,
                                            cancel=self.cancel,
                                        )
                        metadata = dict(
                            original,
                            home=str(home),
                            personalize=False,
                            auto_plan="",
                            recovery=token,
                        )
                        atomic_json(slot / "phone.json", metadata)
                        atomic_json(slot / "snapshot.json", {"clean": False})
                        records = self._inventory(home, slot, progress)
                        record = {
                            "format": "velox-linux-recovery",
                            "version": 1,
                            "id": token,
                            "phone": phone.identity,
                            "root": str(self.paths.root),
                            "created": int(time.time()),
                            "label": value["label"],
                            "rooted": settings.root,
                            "phase": "prepared",
                            "original": original,
                            "old_directory": old_stamp,
                            "new_directory": stamp(slot),
                            "bases": value["bases"],
                            "files": records,
                        }
                        check_cancel(self.cancel)
                        self._write(record)
                        self.record(token)
                        published = True
                    finally:
                        if not published:
                            # Only these two fresh nonce directories are ours to remove.
                            if slot_created and slot.exists() and not slot.is_symlink():
                                shutil.rmtree(slot)
                            if folder.exists() and not folder.is_symlink():
                                shutil.rmtree(folder)
                    progress("Recovered phone is ready for review", 1, force=True)
                    return token

    def verify(self, token, report=lambda _message, _fraction: None):
        value = self.record(token)
        require(
            value["phase"] == "prepared",
            "Only an inactive prepared phone can be verified for activation.",
        )
        folder, slot = self.location(token)
        home = folder / "Android"
        require(stamp(slot) == value["new_directory"], "The prepared desktop changed.")
        progress = Progress(report)
        total = sum(row["bytes"] for row in value["files"].values()) or 1
        completed, seen = 0, set()
        for prefix, root in (("desktop", slot), ("Android", home)):
            with Tree(root) as tree:
                entries = data_entries(tree) if prefix == "Android" else tree.entries()
                with ReadLocks(tree, entries):
                    for entry in entries:
                        check_cancel(self.cancel)
                        key = prefix + "/" + entry.name
                        expected = value["files"].get(key)
                        require(
                            expected is not None and entry.size == expected["bytes"],
                            "Prepared recovery files changed after review.",
                        )
                        descriptor = tree.open(entry.name)
                        try:
                            require(
                                hash_descriptor(descriptor, self.cancel) == expected["sha256"],
                                "A prepared recovery file failed its seal.",
                            )
                        finally:
                            os.close(descriptor)
                        completed += entry.size
                        seen.add(key)
                        progress("Verifying prepared recovery", completed / total * 0.9)
                if prefix == "desktop":
                    self._validate_desktop(tree)
                    metadata = read_value(tree, "phone.json", 65536)
                    require(
                        metadata
                        == dict(
                            value["original"],
                            home=str(home),
                            personalize=False,
                            auto_plan="",
                            recovery=token,
                        ),
                        "The prepared phone registration changed.",
                    )
        require(seen == set(value["files"]), "A prepared recovery file is missing.")
        with Tree(self.paths.sdk / IMAGE) as image:
            require(
                self.vault._bases(
                    image, Progress(lambda message, fraction: progress(message, 0.9 + fraction))
                )
                == value["bases"],
                "The Android base changed after recovery was prepared.",
            )
        progress("Prepared recovery verified", 1, force=True)
        return value

    def activate(self, token, report=lambda _message, _fraction: None):
        check_cancel(self.cancel)
        progress = Progress(report)
        folder, slot = self.location(token)
        phone = self.phone
        with (
            lease(self.paths.locks / (phone.identity + ".maintenance")),
            lease(self.paths.locks / "sdk.lock", shared=True),
        ):
            self._cold()
            with self._avd_lock(phone.avd), AvdLock(folder / "Android/avd/Gaming_Phone.avd"):
                value = self.verify(
                    token, lambda message, fraction: progress(message, fraction * 0.97)
                )
                with (
                    lease(self.paths.locks / "catalog.lock", blocking=True),
                    self._desktop_lock(slot),
                    Tree(self.paths.phones, writable=True) as parent,
                ):
                    require(
                        phone.metadata == value["original"]
                        and stamp(phone.folder) == value["old_directory"],
                        "The selected phone changed. Prepare recovery again before replacing it.",
                    )
                    require(stamp(slot) == value["new_directory"], "The prepared phone changed.")
                    check_cancel(self.cancel)
                    progress("Applying recovered phone", 0.99, force=True)
                    value["phase"] = "activating"
                    self._write(value)
                    exchange(parent, phone.identity, slot.name)
                    value["phase"] = "active"
                    self._write(value)
                progress(
                    "Recovery applied. The previous phone is retained for Undo.", 1, force=True
                )
                return value

    def _previous_avd(self, slot, original):
        home = original.get("home", "")
        if not home:
            return slot / "Android/avd/Gaming_Phone.avd"
        root = Path(home)
        require(
            root.is_absolute()
            and root.resolve() == root
            and root != self.paths.root
            and self.paths.sdk not in root.parents,
            "Invalid previous phone storage.",
        )
        private_directory(root)
        marker = read_json(root / ".velox-owner.json", {})
        require(
            marker
            == {"application": "velox", "phone": self.phone.identity, "root": str(self.paths.root)},
            "Previous storage does not belong to this phone.",
        )
        return root / "avd/Gaming_Phone.avd"

    def undo(self, report=lambda _message, _fraction: None):
        check_cancel(self.cancel)
        phone = self.phone
        with (
            lease(self.paths.locks / (phone.identity + ".maintenance")),
            lease(self.paths.locks / "sdk.lock", shared=True),
        ):
            self._cold()
            token = phone.metadata.get("recovery")
            value = self.reconcile(token)
            require(value["phase"] == "active", "This phone has no active recovery to undo.")
            _folder, slot = self.location(token)
            require(
                stamp(phone.folder) == value["new_directory"]
                and stamp(slot) == value["old_directory"],
                "The previous phone changed.",
            )
            with (
                self._avd_lock(phone.avd),
                self._avd_lock(self._previous_avd(slot, value["original"])),
            ):
                with (
                    lease(self.paths.locks / "catalog.lock", blocking=True),
                    self._desktop_lock(slot),
                    Tree(self.paths.phones, writable=True) as parent,
                ):
                    require(
                        read_json(slot / "phone.json", {}) == value["original"],
                        "The original phone registration changed.",
                    )
                    check_cancel(self.cancel)
                    value["phase"] = "undoing"
                    self._write(value)
                    exchange(parent, phone.identity, slot.name)
                    value["phase"] = "undone"
                    self._write(value)
            report("Previous phone restored. The recovered copy is also retained.", 1)
            return value

    def list(self):
        result = []
        for folder in self.root.iterdir():
            if (
                not TOKEN.fullmatch(folder.name)
                or folder.is_symlink()
                or not folder.is_dir()
                or not (folder / "prepared.json").exists()
            ):
                continue
            require(len(result) < 1000, "This recovery folder contains too many prepared copies.")
            try:
                value = self.record(folder.name)
            except (OSError, VeloxError):
                continue
            result.append(value)
        return sorted(result, key=lambda row: row["created"], reverse=True)

    def before_start(self):
        self.cancel = self.vault.cancel = self.phone.cancel
        token = self.phone.metadata.get("recovery")
        if not token:
            return
        value = self.reconcile(token)
        folder, _slot = self.location(token)
        home = self.phone.home
        if home != folder / "Android":
            from .relocation import Relocation
            from .storage import Storage

            home = Relocation(Storage(self.phone, self.cancel)).origin()
        require(
            value["phase"] == "active"
            and stamp(self.phone.folder) == value["new_directory"]
            and home == folder / "Android",
            "Complete or undo recovery before starting this phone.",
        )
        with Tree(self.paths.sdk / IMAGE) as image:
            require(
                self.vault._bases(image, Progress(lambda _message, _fraction: None))
                == value["bases"],
                "The recovered phone needs its original Android base image. Its data has been retained.",
            )


def reconcile_all(hub):
    """An atomic exchange leaves both directories intact even across a crash."""
    root = hub.paths.vault / "Prepared"
    errors = {}
    if not root.exists():
        return errors
    private_directory(root)
    count = 0
    for folder in root.iterdir():
        if not TOKEN.fullmatch(folder.name) or folder.is_symlink() or not folder.is_dir():
            continue
        count += 1
        require(count <= 1000, "This recovery folder contains too many copies.")
        try:
            with Tree(folder) as tree:
                value = read_value(tree, "prepared.json")
            if not isinstance(value, dict) or value.get("phase") not in ("activating", "undoing"):
                continue
            identity = value.get("phone")
            require(
                isinstance(identity, str) and INSTANCE.fullmatch(identity),
                "Invalid recovery phone identity.",
            )
            with lease(hub.paths.locks / (identity + ".maintenance")):
                Recovery(hub.phone(identity)).reconcile(folder.name)
        except (OSError, VeloxError) as error:
            errors[folder.name] = str(error)
    return errors
