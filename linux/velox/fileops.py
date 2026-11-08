"""Bounded, sparse-aware copies for cold phone maintenance."""

from __future__ import annotations

import contextlib
import ctypes as C
import dataclasses
import errno
import fcntl
import hashlib
import os
from pathlib import Path, PurePosixPath
import stat

from .core import Cancelled, VeloxError


BLOCK = 1024 * 1024
MAX_FILE = 512 * 1024**3
MAX_TREE = 1024**4


def relative_name(name):
    if not isinstance(name, str) or not name:
        raise VeloxError("Unsupported phone file name.")
    try:
        if len(name.encode("utf-8")) > 2048:
            raise VeloxError("Unsupported phone file name.")
    except UnicodeError as error:
        raise VeloxError("Unsupported phone file name.") from error
    path = PurePosixPath(name)
    if (
        path.is_absolute()
        or str(path) != name
        or len(path.parts) > 32
        or any(part in (".", "..") for part in path.parts)
        or any(ord(value) < 32 or value in "\\:\x7f" for value in name)
    ):
        raise VeloxError("Unsafe phone file path.")
    return path.parts


def check_cancel(cancel):
    if cancel is not None and cancel.is_set():
        raise Cancelled("Cancelled. Your original phone is unchanged.")


def identity(info):
    return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns, info.st_nlink


@dataclasses.dataclass(frozen=True)
class Entry:
    name: str
    size: int
    allocated: int
    identity: tuple


class Tree:
    """Resolve every component under an open directory, without following links."""

    def __init__(self, root: Path, *, writable=False):
        self.root, self.writable = Path(root), writable
        self.descriptor = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW)
        info = os.fstat(self.descriptor)
        if writable and (info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) != 0o700):
            self.close()
            raise VeloxError("Write phone data only inside a private folder owned by your account.")

    def __enter__(self):
        return self

    def __exit__(self, *_error):
        self.close()

    def close(self):
        if self.descriptor is not None:
            os.close(self.descriptor)
            self.descriptor = None

    @contextlib.contextmanager
    def parent(self, name, *, create=False):
        parts = relative_name(name)
        descriptor = os.dup(self.descriptor)
        try:
            for part in parts[:-1]:
                if create:
                    if not self.writable:
                        raise ValueError("Read-only phone tree")
                    with contextlib.suppress(FileExistsError):
                        os.mkdir(part, mode=0o700, dir_fd=descriptor)
                child = os.open(
                    part,
                    os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
                    dir_fd=descriptor,
                )
                os.close(descriptor)
                descriptor = child
            yield descriptor, parts[-1]
        finally:
            os.close(descriptor)

    def open(self, name, *, create=False, write=False):
        if (create or write) and not self.writable:
            raise ValueError("Read-only phone tree")
        with self.parent(name, create=create) as (directory, leaf):
            flags = (
                os.O_RDWR | os.O_CREAT | os.O_EXCL
                if create
                else (os.O_RDWR if write else os.O_RDONLY) | os.O_NONBLOCK
            )
            descriptor = os.open(
                leaf, flags | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600, dir_fd=directory
            )
        try:
            info = os.fstat(descriptor)
            if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_size > MAX_FILE:
                raise VeloxError("Linked, special or oversized phone files are not supported.")
            return descriptor
        except BaseException:
            os.close(descriptor)
            raise

    def unlink_created(self, name, descriptor):
        with self.parent(name) as (directory, leaf):
            try:
                current = os.stat(leaf, dir_fd=directory, follow_symlinks=False)
            except FileNotFoundError:
                return
            opened = os.fstat(descriptor)
            if (current.st_dev, current.st_ino) == (opened.st_dev, opened.st_ino):
                os.unlink(leaf, dir_fd=directory)

    def entries(self, include=lambda _name, _directory: True):
        result, total, names, visited = [], 0, set(), 0

        def walk(descriptor, prefix=""):
            nonlocal total, visited
            with os.scandir(descriptor) as children:
                bounded = []
                for child in children:
                    visited += 1
                    if visited > 20000:
                        raise VeloxError("The phone tree contains too many entries.")
                    bounded.append(child)
                for child in sorted(bounded, key=lambda entry: entry.name):
                    name = prefix + child.name
                    info = child.stat(follow_symlinks=False)
                    directory = stat.S_ISDIR(info.st_mode)
                    if not include(name, directory):
                        continue
                    relative_name(name)
                    if directory:
                        nested = os.open(
                            child.name,
                            os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
                            dir_fd=descriptor,
                        )
                        try:
                            walk(nested, name + "/")
                        finally:
                            os.close(nested)
                    else:
                        if (
                            not stat.S_ISREG(info.st_mode)
                            or info.st_nlink != 1
                            or info.st_size > MAX_FILE
                        ):
                            raise VeloxError(
                                "Linked, special or oversized phone files are not supported."
                            )
                        if (
                            len(result) >= 10000
                            or name.casefold() in names
                            or total + info.st_size > MAX_TREE
                        ):
                            raise VeloxError(
                                "The phone tree is too large or has conflicting file names."
                            )
                        names.add(name.casefold())
                        total += info.st_size
                        result.append(
                            Entry(name, info.st_size, info.st_blocks * 512, identity(info))
                        )

        walk(self.descriptor)
        return result

    def sync(self, names=()):
        parents = {""}
        for name in names:
            parts = relative_name(name)
            parents.update("/".join(parts[:count]) for count in range(1, len(parts)))
        for parent in sorted(
            parents, key=lambda value: value.count("/") + bool(value), reverse=True
        ):
            with self.parent(parent + "/.sync" if parent else ".sync") as (directory, _leaf):
                os.fsync(directory)


