"""Private, bounded emulator logs without following file links."""

from __future__ import annotations

import os
from pathlib import Path
import stat

from .core import VeloxError


def open_log(path: Path, maximum=16 * 1024 * 1024):
    descriptor = os.open(
        path, os.O_WRONLY | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600
    )
    try:
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_uid != os.getuid():
            raise VeloxError("Choose a private, unlinked emulator log in this phone's folder.")
        if info.st_size > maximum:
            current = path.stat(follow_symlinks=False)
            if (current.st_dev, current.st_ino) != (info.st_dev, info.st_ino):
                raise VeloxError(
                    "The emulator log changed before startup. Close other tools and try again."
                )
            os.replace(path, path.with_suffix(".previous.log"))
            os.close(descriptor)
            descriptor = None
            descriptor = os.open(
                path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600
            )
        os.fchmod(descriptor, 0o600)
        os.ftruncate(descriptor, 0)
        return descriptor
    except BaseException:
        if descriptor is not None:
            os.close(descriptor)
        raise
