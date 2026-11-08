"""Validate cold QCOW disks and rewrite only their known phone/image backing paths."""

from __future__ import annotations

import dataclasses
import os
import posixpath
import re
import struct

from .core import VeloxError
from .fileops import MAX_FILE, relative_name


def require(value, message):
    if not value:
        raise VeloxError(message)


@dataclasses.dataclass(frozen=True)
class Header:
    virtual_size: int
    cluster: int
    backing_offset: int
    backing_length: int
    backing: str
    format: str


def inspect(descriptor):
    info = os.fstat(descriptor)
    raw = os.pread(descriptor, 104, 0)
    if raw[:4] != b"QFI\xfb":
        return None
    require(len(raw) == 104, "A phone disk header is truncated.")

    def u32(offset):
        return struct.unpack_from(">I", raw, offset)[0]

    def u64(offset):
        return struct.unpack_from(">Q", raw, offset)[0]

    version, bits = u32(4), u32(20)
    require(version in (2, 3) and 9 <= bits <= 21, "Unsupported phone disk format.")
    cluster = 1 << bits
    header = 72 if version == 2 else u32(100)
    require(
        (72 if version == 2 else 104) <= header <= cluster and info.st_size >= cluster,
        "Invalid phone disk header size.",
    )
    require(
        version == 2 or u64(72) & ~1 == 0,
        "External data files or unsupported disk features are not allowed.",
    )
    require(u32(32) == 0, "Encrypted QCOW files are not supported.")
    virtual, backing_offset, backing_length = u64(24), u64(8), u32(16)
    require(0 < virtual <= MAX_FILE, "Invalid virtual phone disk size.")
    l1_size, l1_offset = u32(36), u64(40)
    required_l1 = (virtual + cluster * (cluster // 8) - 1) // (cluster * (cluster // 8))
    require(
        0 < l1_size <= required_l1 * 2 + 512
        and l1_size * 8 <= 128 * 1024**2
        and l1_offset >= cluster
        and l1_offset % cluster == 0
        and l1_offset + l1_size * 8 <= info.st_size,
        "Invalid phone disk mapping table.",
    )
    ref_offset, ref_clusters = u64(48), u32(56)
    require(
        0 < ref_clusters
        and ref_clusters * cluster <= 64 * 1024**2
        and ref_offset >= cluster
        and ref_offset % cluster == 0
        and ref_offset + ref_clusters * cluster <= info.st_size,
        "Invalid phone disk reference table.",
    )
    require(
        u32(60) <= 64
        and (u32(60) == 0 or u64(64) >= cluster and u64(64) + u32(60) * 40 <= info.st_size),
        "Invalid phone disk snapshot table.",
    )
    require(version == 2 or u32(96) <= 6, "Unsupported phone disk reference format.")
    require(
        (backing_length == 0 and backing_offset == 0)
        or (
            0 < backing_length < 4096
            and header <= backing_offset < cluster
            and backing_length <= cluster - backing_offset
        ),
        "Invalid disk backing reference.",
    )
    end = backing_offset if backing_length else cluster
    offset, backing_format, records = header, "", 0
    while offset + 8 <= end:
        extension = os.pread(descriptor, 8, offset)
        require(len(extension) == 8, "Truncated disk extension.")
        kind, length = struct.unpack(">II", extension)
        if kind == 0:
            require(length == 0, "Invalid disk extension terminator.")
            break
        records += 1
        require(
            records <= 128 and ((length + 7) & ~7) <= end - offset - 8 and kind != 0x44415441,
            "External or malformed disk extensions are not allowed.",
        )
        if kind == 0xE2792ACA:
            value = os.pread(descriptor, length, offset + 8)
            require(
                not backing_format and value in (b"raw", b"qcow2"),
                "Unsupported disk backing format.",
            )
            backing_format = value.decode("ascii")
        offset += 8 + ((length + 7) & ~7)
    backing = ""
    if backing_length:
        require(bool(backing_format), "The disk backing format must be explicit.")
        value = os.pread(descriptor, backing_length, backing_offset)
        require(len(value) == backing_length, "Truncated disk backing name.")
        try:
            backing = value.decode("utf-8")
        except UnicodeError as error:
            raise VeloxError("Unsupported disk backing name.") from error
        require(
            not any(ord(character) < 32 or ord(character) == 127 for character in backing),
            "Unsafe disk backing name.",
        )
    return Header(virtual, cluster, backing_offset, backing_length, backing, backing_format)


def _original_path(value):
    require(
        isinstance(value, str)
        and 0 < len(value) < 4096
        and not any(ord(character) < 32 for character in value),
        "Invalid original phone path.",
    )
    value = value.replace("\\", "/")
    drive = bool(re.match(r"^[A-Za-z]:/", value))
    require(
        (":" not in value[2:] if drive else ":" not in value) and (value.startswith("/") or drive),
        "Original phone paths must be absolute file paths.",
    )
    return posixpath.normpath(value)


def backing_target(reference, original_avd, original_image, phone_files, image_files):
    avd, image = _original_path(original_avd), _original_path(original_image)
    reference = reference.replace("\\", "/")
    absolute = reference.startswith("/") or bool(re.match(r"^[A-Za-z]:/", reference))
    require(
        ":" not in reference[2:] and (absolute or ":" not in reference),
        "External disk protocols are not allowed.",
    )
    normal = posixpath.normpath(reference if absolute else posixpath.join(avd, reference))
    windows = bool(re.match(r"^[A-Za-z]:/", avd)) or avd.startswith("//")
    for kind, root, names in (("phone", avd, phone_files), ("image", image, image_files)):
        prefix = root.rstrip("/") + "/"
        matches = (
            normal.casefold().startswith(prefix.casefold())
            if windows
            else normal.startswith(prefix)
        )
        if matches:
            tail = normal[len(prefix) :]
            relative_name(tail)
            matched = next(
                (
                    name
                    for name in names
                    if (name.casefold() == tail.casefold() if windows else name == tail)
                ),
                None,
            )
            require(
                matched is not None, "A disk references a file outside its verified phone or image."
            )
            return kind, matched
    raise VeloxError("External disk backing files are not allowed.")


def rebase(tree, name, original_avd, original_image, image, phone_files, image_files):
    descriptor = tree.open(name, write=True)
    try:
        header = inspect(descriptor)
        require(
            header is not None or not name.casefold().endswith(".qcow2"),
            "A QCOW disk has an invalid header.",
        )
        if header is None:
            return None
        if not header.backing:
            return header.virtual_size
        require(
            name.casefold().endswith(".qcow2"), "Only a phone overlay may have a backing image."
        )
        kind, target = backing_target(
            header.backing, original_avd, original_image, phone_files, image_files
        )
        require(
            posixpath.basename(target).casefold() == posixpath.basename(name[:-6]).casefold(),
            "The backing image does not match this overlay.",
        )
        base_tree = tree if kind == "phone" else image
        base_descriptor = base_tree.open(target)
        try:
            base = inspect(base_descriptor)
            require(
                (base is not None) == (header.format == "qcow2")
                and (base is None or not base.backing),
                "Alternate disk backing chains are not allowed.",
            )
        finally:
            os.close(base_descriptor)
        actual = base_tree.root / target
        parent = (tree.root / name).parent
        value = os.path.relpath(actual, parent).replace(os.sep, "/").encode("utf-8")
        require(
            len(value) < 4096 and len(value) <= header.cluster - header.backing_offset,
            "The recovered backing path is too long.",
        )
        if len(value) > header.backing_length:
            padding = os.pread(
                descriptor,
                len(value) - header.backing_length,
                header.backing_offset + header.backing_length,
            )
            require(
                len(padding) == len(value) - header.backing_length and not any(padding),
                "The disk has no safe space for its recovered backing path.",
            )
        replacement = value.ljust(max(len(value), header.backing_length), b"\0")
        require(
            os.pwrite(descriptor, replacement, header.backing_offset) == len(replacement),
            "Disk path rewrite was incomplete.",
        )
        require(
            os.pwrite(descriptor, struct.pack(">I", len(value)), 16) == 4,
            "Disk header rewrite was incomplete.",
        )
        os.fsync(descriptor)
        return header.virtual_size
    finally:
        os.close(descriptor)