class ReadLocks:
    """Open-file-description locks survive other readers closing their FDs."""

    def __init__(self, tree, entries):
        self.tree, self.entries, self.descriptors = tree, entries, []

    def __enter__(self):
        class Lock(C.Structure):
            _fields_ = [
                ("kind", C.c_short),
                ("whence", C.c_short),
                ("start", C.c_longlong),
                ("length", C.c_longlong),
                ("pid", C.c_int),
            ]

        try:
            for entry in self.entries:
                descriptor = self.tree.open(entry.name)
                try:
                    # Standalone Python may be built against headers that omit
                    # this Linux constant. Its ABI value is fixed for x86-64.
                    fcntl.fcntl(
                        descriptor,
                        getattr(fcntl, "F_OFD_SETLK", 37),
                        bytes(Lock(fcntl.F_RDLCK, os.SEEK_SET, 0, 0, 0)),
                    )
                    if identity(os.fstat(descriptor)) != entry.identity:
                        raise VeloxError("A phone file changed before it could be locked.")
                    self.descriptors.append(descriptor)
                    descriptor = None
                finally:
                    if descriptor is not None:
                        os.close(descriptor)
        except OSError as error:
            self.close()
            if error.errno in (errno.EACCES, errno.EAGAIN):
                raise VeloxError(
                    "Phone files are in use. Close this phone and other emulator tools before copying."
                ) from error
            if error.errno in (errno.EINVAL, errno.ENOSYS, errno.EOPNOTSUPP):
                raise VeloxError(
                    "This storage does not support the file locks needed for safe phone copies."
                ) from error
            raise
        except BaseException:
            self.close()
            raise
        return self

    def __exit__(self, *_error):
        self.close()

    def close(self):
        for descriptor in self.descriptors:
            os.close(descriptor)
        self.descriptors.clear()


