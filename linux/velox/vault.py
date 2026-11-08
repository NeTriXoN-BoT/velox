"""Cold, checksum-verified phone backups in private directories."""

from __future__ import annotations

import datetime
import json
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import re
import shutil
import tempfile
import threading
import time
import uuid

from . import disks
from .core import (
    INSTANCE,
    PACKAGE,
    Settings,
    VeloxError,
    atomic_json,
    lease,
    private_directory,
    safe_name,
)
from .avdlock import AvdLock
from .fileops import (
    MAX_FILE,
    MAX_TREE,
    ReadLocks,
    Tree,
    check_cancel,
    copy_file,
    hash_descriptor,
    identity,
    relative_name,
)


BASES = frozenset(("system.img", "vendor.img", "ramdisk.img", "kernel-ranchu", "build.prop"))
DISKS = frozenset(
    (
        "cache.img",
        "cache.img.qcow2",
        "userdata-qemu.img",
        "userdata-qemu.img.qcow2",
        "encryptionkey.img",
        "encryptionkey.img.qcow2",
        "sdcard.img",
        "sdcard.img.qcow2",
        "system.img.qcow2",
        "vendor.img.qcow2",
    )
)
IMAGE = "system-images/android-35/google_apis_playstore/x86_64"
HEX = re.compile(r"[a-f0-9]{64}\Z")
SCHEDULE_ID = re.compile(r"[a-f0-9]{32}\Z")


def require(value, message):
    if not value:
        raise VeloxError(message)


def desktop_file(name):
    try:
        parts = relative_name(name)
    except VeloxError:
        return False
    if len(parts) == 1:
        return name in (
            "settings.json",
            "experience.json",
            "wallpaper-current.json",
            "wallpaper-pending.json",
        )
    if len(parts) != 2:
        return False
    folder, leaf = parts
    if folder == "controls":
        return leaf.endswith(".json") and PACKAGE.fullmatch(leaf[:-5]) is not None
    if folder == "wallpapers":
        return leaf.endswith(".png") and len(leaf.encode("utf-8")) <= 300
    return folder == "root" and leaf in ("ramdisk-root.img", "root.json")


def allowed(name):
    parts = relative_name(name)
    if parts[0] == "avd":
        return len(parts) == 2 and parts[1] in DISKS
    return parts[0] == "desktop" and len(parts) > 1 and desktop_file("/".join(parts[1:]))


def maximum(name):
    if name.startswith("avd/"):
        return MAX_FILE
    if name == "desktop/root/ramdisk-root.img":
        return 256 * 1024**2
    if name.startswith("desktop/wallpapers/"):
        return 32 * 1024**2
    return 2 * 1024**2 if name == "desktop/experience.json" else 65536


def unique(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "The backup repeats a manifest field.")
        result[key] = value
    return result


def read_value(tree, name, limit=2 * 1024**2):
    descriptor = tree.open(name)
    try:
        before = os.fstat(descriptor)
        require(before.st_size <= limit, "The backup metadata is too large.")
        value = os.pread(descriptor, limit + 1, 0)
        require(
            len(value) <= limit and identity(before) == identity(os.fstat(descriptor)),
            "The backup metadata changed during reading.",
        )
        try:
            return json.loads(
                value,
                object_pairs_hook=unique,
                parse_constant=lambda _: (_ for _ in ()).throw(ValueError("Nonfinite value")),
            )
        except (ValueError, UnicodeError, RecursionError) as error:
            raise VeloxError("The backup metadata is damaged.") from error
    finally:
        os.close(descriptor)


def original_path(value):
    require(isinstance(value, str) and 0 < len(value) <= 4096, "Invalid original backup path.")
    require(
        not any(ord(char) < 32 or ord(char) == 127 for char in value),
        "Invalid original backup path.",
    )
    require(
        PurePosixPath(value).is_absolute() or PureWindowsPath(value).is_absolute(),
        "Original backup paths must be absolute.",
    )
    return value


