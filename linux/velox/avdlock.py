"""Coordinate cold maintenance with the Android Emulator's AVD share lock."""

from __future__ import annotations

import errno
import fcntl
import os
import stat
import time

from .core import Busy, VeloxError
from .fileops import Tree


class AvdLock:
    """Exclude both writable and read-only SDK instances; never delete the lock."""

    def __init__(self, folder, timeout=0):
        if not 0 <= timeout <= 30:
            raise ValueError("Unsupported Android lock wait")
        self.timeout = timeout
        self.tree = Tree(folder)
        self.descriptor = None

    def __enter__(self):
        try:
            self.descriptor = os.open(
                "multiinstance.lock",
                os.O_RDWR | os.O_CREAT | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK,
                0o600,
                dir_fd=self.tree.descriptor,
            )
            info = os.fstat(self.descriptor)
            if (
                not stat.S_ISREG(info.st_mode)
                or info.st_uid != os.getuid()
                or info.st_nlink != 1
                or info.st_size > 65536
            ):
                raise VeloxError("The Android phone lock is not a private regular file.")
            try:
                deadline = time.monotonic() + self.timeout
                while True:
                    try:
                        fcntl.flock(self.descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        break
                    except BlockingIOError as error:
                        remaining = deadline - time.monotonic()
                        if remaining <= 0:
                            raise Busy(
                                "Phone files are in use. Close this phone and other emulator tools before maintenance."
                            ) from error
                        time.sleep(min(0.15, remaining))
            except OSError as error:
                if error.errno in (errno.EINVAL, errno.ENOSYS, errno.EOPNOTSUPP):
                    raise VeloxError(
                        "This storage does not support Android's phone lock. Choose a local Linux filesystem."
                    ) from error
                raise
            self.check()
            return self
        except BaseException:
            self.close()
            raise

    def check(self):
        opened = os.fstat(self.descriptor)
        current = os.stat("multiinstance.lock", dir_fd=self.tree.descriptor, follow_symlinks=False)
        root = self.tree.root.stat(follow_symlinks=False)
        held = os.fstat(self.tree.descriptor)
        if (opened.st_dev, opened.st_ino, opened.st_nlink) != (
            current.st_dev,
            current.st_ino,
            1,
        ) or (root.st_dev, root.st_ino) != (held.st_dev, held.st_ino):
            raise VeloxError(
                "Phone storage changed during maintenance. Close other tools and try again."
            )

    def __exit__(self, *_error):
        self.close()

    def close(self):
        if self.descriptor is not None:
            os.close(self.descriptor)
            self.descriptor = None
        self.tree.close()
