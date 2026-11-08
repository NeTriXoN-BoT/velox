from __future__ import annotations

import dataclasses
import os
import re
import shlex
import shutil
import stat
import struct
import tempfile
import threading
import urllib.parse
import zipfile
from pathlib import Path, PurePosixPath

from .core import VeloxError, file_hash, package, private_directory, regular
from .fileops import check_cancel


@dataclasses.dataclass(frozen=True)
class Apk:
    file: Path
    package: str
    split: str
    configuration: str
    version: int
    digest: str


def manifest(path: Path, cancel=None) -> Apk:
    """Read Android's binary manifest without executing tools from an archive."""
    try:
        with zipfile.ZipFile(path) as archive:
            info = archive.getinfo("AndroidManifest.xml")
            if info.file_size > 4 * 1024 * 1024 or info.flag_bits & 1:
                raise VeloxError("This Android manifest is too large or encrypted.")
            with archive.open(info) as source:
                data = source.read(4 * 1024 * 1024 + 1)
        if len(data) > 4 * 1024 * 1024:
            raise VeloxError("This Android manifest is too large.")

        def u16(offset):
            return struct.unpack_from("<H", data, offset)[0]

        def u32(offset):
            return struct.unpack_from("<I", data, offset)[0]

        if len(data) < 8 or u16(0) != 3 or u32(4) != len(data):
            raise VeloxError("This file doesn't contain a supported Android manifest.")
        strings, attributes = [], None
        offset = u16(2)
        while offset + 8 <= len(data):
            kind, header, size = u16(offset), u16(offset + 2), u32(offset + 4)
            if size < header or header < 8 or size > len(data) - offset:
                raise VeloxError("The Android manifest is damaged.")
            end = offset + size
            if kind == 1:
                count, flags, start = u32(offset + 8), u32(offset + 16), u32(offset + 20)
                if header < 28 or count > 65536 or header + count * 4 > size or start > size:
                    raise VeloxError("The manifest string table is invalid.")
                strings = []

                def length(position, utf8):
                    if position >= end:
                        raise ValueError("String length outside chunk")
                    value = data[position] if utf8 else u16(position)
                    position += 1 if utf8 else 2
                    if value & (0x80 if utf8 else 0x8000):
                        value = ((value & (0x7F if utf8 else 0x7FFF)) << (8 if utf8 else 16)) | (
                            data[position] if utf8 else u16(position)
                        )
                        position += 1 if utf8 else 2
                    return value, position

                for index in range(count):
                    position = offset + start + u32(offset + header + index * 4)
                    if not offset + start <= position < end:
                        raise ValueError("Invalid string offset")
                    utf8 = bool(flags & 0x100)
                    count_chars, position = length(position, utf8)
                    if utf8:
                        count_bytes, position = length(position, True)
                    else:
                        count_bytes = count_chars * 2
                    if position + count_bytes + (1 if utf8 else 2) > end:
                        raise ValueError("String outside chunk")
                    strings.append(
                        data[position : position + count_bytes].decode(
                            "utf-8" if utf8 else "utf-16-le"
                        )
                    )
            elif kind == 0x102:
                name = strings[u32(offset + 20)]
                if name == "manifest":
                    if attributes is not None:
                        raise ValueError("Repeated manifest root")
                    attributes = {}
                    first, stride, count = u16(offset + 24), u16(offset + 26), u16(offset + 28)
                    if stride < 20 or count > 128 or 16 + first + stride * count > size:
                        raise ValueError("Invalid attributes")
                    for index in range(count):
                        at = offset + 16 + first + index * stride
                        key, raw = strings[u32(at + 4)], u32(at + 8)
                        value = (
                            strings[raw]
                            if raw != 0xFFFFFFFF
                            else strings[u32(at + 16)]
                            if data[at + 15] == 3
                            else u32(at + 16)
                        )
                        if key in attributes:
                            raise ValueError("Repeated manifest attribute")
                        attributes[key] = value
            offset = end
        if attributes is None:
            raise ValueError("Missing manifest root")
        identity = package(attributes.get("package", ""))
        split, configuration, version = (
            attributes.get("split", ""),
            attributes.get("configForSplit", ""),
            attributes.get("versionCode", 0),
        )
        if (
            not isinstance(split, str)
            or not isinstance(configuration, str)
            or len(split) > 200
            or len(configuration) > 200
            or type(version) is not int
        ):
            raise ValueError("Invalid split identity")
        return Apk(path, identity, split, configuration, version, file_hash(path, cancel))
    except (
        OSError,
        KeyError,
        IndexError,
        ValueError,
        struct.error,
        zipfile.BadZipFile,
        RuntimeError,
    ) as error:
        raise VeloxError("The APK manifest is damaged or unsupported.") from error


