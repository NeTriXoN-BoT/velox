"""Native Android navigation and the guarded, optional Pixel Launcher overlay."""

from __future__ import annotations

import contextlib
import re
import shlex
import tempfile
import uuid
from pathlib import Path

from .core import VeloxError, atomic_bytes, bounded_read
from .resources import checked


LAUNCHER = "com.google.android.apps.nexuslauncher"
OVERLAY = "app.velox.navorder"
NEEDS_ROOT = "Reversed buttons need Root. Standard buttons stay available until Root is ready."
NEEDS_ACCESS = "In Magisk, open Superuser, tap + and enable Shell to use reversed buttons."
RESTART = "Restart the phone to finish setting up reversed buttons."
UNSUPPORTED = "This Android launcher is not compatible with reversed buttons. Standard buttons remain available."


def apply(phone, settings):
    runtime = phone.runtime()
    key = (runtime["pid"], runtime["birth"], settings.navigation)
    previous = getattr(phone, "_navigation_applied", None)
    root = bool(runtime.get("settings", {}).get("root"))
    if previous and previous[0] == key and previous[1] != NEEDS_ACCESS:
        phone.navigation_notice = previous[1]
        return
    notice = ""
    try:
        overlays = phone.shell("cmd overlay list " + LAUNCHER, timeout=5)
        exists = OVERLAY in overlays
        enabled = "[x] " + OVERLAY in overlays
        base = "com.android.internal.systemui.navbar." + (
            "gestural" if settings.navigation == 2 else "threebutton"
        )
        if settings.navigation == 1 and not exists:
            # Navigation is cosmetic. Missing root must never fail a phone boot.
            phone.shell("cmd overlay enable-exclusive --user current --category " + base, timeout=8)
            if not root:
                notice = NEEDS_ROOT
            elif not root_allowed(phone):
                notice = NEEDS_ACCESS
            elif not prepare(phone):
                notice = UNSUPPORTED
            else:
                notice = RESTART
        else:
            refresh = exists and enabled != (settings.navigation == 1)
            if refresh and (not root or not root_allowed(phone)):
                notice = NEEDS_ACCESS if root else NEEDS_ROOT
            else:
                command = "cmd overlay enable-exclusive --user current --category " + base
                if exists:
                    command += (
                        " && cmd overlay "
                        + ("enable" if settings.navigation == 1 else "disable")
                        + " --user current "
                        + OVERLAY
                    )
                phone.shell(command, timeout=8)
                if refresh:
                    home = LAUNCHER + "/" in phone.shell(
                        "dumpsys window | grep -m1 mCurrentFocus", timeout=5
                    )
                    # Refresh only the known Pixel Launcher, never the foreground game.
                    from .rooting import Root

                    Root(phone).su("killall " + LAUNCHER, timeout=5)
                    if home:
                        phone.key(3)
    except (OSError, VeloxError):
        notice = (
            (UNSUPPORTED if root else NEEDS_ROOT)
            if settings.navigation == 1
            else "Android navigation could not be refreshed. Try again after restarting the phone."
        )
    phone.navigation_notice = notice
    phone._navigation_applied = (key, notice)


def root_allowed(phone):
    from .rooting import Root

    try:
        return "uid=0(" in Root(phone).su("id", timeout=5)
    except (OSError, VeloxError):
        return False


def prepare(phone):
    apk = checked("VeloxNavOrder.apk")
    guard = checked("post-fs-data.sh", 65536)
    stock = checked("stock-taskbar.xml", 1024**2)
    path = phone.shell("pm path " + LAUNCHER, timeout=5)
    if not path.startswith("package:/") or any(char in path for char in "\r\n"):
        return False
    expected = re.search(rb"^EXPECTED=([a-f0-9]{64})$", bounded_read(guard, 65536), re.M)
    actual_hash = phone.shell("sha256sum " + shlex.quote(path[8:]), timeout=10).split()
    if not expected or not actual_hash or actual_hash[0].encode() != expected[1]:
        return False
    actual = phone.adb(
        "exec-out", "unzip", "-p", path[8:], "res/layout/taskbar.xml", timeout=10, maximum=1024**2
    )
    if actual != bounded_read(stock, 1024**2):
        return False
    remote = "/data/local/tmp/velox-navigation-" + uuid.uuid4().hex
    with tempfile.TemporaryDirectory(prefix="navigation-", dir=phone.paths.cache) as directory:
        prop = Path(directory) / "module.prop"
        atomic_bytes(
            prop,
            b"id=velox-navigation\nname=Velox native navigation\nversion=1.0\nversionCode=1\nauthor=Velox\ndescription=Optional native button order for Velox Android 15.\n",
        )
        phone.shell("mkdir " + shlex.quote(remote), timeout=5)
        try:
            for source, leaf in (
                (apk, "VeloxNavOrder.apk"),
                (guard, "post-fs-data.sh"),
                (prop, "module.prop"),
            ):
                phone.adb("push", str(source), remote + "/" + leaf, timeout=30, maximum=1024**2)
            module = "/data/adb/modules/velox-navigation"
            command = f"mkdir -p {module}/system/product/overlay && cp {remote}/VeloxNavOrder.apk {module}/VeloxNavOrder.apk && cp {remote}/VeloxNavOrder.apk {module}/system/product/overlay/VeloxNavOrder.apk && cp {remote}/module.prop {module}/module.prop && cp {remote}/post-fs-data.sh {module}/post-fs-data.sh && chmod 755 {module}/post-fs-data.sh && chmod 644 {module}/module.prop {module}/system/product/overlay/VeloxNavOrder.apk && chcon u:object_r:system_file:s0 {module}/system/product/overlay/VeloxNavOrder.apk && echo velox-nav-prepared"
            from .rooting import Root

            return "velox-nav-prepared" in Root(phone).su(command, timeout=15)
        finally:
            with contextlib.suppress(OSError, VeloxError):
                phone.shell("rm -rf " + shlex.quote(remote), timeout=5)