def hash_descriptor(descriptor, cancel=None):
    before = os.fstat(descriptor)
    digest, offset, discarded = hashlib.sha256(), 0, 0
    streaming = before.st_size >= 128 * 1024**2

    def advise(start, size, option):
        if streaming and option is not None and hasattr(os, "posix_fadvise"):
            with contextlib.suppress(OSError):
                os.posix_fadvise(descriptor, start, size, option)

    # Cold disk verification must not fill the desktop's cache with tens of
    # gigabytes of already-consumed data. Advise this file's consumed ranges.
    advise(0, 0, getattr(os, "POSIX_FADV_NOREUSE", None))
    try:
        while offset < before.st_size:
            check_cancel(cancel)
            block = os.pread(descriptor, min(BLOCK, before.st_size - offset), offset)
            if not block:
                raise VeloxError("A phone file changed during verification.")
            digest.update(block)
            offset += len(block)
            if offset - discarded >= 64 * 1024**2:
                advise(discarded, offset - discarded, getattr(os, "POSIX_FADV_DONTNEED", None))
                discarded = offset
        if identity(before) != identity(os.fstat(descriptor)):
            raise VeloxError("A phone file changed during verification.")
        return digest.hexdigest()
    finally:
        if offset > discarded:
            advise(discarded, offset - discarded, getattr(os, "POSIX_FADV_DONTNEED", None))


def _extent(source, offset, size):
    try:
        start = os.lseek(source, offset, os.SEEK_DATA)
        end = min(size, os.lseek(source, start, os.SEEK_HOLE))
        if start < offset or end <= start:
            raise VeloxError("Invalid sparse phone file extent.")
        return start, end
    except OSError as error:
        if error.errno == errno.ENXIO:
            return size, size
        if error.errno in (errno.EINVAL, errno.ENOTSUP):
            return offset, size
        raise


def copy_file(
    source: Tree,
    destination: Tree,
    entry: Entry,
    *,
    target=None,
    cancel=None,
    progress=lambda _done: None,
):
    """Try kernel copy-on-write, then copy allocated extents in bounded chunks."""
    target = target or entry.name
    check_cancel(cancel)
    read_descriptor = source.open(entry.name)
    write_descriptor = None
    completed = False
    try:
        if identity(os.fstat(read_descriptor)) != entry.identity:
            raise VeloxError("A phone file changed before it could be copied.")
        write_descriptor = destination.open(target, create=True)
        cloned = False
        try:
            fcntl.ioctl(write_descriptor, 0x40049409, read_descriptor)  # Linux FICLONE
            cloned = True
            progress(entry.size)
        except OSError as error:
            if error.errno not in (
                errno.EXDEV,
                errno.EINVAL,
                errno.EOPNOTSUPP,
                errno.ENOTTY,
                errno.ENOSYS,
            ):
                raise
        if not cloned:
            os.ftruncate(write_descriptor, entry.size)
            offset, kernel_copy = 0, True
            while offset < entry.size:
                check_cancel(cancel)
                start, end = _extent(read_descriptor, offset, entry.size)
                offset = start
                while offset < end:
                    check_cancel(cancel)
                    count = min(BLOCK, end - offset)
                    if kernel_copy:
                        try:
                            copied = os.copy_file_range(
                                read_descriptor, write_descriptor, count, offset, offset
                            )
                            if not copied:
                                kernel_copy = False
                        except OSError as error:
                            if error.errno not in (
                                errno.EXDEV,
                                errno.EINVAL,
                                errno.EOPNOTSUPP,
                                errno.ENOSYS,
                            ):
                                raise
                            kernel_copy = False
                    if not kernel_copy:
                        block = os.pread(read_descriptor, count, offset)
                        if not block:
                            raise VeloxError("A phone file changed during copying.")
                        copied = len(block)
                        if block.count(0) != len(block):
                            pending = memoryview(block)
                            while pending:
                                written = os.pwrite(
                                    write_descriptor, pending, offset + copied - len(pending)
                                )
                                if written <= 0:
                                    raise OSError("Phone copy made no progress")
                                pending = pending[written:]
                    offset += copied
                    progress(offset)
                progress(offset)
        os.fsync(write_descriptor)
        check_cancel(cancel)
        if (
            identity(os.fstat(read_descriptor)) != entry.identity
            or os.fstat(write_descriptor).st_size != entry.size
        ):
            raise VeloxError("A phone file changed during copying.")
        digest = hash_descriptor(write_descriptor, cancel)
        completed = True
        return digest
    finally:
        try:
            if write_descriptor is not None:
                try:
                    if not completed:
                        destination.unlink_created(target, write_descriptor)
                finally:
                    os.close(write_descriptor)
        finally:
            os.close(read_descriptor)