def select_splits(apks: list[Apk], density: int) -> list[Apk]:
    if not apks or len(apks) > 512:
        raise VeloxError("This app bundle contains no usable APKs or too many splits.")
    universal = [apk for apk in apks if apk.file.name.lower() == "universal.apk" and not apk.split]
    if len(universal) == 1:
        return universal
    bases = [apk for apk in apks if not apk.split]
    if len(bases) != 1:
        raise VeloxError(
            "This bundle contains several app variants. Choose a universal APK or a bundle for x86-64 Android."
        )
    base = bases[0]
    if any(apk.package != base.package or apk.version != base.version for apk in apks):
        raise VeloxError("The bundle mixes different apps or versions; nothing was installed.")
    if len({apk.split for apk in apks}) != len(apks):
        raise VeloxError("The bundle contains conflicting split names.")
    abi = re.compile(r"(?:^|[._-])(x86_64|x86|arm64_v8a|armeabi_v7a)(?:$|[.-])")
    present = {match[1] for apk in apks if (match := abi.search(apk.split))}
    preferred = next(
        (name for name in ("x86_64", "x86", "arm64_v8a", "armeabi_v7a") if name in present), None
    )
    density_names = {
        "ldpi": 120,
        "mdpi": 160,
        "tvdpi": 213,
        "hdpi": 240,
        "xhdpi": 320,
        "xxhdpi": 480,
        "xxxhdpi": 640,
    }
    pattern = re.compile(r"(?:^|[._-])(xxxhdpi|xxhdpi|xhdpi|tvdpi|hdpi|mdpi|ldpi)(?:$|[.-])")
    groups = {}
    for apk in apks:
        if match := pattern.search(apk.split):
            groups.setdefault(apk.configuration, set()).add(match[1])
    best = {
        group: min(names, key=lambda name: abs(density_names[name] - density))
        for group, names in groups.items()
    }
    return [
        apk
        for apk in apks
        if (not (match := abi.search(apk.split)) or match[1] == preferred)
        and (not (match := pattern.search(apk.split)) or match[1] == best[apk.configuration])
    ]


@dataclasses.dataclass(frozen=True)
class Operation:
    kind: str
    files: tuple[Path, ...]
    destination: str
    digests: tuple[str, ...]
    label: str


