"""Opt-in Magisk setup inside one owned stock phone; shared SDK files stay intact."""

from __future__ import annotations

import contextlib
import os
from pathlib import Path
import re
import shlex
import tempfile
import uuid

from .core import VeloxError, atomic_json, file_hash, lease, private_directory, read_json, regular
from .fileops import Tree, copy_file, check_cancel
from .resources import checked
from .setup import download
from .vault import IMAGE, require


MAGISK = {
    "name": "Magisk 31.1",
    "url": "https://github.com/topjohnwu/Magisk/releases/download/v31.1/Magisk-v31.1.apk",
    "bytes": 10679283,
    "sha256": "89cefac0a318afa39fb3880c45c87b036c05bc1ebf02a7a715b7f8d7c26ac6e7",
}


class Root:
    def __init__(self, phone, cancel=None):
        import threading

        self.phone, self.paths = phone, phone.paths
        self.cancel = cancel or threading.Event()
        self.root = phone.folder / "root"
        self.image = self.root / "ramdisk-root.img"

    def available(self):
        try:
            if self.root.is_symlink():
                return False
            info = regular(self.image, maximum=256 * 1024**2, owner=True)
            state = regular(self.root / "root.json", maximum=65536, owner=True)
            value = read_json(self.root / "root.json", {}, 65536)
            return (
                info.st_nlink == 1
                and info.st_size >= 100000
                and state.st_nlink == 1
                and isinstance(value, dict)
                and value.get("format") == "velox-linux-root"
                and type(value.get("version")) is int
                and value["version"] == 1
                and value.get("magisk") == MAGISK["sha256"]
                and all(
                    isinstance(value.get(key), str) and re.fullmatch(r"[a-f0-9]{64}", value[key])
                    for key in ("stock", "ramdisk")
                )
            )
        except (OSError, VeloxError):
            return False

    def su(self, command, *, timeout=8):
        paths = ("/debug_ramdisk/su", "/sbin/su", "/system/xbin/su", "/system/bin/su")
        binary = self.phone.shell(
            "for p in " + " ".join(paths) + '; do if [ -x "$p" ]; then echo "$p"; break; fi; done',
            timeout=4,
        )
        require(
            binary in paths, "Root is not active in this boot. Prepare Root and restart the phone."
        )
        return self.phone.shell(
            shlex.quote(binary) + " -c " + shlex.quote(command), timeout=timeout
        )

    def open_manager(self):
        text = self.phone.shell(
            "cmd package resolve-activity --brief -a android.intent.action.MAIN -c android.intent.category.LAUNCHER com.topjohnwu.magisk",
            timeout=8,
        )
        component = text.splitlines()[-1] if text else ""
        require(
            component.startswith("com.topjohnwu.magisk/"),
            "The Magisk manager is not installed. Prepare Root first.",
        )
        self.phone.launch(component)

    def verify(self):
        require(self.available(), "Set up Root first, or turn Root off to start normally.")
        value = read_json(self.root / "root.json", {}, 65536)
        require(
            isinstance(value, dict)
            and value.get("format") == "velox-linux-root"
            and type(value.get("version")) is int
            and value["version"] == 1
            and value.get("magisk") == MAGISK["sha256"]
            and value.get("ramdisk") == file_hash(self.image, self.cancel),
            "The prepared Root image failed its integrity check. Start without Root and prepare it again.",
        )
        require(
            value.get("stock") == file_hash(self.paths.sdk / IMAGE / "ramdisk.img", self.cancel),
            "The Android base changed. Start without Root and prepare Root again for this image.",
        )
        return self.image

    def stop_remote(self, remote, identity):
        current = self.phone.runtime()
        if (current["pid"], current["birth"]) != identity:
            return
        raw = self.phone.shell(
            "cat " + shlex.quote(remote + "/process.stat"), timeout=4, maximum=4096
        )
        pid = raw.split(" ", 1)[0]
        if not re.fullmatch(r"[1-9][0-9]{0,8}", pid):
            return
        original = raw[raw.rindex(")") + 2 :].split()
        actual = self.phone.shell("cat /proc/" + pid + "/stat", timeout=4, maximum=4096)
        observed = actual[actual.rindex(")") + 2 :].split()
        if (
            len(original) < 20
            or len(observed) < 20
            or original[19] != observed[19]
            or observed[2] != pid
            or observed[3] != pid
        ):
            return
        arguments = self.phone.adb(
            "exec-out", "cat", "/proc/" + pid + "/cmdline", timeout=4, maximum=16384
        ).split(b"\0")
        if (remote + "/patch-ramdisk.sh").encode() in arguments:
            # Only this isolated session: verified phone, PID birth, argv and PGID.
            self.phone.shell("toybox kill -TERM -- -" + pid, timeout=4)

    def prepare(self, report=lambda _message, _fraction: None):
        phone = self.phone
        check_cancel(self.cancel)
        with (
            lease(self.paths.locks / (phone.identity + ".maintenance")),
            lease(self.paths.locks / "sdk.lock", shared=True),
        ):
            runtime = phone.runtime()
            require(
                phone.state == "running" and not runtime.get("settings", {}).get("root"),
                "Start this phone without Root before preparing Root.",
            )
            identity = (runtime["pid"], runtime["birth"])
            script = checked("patch-ramdisk.sh", 65536)
            cache = private_directory(self.paths.cache / "root-tools")
            apk = cache / "Magisk-v31.1.apk"
            report("Downloading verified Root tools", 0)
            download(
                MAGISK,
                apk,
                lambda _name, fraction: report("Downloading verified Root tools", fraction * 0.2),
                self.cancel,
            )
            private_directory(self.root)
            remote = "/data/data/com.android.shell/VeloxRoot-" + uuid.uuid4().hex
            with tempfile.TemporaryDirectory(prefix="root-", dir=self.paths.cache) as temporary:
                temporary = Path(temporary)
                with (
                    Tree(self.paths.sdk / IMAGE) as source,
                    Tree(temporary, writable=True) as target,
                ):
                    entries = source.entries(
                        lambda name, directory: not directory and name == "ramdisk.img"
                    )
                    require(
                        len(entries) == 1 and 4096 <= entries[0].size <= 256 * 1024**2,
                        "The stock Android ramdisk is missing or invalid.",
                    )
                    stock = copy_file(source, target, entries[0], cancel=self.cancel)
                phone.shell("mkdir " + shlex.quote(remote), timeout=5, cancel=self.cancel)
                try:
                    for source, leaf in (
                        (apk, "Magisk.zip"),
                        (script, "patch-ramdisk.sh"),
                        (temporary / "ramdisk.img", "ramdisk.img"),
                    ):
                        phone.adb(
                            "push",
                            str(source),
                            remote + "/" + leaf,
                            timeout=90,
                            maximum=1024**2,
                            cancel=self.cancel,
                        )
                    report("Preparing the private Root image", 0.3)
                    # The AVD patcher has no network or interactive channel selection.
                    command = (
                        "cd "
                        + shlex.quote(remote)
                        + " && cat /proc/$$/stat > process.stat && exec sh "
                        + shlex.quote(remote + "/patch-ramdisk.sh")
                    )
                    phone.shell(
                        "setsid sh -c " + shlex.quote(command),
                        timeout=300,
                        maximum=4 * 1024**2,
                        cancel=self.cancel,
                    )
                    result = temporary / "ramdisk-root.img"
                    phone.adb(
                        "pull",
                        remote + "/ramdiskpatched4AVD.img",
                        str(result),
                        timeout=90,
                        maximum=1024**2,
                        cancel=self.cancel,
                    )
                    info = regular(result, maximum=256 * 1024**2, owner=True)
                    require(
                        info.st_nlink == 1 and info.st_size >= 100000,
                        "The Root image could not be prepared. The shared Android image is unchanged.",
                    )
                    current = phone.runtime()
                    require(
                        (current["pid"], current["birth"]) == identity,
                        "This phone restarted during Root setup. Prepare Root again.",
                    )
                    report("Installing the Magisk manager", 0.85)
                    output = phone.adb(
                        "install", "-r", str(apk), timeout=120, maximum=1024**2, cancel=self.cancel
                    )
                    require(
                        b"Success" in output,
                        "The Magisk manager could not be installed. Root has not been enabled.",
                    )
                    check_cancel(self.cancel)
                    digest = file_hash(result, self.cancel)
                    with (
                        Tree(temporary) as source,
                        Tree(private_directory(self.root), writable=True) as target,
                    ):
                        entry = next(
                            entry for entry in source.entries() if entry.name == "ramdisk-root.img"
                        )
                        leaf = "ramdisk-" + uuid.uuid4().hex + ".new"
                        copy_file(source, target, entry, target=leaf, cancel=self.cancel)
                        os.replace(self.root / leaf, self.image)
                        target.sync(["ramdisk-root.img"])
                    atomic_json(
                        self.root / "root.json",
                        dict(
                            format="velox-linux-root",
                            version=1,
                            magisk=MAGISK["sha256"],
                            stock=stock,
                            ramdisk=digest,
                        ),
                    )
                    self.verify()
                finally:
                    with contextlib.suppress(OSError, VeloxError, ValueError, IndexError):
                        self.stop_remote(remote, identity)
                    with contextlib.suppress(OSError, VeloxError):
                        phone.shell("rm -rf " + shlex.quote(remote), timeout=8)
            report("Root is ready. Turn on Root and restart the phone.", 1)
            return str(self.image)