def manifest(tree):
    value = read_value(tree, "manifest.json")
    require(
        isinstance(value, dict)
        and value.get("format") == "velox-linux-backup"
        and type(value.get("version")) is int
        and value["version"] == 1,
        "This backup format is not supported by this Linux build.",
    )
    require(
        isinstance(value.get("label"), str) and safe_name(value["label"]) == value["label"],
        "Invalid backup label.",
    )
    require(
        type(value.get("created")) is int and 0 < value["created"] <= 4102444800,
        "Invalid backup date.",
    )
    require(isinstance(value.get("source"), dict), "Invalid original phone information.")
    original_path(value["source"].get("avd"))
    original_path(value["source"].get("image"))
    bases = value.get("bases")
    require(
        isinstance(bases, dict) and set(bases) == BASES,
        "The backup must identify all five Android base files.",
    )
    for row in bases.values():
        require(
            isinstance(row, dict)
            and type(row.get("bytes")) is int
            and 0 < row["bytes"] <= MAX_FILE
            and isinstance(row.get("sha256"), str)
            and HEX.fullmatch(row["sha256"])
            and row.get("format") in ("raw", "qcow2"),
            "Invalid Android base fingerprint.",
        )
    files = value.get("files")
    require(isinstance(files, dict) and 1 <= len(files) <= 3000, "Invalid backup file inventory.")
    require(
        {"desktop/settings.json", "avd/userdata-qemu.img", "avd/encryptionkey.img"} <= set(files),
        "The backup is missing phone data, encryption keys or preferences.",
    )
    seen, total = set(), 0
    for name, row in files.items():
        require(
            allowed(name) and name.casefold() not in seen,
            "The backup contains an unsupported or repeated file.",
        )
        require(
            isinstance(row, dict)
            and type(row.get("bytes")) is int
            and 0 <= row["bytes"] <= maximum(name)
            and isinstance(row.get("sha256"), str)
            and HEX.fullmatch(row["sha256"]),
            "Invalid backup file fingerprint.",
        )
        seen.add(name.casefold())
        total += row["bytes"]
    require(total <= MAX_TREE, "The backup is too large.")
    require(
        files["avd/userdata-qemu.img"]["bytes"] > 0 and files["avd/encryptionkey.img"]["bytes"] > 0,
        "The phone data or encryption keys are empty.",
    )
    if "schedule" in value:
        scheduled = value["schedule"]
        require(
            isinstance(scheduled, dict)
            and set(scheduled) == {"id", "phone", "root"}
            and isinstance(scheduled["id"], str)
            and SCHEDULE_ID.fullmatch(scheduled["id"])
            and isinstance(scheduled["phone"], str)
            and INSTANCE.fullmatch(scheduled["phone"]),
            "Invalid scheduled backup identity.",
        )
        original_path(scheduled["root"])
    return value


class Progress:
    def __init__(self, callback):
        self.callback = callback
        self.previous = 0.0

    def __call__(self, message, fraction, force=False):
        current = time.monotonic()
        if force or current - self.previous >= 0.1:
            self.previous = current
            self.callback(message, max(0.0, min(1.0, fraction)))


