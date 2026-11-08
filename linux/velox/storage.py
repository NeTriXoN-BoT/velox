"""Cold phone copies and reversible removal; shared SDK data is never moved."""

from __future__ import annotations

import contextlib
import os
from pathlib import Path
import re
import tempfile
import threading
import time
import uuid

from .avdlock import AvdLock
from .core import (
    INSTANCE,
    Settings,
    VeloxError,
    atomic_json,
    lease,
    private_directory,
    read_json,
    safe_name,
    validate_metadata,
)
from .fileops import Tree, check_cancel
from .recovery import Recovery, stamp
from .vault import Vault, require


TOKEN = re.compile(r"[a-f0-9]{32}\Z")


class Storage:
    def __init__(self, phone, cancel=None):
        self.phone, self.hub, self.paths = phone, phone.hub, phone.paths
        self.cancel = cancel or threading.Event()
        self.removed = private_directory(self.paths.root / "Removed")

    def cold(self):
        require(
            not self.phone.alive() and self.phone.state == "stopped",
            "Stop this phone before managing its storage.",
        )

    @contextlib.contextmanager
    def avd_lock(self):
        if self.phone.avd.exists():
            with AvdLock(self.phone.avd):
                yield
        else:
            yield

    def usage(self):
        result = {"home": str(self.phone.home), "phone": 0, "allocated": 0, "desktop": 0, "logs": 0}
        if self.phone.home.is_dir():
            with Tree(self.phone.home) as tree:
                for entry in tree.entries():
                    check_cancel(self.cancel)
                    result["phone"] += entry.size
                    result["allocated"] += entry.allocated
        with Tree(self.phone.folder) as tree:
            entries = tree.entries(lambda name, directory: name != "Android" if directory else True)
            for entry in entries:
                check_cancel(self.cancel)
                result["desktop"] += entry.allocated
                if entry.name in ("emulator.log", "emulator.log.1"):
                    result["logs"] += entry.allocated
        from .relocation import Relocation

        result["move"] = Relocation(self).active()
        return result

    def relocate(self, parent, report=lambda _message, _fraction: None):
        from .relocation import Relocation

        return Relocation(self).move(parent, report)

    def undo_relocation(self, report=lambda _message, _fraction: None):
        from .relocation import Relocation

        return Relocation(self).undo(report)

    def clear_logs(self, report=lambda _message, _fraction: None):
        with lease(self.paths.locks / (self.phone.identity + ".maintenance")):
            self.cold()
            with self.avd_lock(), Tree(self.phone.folder, writable=True) as tree:
                for name in ("emulator.log", "emulator.log.1"):
                    check_cancel(self.cancel)
                    try:
                        descriptor = tree.open(name)
                    except FileNotFoundError:
                        continue
                    try:
                        # Delete only the opened ordinary file, never a substituted link.
                        tree.unlink_created(name, descriptor)
                    finally:
                        os.close(descriptor)
                tree.sync()
            report("Old startup logs cleared. Apps, saves and captures are kept.", 1)

    def clone(self, name, report=lambda _message, _fraction: None, *, full=True):
        check_cancel(self.cancel)
        self.cold()
        name = safe_name(name)
        if not full:
            return self.hub.create(name, template=self.phone.identity)
        # The same verified cold-copy and recovery path handles QCOW chains,
        # encryption keys, rooted ramdisks and pending wallpapers in one place.
        with tempfile.TemporaryDirectory(prefix=".clone-", dir=self.paths.cache) as temporary:
            source = Vault(self.phone)
            source.root, source.cancel = Path(temporary), self.cancel
            backup = source.create(lambda message, fraction: report(message, fraction * 0.35))
            check_cancel(self.cancel)
            identity = self.hub.create(name)
            target = self.hub.phone(identity)
            manager = Recovery(target)
            manager.cancel = manager.vault.cancel = self.cancel
            # If an action fails after registration, retain the named target.
            # Never roll back a directory which may already contain a restore.
            prepared = manager.prepare(
                backup,
                lambda message, fraction: report(message, 0.35 + fraction * 0.55),
                for_clone=True,
            )
            manager.activate(
                prepared, lambda message, fraction: report(message, 0.9 + fraction * 0.1)
            )
            report("Phone cloned. Its apps and saves are independent.", 1)
            return identity

    def remove(self, report=lambda _message, _fraction: None):
        phone = self.phone
        require(phone.identity != "main", "The main phone cannot be removed.")
        check_cancel(self.cancel)
        with (
            lease(self.paths.locks / (phone.identity + ".maintenance")),
            lease(self.paths.locks / "catalog.lock", blocking=True),
        ):
            self.cold()
            metadata = phone.metadata
            settings = self.paths.settings(phone.identity)
            Settings.from_dict(settings.as_dict())
            # Refuse links/special files before retaining the whole desktop tree.
            with self.avd_lock(), Tree(phone.folder) as source:
                source.entries()
                token = uuid.uuid4().hex
                folder = self.removed / token
                os.mkdir(folder, 0o700)
                value = {
                    "format": "velox-linux-removed",
                    "version": 1,
                    "token": token,
                    "phone": phone.identity,
                    "metadata": metadata,
                    "created": int(time.time()),
                    "directory": stamp(phone.folder),
                    "root": str(self.paths.root),
                    "phase": "removing",
                }
                atomic_json(folder / "removed.json", value)
                with (
                    Tree(self.paths.phones, writable=True) as parent,
                    Tree(folder, writable=True) as destination,
                ):
                    require(
                        stamp(phone.folder) == value["directory"],
                        "Phone storage changed during removal.",
                    )
                    check_cancel(self.cancel)
                    os.rename(
                        phone.identity,
                        "phone",
                        src_dir_fd=parent.descriptor,
                        dst_dir_fd=destination.descriptor,
                    )
                    with self.hub._lock:
                        self.hub._phones.pop(phone.identity, None)
                    os.fsync(parent.descriptor)
                    os.fsync(destination.descriptor)
                value["phase"] = "removed"
                atomic_json(folder / "removed.json", value)
        report("Phone removed. Restore it from Removed phones whenever you need it.", 1)
        return token

    def removed_record(self, token):
        require(
            isinstance(token, str) and TOKEN.fullmatch(token), "Invalid removed phone identity."
        )
        folder = self.removed / token
        with Tree(folder) as tree:
            from .vault import read_value

            value = read_value(tree, "removed.json", 65536)
        require(
            isinstance(value, dict)
            and value.get("format") == "velox-linux-removed"
            and type(value.get("version")) is int
            and value["version"] == 1
            and value.get("token") == token
            and value.get("root") == str(self.paths.root)
            and isinstance(value.get("phone"), str)
            and INSTANCE.fullmatch(value["phone"])
            and value["phone"] != "main"
            and isinstance(value.get("metadata"), dict)
            and value["metadata"].get("id") == value["phone"]
            and isinstance(value["metadata"].get("name"), str)
            and safe_name(value["metadata"]["name"]) == value["metadata"]["name"]
            and type(value.get("created")) is int
            and 0 < value["created"] <= 4102444800
            and isinstance(value.get("directory"), list)
            and len(value["directory"]) == 2
            and all(type(number) is int and number >= 0 for number in value["directory"])
            and value.get("phase") in ("removing", "removed", "restoring", "restored"),
            "This removed phone record is damaged.",
        )
        validate_metadata(value["phone"], value["metadata"])
        if "planned" in value:
            validate_metadata(value["phone"], value["planned"])
            require(
                {key: item for key, item in value["planned"].items() if key not in ("port", "grpc")}
                == {
                    key: item
                    for key, item in value["metadata"].items()
                    if key not in ("port", "grpc")
                },
                "The removed phone registration changed.",
            )
        return value

    def list_removed(self):
        result = []
        for folder in self.removed.iterdir():
            if not TOKEN.fullmatch(folder.name) or folder.is_symlink() or not folder.is_dir():
                continue
            require(
                len(result) < 1000,
                "Too many removed phones. Open the storage folder to review them.",
            )
            try:
                value = self.removed_record(folder.name)
                if (folder / "phone").exists() and stamp(folder / "phone") == value["directory"]:
                    result.append(value)
            except (OSError, ValueError, VeloxError) as error:
                result.append({"token": folder.name, "created": 0, "error": str(error)})
        return sorted(result, key=lambda value: value["created"], reverse=True)

    def restore_removed(self, token, report=lambda _message, _fraction: None):
        value = self.removed_record(token)
        identity = value["phone"]
        with (
            lease(self.paths.locks / (identity + ".maintenance")),
            lease(self.paths.locks / "catalog.lock", blocking=True),
        ):
            check_cancel(self.cancel)
            value = self.removed_record(token)
            folder = self.removed / token
            require(
                not self.paths.phone(identity).exists(),
                "A phone with this identity already exists.",
            )
            require(stamp(folder / "phone") == value["directory"], "Removed phone storage changed.")
            require(
                read_json(folder / "phone/phone.json", {})
                in (value["metadata"], value.get("planned")),
                "The removed phone registration changed.",
            )
            Settings.from_dict(read_json(folder / "phone/settings.json", {}))
            with Tree(folder / "phone") as tree:
                tree.entries()
            # A newer phone may have taken this port while the original was removed.
            used = {item["port"] for item in self.hub.list() if not item.get("error")}
            metadata = dict(value["metadata"])
            port = metadata.get("port")
            if (
                type(port) is not int
                or port not in range(5556, 5684, 2)
                or port in used
                or not self.hub._ports_free(port)
            ):
                port = next(
                    (
                        port
                        for port in range(5556, 5684, 2)
                        if port not in used and self.hub._ports_free(port)
                    ),
                    None,
                )
            require(port is not None, "No free phone connection ports are available.")
            metadata.update(port=port, grpc=port + 1000)
            self.paths.data_home(identity, metadata=metadata)
            # Keep the receipt consistent if power fails after a port repair.
            value.update(planned=metadata, phase="restoring")
            atomic_json(folder / "removed.json", value)
            atomic_json(folder / "phone/phone.json", metadata)
            with (
                Tree(folder, writable=True) as source,
                Tree(self.paths.phones, writable=True) as destination,
            ):
                check_cancel(self.cancel)
                require(
                    stamp(folder / "phone") == value["directory"], "Removed phone storage changed."
                )
                os.rename(
                    "phone",
                    identity,
                    src_dir_fd=source.descriptor,
                    dst_dir_fd=destination.descriptor,
                )
                os.fsync(source.descriptor)
                os.fsync(destination.descriptor)
            value["phase"] = "restored"
            atomic_json(folder / "removed.json", value)
        report("Phone restored. Its apps and saves are ready.", 1)
        return identity