class FileBatch:
    def __init__(self, phone, sources: list[Path], density=420, cancel=None):
        if not 1 <= len(sources) <= 64:
            raise VeloxError("Choose between 1 and 64 files.")
        self.phone, self.operations = phone, []
        self.cancel = cancel or threading.Event()
        self._temporary = tempfile.TemporaryDirectory(
            prefix="import-", dir=private_directory(phone.paths.cache / "imports")
        )
        self.folder = Path(self._temporary.name)
        try:
            for index, source in enumerate(sources):
                check_cancel(self.cancel)
                source = source.absolute()
                info = regular(source, maximum=8 * 1024**3)
                if shutil.disk_usage(self.folder).free < info.st_size + 1024**3:
                    raise VeloxError("There isn't enough free space to prepare these files safely.")
                local = self.folder / str(index) / source.name
                private_directory(local.parent)
                with (
                    os.fdopen(
                        os.open(source, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW), "rb"
                    ) as reader,
                    local.open("xb") as target,
                ):
                    before = os.fstat(reader.fileno())
                    if not stat.S_ISREG(before.st_mode):
                        raise VeloxError("Choose regular files, without links.")
                    copied = 0
                    while block := reader.read(1024 * 1024):
                        check_cancel(self.cancel)
                        copied += len(block)
                        if copied > info.st_size:
                            raise VeloxError(
                                "A selected file changed while being prepared. Choose it again."
                            )
                        target.write(block)
                    after = os.fstat(reader.fileno())
                    if copied != info.st_size or (before.st_size, before.st_mtime_ns) != (
                        after.st_size,
                        after.st_mtime_ns,
                    ):
                        raise VeloxError(
                            "A selected file changed while being prepared. Choose it again."
                        )
                os.chmod(local, 0o600)
                extension = source.suffix.lower()
                if extension == ".apk":
                    apk = manifest(local, self.cancel)
                    self.operations.append(
                        Operation("install", (local,), apk.package, (apk.digest,), source.name)
                    )
                elif extension in (".apks", ".xapk", ".apkm"):
                    self._bundle(local, density)
                elif extension == ".obb":
                    match = re.fullmatch(
                        r"(?:main|patch)\.[0-9]{1,12}\.([A-Za-z0-9_.]+)\.obb", source.name
                    )
                    if not match:
                        raise VeloxError(
                            "OBB names must use main.<version>.<package>.obb or patch.<version>.<package>.obb."
                        )
                    identity = package(match[1])
                    self._file(local, "/sdcard/Android/obb/" + identity + "/" + source.name)
                elif extension == ".aab":
                    raise VeloxError(
                        "Export a universal APK or APK set from this Android App Bundle before installing it."
                    )
                else:
                    self._file(local, "/sdcard/Download/" + source.name)
        except Exception:
            self.close()
            raise

    def _file(self, file, destination):
        self.operations.append(
            Operation("file", (file,), destination, (file_hash(file, self.cancel),), file.name)
        )

    def _bundle(self, path, density):
        output = private_directory(path.parent / "unpacked")
        apks, obbs = [], []
        try:
            with zipfile.ZipFile(path) as archive:
                entries = archive.infolist()
                if len(entries) > 20000 or sum(entry.file_size for entry in entries) > 12 * 1024**3:
                    raise VeloxError("This app bundle exceeds the supported extraction limits.")
                if (
                    shutil.disk_usage(output).free
                    < sum(entry.file_size for entry in entries) + 1024**3
                ):
                    raise VeloxError("There isn't enough space to unpack this bundle safely.")
                seen = set()
                for entry in entries:
                    check_cancel(self.cancel)
                    relative = PurePosixPath(entry.filename)
                    kind = (entry.external_attr >> 16) & 0o170000
                    if (
                        not entry.filename
                        or "\\" in entry.filename
                        or relative.is_absolute()
                        or ".." in relative.parts
                        or any(
                            part in (".", "") or ":" in part or any(ord(c) < 32 for c in part)
                            for part in relative.parts
                        )
                        or entry.flag_bits & 1
                        or kind not in (0, stat.S_IFREG, stat.S_IFDIR)
                        or relative in seen
                    ):
                        raise VeloxError("Unsafe or conflicting bundle paths were rejected.")
                    seen.add(relative)
                    if entry.is_dir() or relative.suffix.lower() not in (".apk", ".obb"):
                        continue
                    target = output.joinpath(*relative.parts)
                    private_directory(target.parent)
                    with archive.open(entry) as reader, target.open("xb") as writer:
                        total = 0
                        while block := reader.read(1024 * 1024):
                            check_cancel(self.cancel)
                            total += len(block)
                            if total > entry.file_size:
                                raise VeloxError("Invalid bundle member size.")
                            writer.write(block)
                    os.chmod(target, 0o600)
                    if relative.suffix.lower() == ".apk":
                        apks.append(manifest(target, self.cancel))
                    else:
                        obbs.append((target, relative))
        except (zipfile.BadZipFile, RuntimeError) as error:
            raise VeloxError("The Android app bundle could not be opened.") from error
        selected = select_splits(apks, density)
        identity = selected[0].package
        self.operations.append(
            Operation(
                "install",
                tuple(apk.file for apk in selected),
                identity,
                tuple(apk.digest for apk in selected),
                path.name,
            )
        )
        for file, relative in obbs:
            if not re.fullmatch(
                r"(?:main|patch)\.[0-9]{1,12}\." + re.escape(identity) + r"\.obb", file.name
            ):
                raise VeloxError("An expansion file doesn't belong to the bundled app.")
            self._file(file, "/sdcard/Android/obb/" + identity + "/" + file.name)

    def apply(self, report=lambda _message, _fraction: None):
        if not self.phone.alive() or self.phone.state != "running":
            raise VeloxError("Start the phone before installing apps or sending files.")
        for index, operation in enumerate(self.operations):
            check_cancel(self.cancel)
            report(operation.label, index / len(self.operations))
            if any(
                file_hash(file, self.cancel) != digest
                for file, digest in zip(operation.files, operation.digests)
            ):
                raise VeloxError("A prepared file changed; nothing further was installed.")
            if operation.kind == "install":
                command = "install" if len(operation.files) == 1 else "install-multiple"
                result = self.phone.adb(
                    command,
                    "-r",
                    *map(str, operation.files),
                    timeout=300,
                    maximum=1024 * 1024,
                    cancel=self.cancel,
                )
                if b"Success" not in result:
                    raise VeloxError(
                        "Android refused this app. Existing apps and saves were retained."
                    )
            else:
                directory = str(PurePosixPath(operation.destination).parent)
                self.phone.shell("mkdir -p " + shlex.quote(directory))
                self.phone.adb(
                    "push",
                    str(operation.files[0]),
                    operation.destination,
                    timeout=600,
                    maximum=1024 * 1024,
                    cancel=self.cancel,
                )
                self.phone.shell(
                    "am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d "
                    + shlex.quote("file://" + urllib.parse.quote(operation.destination))
                )
        report("Ready", 1.0)

    def close(self):
        self._temporary.cleanup()