class Vault:
    def __init__(self, phone):
        self.phone, self.paths = phone, phone.paths
        self.root = private_directory(self.paths.vault / "Backups")
        self.cancel = threading.Event()

    def _bases(self, image, report):
        entries = image.entries(lambda name, directory: not directory and name in BASES)
        require(
            {entry.name for entry in entries} == BASES,
            "Android setup is missing a required base file.",
        )
        records = {}
        for index, entry in enumerate(entries):
            check_cancel(self.cancel)
            descriptor = image.open(entry.name)
            try:
                header = disks.inspect(descriptor)
                require(
                    header is None or not header.backing,
                    "Android base files must not reference external images.",
                )
                cache = getattr(self.phone.hub.setup, "_base_hashes", {})
                key = (str(image.root), entry.name)
                stamp = identity(os.fstat(descriptor))
                require(stamp == entry.identity, "An Android base changed before verification.")
                cached = cache.get(key)
                digest = (
                    cached[1]
                    if cached and cached[0] == stamp
                    else hash_descriptor(descriptor, self.cancel)
                )
                require(
                    identity(os.fstat(descriptor)) == stamp,
                    "An Android base changed during verification.",
                )
                cache[key] = (stamp, digest)
                self.phone.hub.setup._base_hashes = cache
                records[entry.name] = {
                    "bytes": entry.size,
                    "sha256": digest,
                    "format": "qcow2" if header else "raw",
                }
            finally:
                os.close(descriptor)
            report("Checking Android base files", (index + 1) / len(entries) * 0.1, force=True)
        return records

    def _disk_inventory(self, tree, value):
        names = {name[4:] for name in value["files"] if name.startswith("avd/")}
        headers = {}
        for name in names:
            descriptor = tree.open("avd/" + name)
            try:
                headers[name] = disks.inspect(descriptor)
                require(
                    headers[name] is not None or not name.endswith(".qcow2"),
                    "A backup QCOW disk has an invalid header.",
                )
            finally:
                os.close(descriptor)
        for name, header in headers.items():
            if header is None or not header.backing:
                continue
            require(
                name.endswith(".qcow2"), "Only known phone overlays may reference backing images."
            )
            kind, target = disks.backing_target(
                header.backing, value["source"]["avd"], value["source"]["image"], names, BASES
            )
            require(
                PurePosixPath(target).name == name[:-6],
                "An overlay references the wrong backing image.",
            )
            base = headers.get(target) if kind == "phone" else None
            base_format = (
                ("qcow2" if base else "raw")
                if kind == "phone"
                else value["bases"][target]["format"]
            )
            require(
                base_format == header.format and (base is None or not base.backing),
                "Alternate disk backing chains are not allowed.",
            )

    def verify(self, path, report=lambda _message, _fraction: None):
        check_cancel(self.cancel)
        progress = Progress(report)
        with Tree(Path(path)) as source:
            value = manifest(source)
            entries = source.entries()
            require(
                {entry.name for entry in entries} == set(value["files"]) | {"manifest.json"},
                "The backup has missing or unlisted files.",
            )
            total = sum(row["bytes"] for row in value["files"].values()) or 1
            completed = 0
            with ReadLocks(source, entries):
                for entry in entries:
                    if entry.name == "manifest.json":
                        continue
                    check_cancel(self.cancel)
                    expected = value["files"][entry.name]
                    require(entry.size == expected["bytes"], "A backup file has the wrong size.")
                    descriptor = source.open(entry.name)
                    try:
                        require(
                            hash_descriptor(descriptor, self.cancel) == expected["sha256"],
                            "A backup file failed its checksum.",
                        )
                    finally:
                        os.close(descriptor)
                    completed += entry.size
                    progress("Verifying phone backup", completed / total * 0.95)
                Settings.from_dict(read_value(source, "desktop/settings.json", 65536))
                self._disk_inventory(source, value)
            progress("Backup verified", 1.0, force=True)
            return value

    def create(self, report=lambda _message, _fraction: None, *, schedule=None, permit=None):
        check_cancel(self.cancel)
        progress = Progress(report)
        phone = self.phone
        with (
            lease(self.paths.locks / (phone.identity + ".maintenance")),
            lease(self.paths.locks / "sdk.lock", shared=True),
        ):
            require(
                not phone.alive() and phone.state == "stopped",
                "Stop this phone before creating a cold backup.",
            )
            require(phone.avd.is_dir(), "Start this phone once before creating a backup.")
            self.paths.settings(phone.identity)
            with (
                AvdLock(phone.avd),
                Tree(phone.avd) as avd,
                Tree(phone.folder) as desktop,
                Tree(self.paths.sdk / IMAGE) as image,
            ):
                avd_entries = avd.entries(lambda name, directory: not directory and name in DISKS)
                desktop_entries = desktop.entries(
                    lambda name, directory: (
                        name in ("controls", "wallpapers", "root")
                        if directory
                        else desktop_file(name)
                    )
                )
                selected = [(avd, entry, "avd/" + entry.name) for entry in avd_entries] + [
                    (desktop, entry, "desktop/" + entry.name) for entry in desktop_entries
                ]
                require(len(selected) <= 3000, "The phone has too many backup files.")
                require(
                    {name for _tree, _entry, name in selected}
                    >= {"avd/userdata-qemu.img", "avd/encryptionkey.img", "desktop/settings.json"},
                    "The phone's data or encryption keys are missing.",
                )
                for _tree, entry, name in selected:
                    require(
                        entry.size <= maximum(name),
                        "A phone preference or picture is too large to back up.",
                    )
                allocated = sum(entry.allocated for _tree, entry, _name in selected)
                require(
                    shutil.disk_usage(self.root).free >= allocated + 256 * 1024**2,
                    "Free more storage space before creating this backup.",
                )
                total = sum(entry.size for _tree, entry, _name in selected) or 1
                require(total <= MAX_TREE, "The phone backup is too large.")
                if permit is not None:
                    permit(total)
                created = int(time.time())
                label = phone.metadata["name"]
                name = (
                    datetime.datetime.fromtimestamp(created, datetime.timezone.utc).strftime(
                        "Velox-%Y%m%d-%H%M%S-"
                    )
                    + uuid.uuid4().hex[:12]
                    + ".vbackup"
                )
                with (
                    ReadLocks(avd, avd_entries),
                    ReadLocks(desktop, desktop_entries),
                    tempfile.TemporaryDirectory(prefix=".creating-", dir=self.root) as temporary,
                ):
                    stage = Path(temporary)
                    with Tree(stage, writable=True) as destination:
                        bases = self._bases(image, progress)
                        files, completed = {}, 0
                        for source, entry, target in selected:
                            digest = copy_file(
                                source,
                                destination,
                                entry,
                                target=target,
                                cancel=self.cancel,
                                progress=lambda count, done=completed: progress(
                                    "Copying and checking phone data",
                                    0.1 + 0.85 * (done + count) / total,
                                ),
                            )
                            files[target] = {"bytes": entry.size, "sha256": digest}
                            completed += entry.size
                        value = {
                            "format": "velox-linux-backup",
                            "version": 1,
                            "created": created,
                            "label": label,
                            "source": {"avd": str(phone.avd), "image": str(image.root)},
                            "bases": bases,
                            "files": files,
                        }
                        if schedule is not None:
                            value["schedule"] = schedule
                        atomic_json(stage / "manifest.json", value)
                        manifest(destination)
                        Settings.from_dict(read_value(destination, "desktop/settings.json", 65536))
                        self._disk_inventory(destination, value)
                        check_cancel(self.cancel)
                        destination.sync([*files, "manifest.json"])
                        final = self.root / name
                        require(not final.exists(), "A backup with this name already exists.")
                        os.rename(stage, final)
                        directory = os.open(self.root, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
                        try:
                            os.fsync(directory)
                        finally:
                            os.close(directory)
                progress("Backup created", 1.0, force=True)
                return final

    def list(self):
        result = []
        for path in self.root.iterdir():
            if not path.name.endswith(".vbackup") or path.is_symlink() or not path.is_dir():
                continue
            if len(result) >= 1000:
                raise VeloxError("This recovery folder contains too many backups.")
            try:
                with Tree(path) as tree:
                    value = manifest(tree)
                result.append(
                    {
                        "path": path,
                        "label": value["label"],
                        "created": value["created"],
                        "bytes": sum(row["bytes"] for row in value["files"].values()),
                        "error": "",
                        "schedule": value.get("schedule"),
                    }
                )
            except (OSError, VeloxError) as error:
                result.append(
                    {
                        "path": path,
                        "label": path.name,
                        "created": 0,
                        "bytes": 0,
                        "error": str(error),
                    }
                )
        return sorted(result, key=lambda row: row["created"], reverse=True)
