"""Optional per-account application-menu integration; no root or shell execution."""

from __future__ import annotations

import os
from pathlib import Path
import stat
import tempfile

from .core import VeloxError, atomic_bytes, private_directory
from .resources import checked
from .updates import appimage


def quote(value):
    if any(ord(char) < 32 for char in value) or "=" in value:
        raise VeloxError(
            "Move Velox to a folder without control characters or an equals sign before creating a menu shortcut."
        )
    escaped = ""
    for char in value:
        escaped += (
            "\\\\\\\\"
            if char == "\\"
            else "\\\\" + char
            if char in ('"', "`", "$")
            else "%%"
            if char == "%"
            else char
        )
    return '"' + escaped + '"'


def install(paths):
    image = appimage()
    data = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")).absolute()
    applications = data / "applications"
    applications.mkdir(mode=0o755, parents=True, exist_ok=True)
    info = applications.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or applications.is_symlink():
        raise VeloxError(
            "The application-menu folder must belong to your account and cannot be a link."
        )
    icon = private_directory(paths.root / "desktop") / "velox.png"
    atomic_bytes(icon, checked("icon.png").read_bytes(), 0o644)
    # String escaping precedes Exec argument escaping in the desktop-entry format.
    icon_value = str(icon).replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r")
    text = (
        "[Desktop Entry]\nType=Application\nVersion=1.0\nName=Velox\n"
        "Comment=Your games. Your phone. Your space.\n"
        "Comment[de]=Deine Spiele. Dein Handy. Dein Platz.\n"
        # A fixed launcher also lets GIO resolve the executable before expanding
        # a literal %% in the AppImage filename. No shell parses these arguments.
        f"Exec=/usr/bin/env -- {quote(str(image))} --data-dir {quote(str(paths.root))}\nIcon={icon_value}\n"
        "Terminal=false\nCategories=Game;Emulator;\nStartupWMClass=Velox\n"
        "X-Velox-Managed=true\n"
    )
    target = applications / "app.velox.Velox.desktop"
    descriptor, temporary = tempfile.mkstemp(prefix=".velox-", dir=applications)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(text.encode())
            os.fchmod(output.fileno(), 0o644)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, target)
        directory = os.open(
            applications, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW
        )
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        Path(temporary).unlink(missing_ok=True)
    return target
