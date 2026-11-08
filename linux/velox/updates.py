"""Signed Linux release metadata and atomic AppImage replacement."""

from __future__ import annotations

import dataclasses
import hashlib
import os
from pathlib import Path
import re
import shutil
import stat
import tempfile
import threading
import urllib.parse

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec, utils

from .core import VeloxError, atomic_json, bounded_read, lease
from .fileops import check_cancel, hash_descriptor, identity
from .setup import download, fetch, resource_root


FEED = "https://github.com/NeTriXoN-BoT/velox/releases/latest/download/update-linux.txt"
PUBLIC_X = 0x58690467E06269000159D30B58B0DEFDEABAFC0354BC2A44EA31D303766CF71D
PUBLIC_Y = 0x67E64B34CED369CA6E981204191FFFF2F27B2AD7869C46A5A30D4353C0BD0273
MAX_IMAGE = 768 * 1024**2


def version(value):
    if not isinstance(value, str) or not re.fullmatch(
        r"(?:0|[1-9][0-9]{0,4})(?:\.(?:0|[1-9][0-9]{0,4})){2}", value
    ):
        raise VeloxError("The update version is invalid.")
    result = tuple(map(int, value.split(".")))
    if any(part > 65535 for part in result):
        raise VeloxError("The update version is invalid.")
    return result


def current_version():
    try:
        value = bounded_read(resource_root() / "version.txt", 32).decode("ascii").strip()
        version(value)
        return value
    except (OSError, VeloxError, UnicodeError) as error:
        raise VeloxError(
            "Velox's version metadata is missing or invalid. Download a fresh copy."
        ) from error


@dataclasses.dataclass(frozen=True)
class Release:
    version: str
    url: str
    size: int
    sha256: str
    notes: str
    manifest: bytes = dataclasses.field(repr=False)

    @property
    def package(self):
        return dict(name="Downloading update…", url=self.url, bytes=self.size, sha256=self.sha256)


def verify(data: bytes) -> Release:
    try:
        if len(data) > 65536 or b"\r" in data or not data.endswith(b"\n"):
            raise ValueError("Invalid framing")
        lines = data.decode("utf-8", errors="strict").splitlines()
        if len(lines) != 7 or lines[0] != "velox-linux-update 1":
            raise ValueError("Invalid manifest")
        keys = ("version", "url", "size", "sha256", "notes", "signature")
        fields = {}
        for key, line in zip(keys, lines[1:]):
            if not line.startswith(key + "="):
                raise ValueError("Invalid field")
            fields[key] = line[len(key) + 1 :]
        version(fields["version"])
        url = urllib.parse.urlsplit(fields["url"])
        if (
            url.scheme != "https"
            or not url.hostname
            or url.username
            or url.password
            or url.fragment
        ):
            raise ValueError("Invalid URL")
        if (
            not re.fullmatch(r"[1-9][0-9]{0,9}", fields["size"])
            or not 1024 <= int(fields["size"]) <= MAX_IMAGE
        ):
            raise ValueError("Invalid size")
        if not re.fullmatch(r"[a-f0-9]{64}", fields["sha256"]) or not re.fullmatch(
            r"[a-f0-9]{128}", fields["signature"]
        ):
            raise ValueError("Invalid digest/signature")
        signature = bytes.fromhex(fields["signature"])
        encoded = utils.encode_dss_signature(
            int.from_bytes(signature[:32]), int.from_bytes(signature[32:])
        )
        public = ec.EllipticCurvePublicNumbers(PUBLIC_X, PUBLIC_Y, ec.SECP256R1()).public_key()
        body = data[: data.rindex(b"signature=")]
        public.verify(encoded, body, ec.ECDSA(hashes.SHA256()))
        return Release(
            fields["version"],
            fields["url"],
            int(fields["size"]),
            fields["sha256"],
            fields["notes"].replace("\\n", "\n"),
            data,
        )
    except (ValueError, UnicodeError, InvalidSignature) as error:
        raise VeloxError(
            "The Linux update signature or release metadata is invalid. Nothing was installed."
        ) from error


def check():
    release = verify(fetch(FEED, 65536))
    return release if version(release.version) > version(current_version()) else None


def appimage():
    value = os.environ.get("APPIMAGE", "")
    if not value:
        raise VeloxError("Open the packaged AppImage to use automatic updates.")
    path = Path(value).absolute()
    info = path.lstat()
    parent = path.parent.lstat()
    if (
        not stat.S_ISREG(info.st_mode)
        or info.st_uid != os.getuid()
        or info.st_nlink != 1
        or path.parent.resolve(strict=True) != path.parent
        or not stat.S_ISDIR(parent.st_mode)
        or parent.st_uid != os.getuid()
        or not os.access(path.parent, os.W_OK)
    ):
        raise VeloxError(
            "Move the AppImage to a writable folder owned by your account before updating."
        )
    return path


class Updater:
    def __init__(self, paths):
        self.paths, self.cancel = paths, threading.Event()

    def apply(self, release, report=lambda _message, _fraction: None):
        check_cancel(self.cancel)
        # Recheck the signed bytes at the mutation boundary, independent of the UI.
        release = verify(release.manifest)
        if version(release.version) <= version(current_version()):
            raise VeloxError("Velox is already up to date.")
        current = appimage()
        with lease(self.paths.locks / "appimage-update.lock"):
            source = os.open(current, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
            try:
                before = os.fstat(source)
                if not stat.S_ISREG(before.st_mode) or before.st_nlink != 1:
                    raise VeloxError("The AppImage changed before updating. Try again.")
                if (
                    shutil.disk_usage(current.parent).free
                    < release.size + before.st_size + 256 * 1024**2
                ):
                    raise VeloxError(
                        "There isn't enough free space to retain the previous AppImage safely."
                    )
                # Keep the old image beside the new one. The installed filename changes
                # only once, after the complete signed download and backup are durable.
                folder = Path(tempfile.mkdtemp(prefix=".velox-update-", dir=current.parent))
                target, previous = folder / "Velox.AppImage", folder / "Previous.AppImage"
                download(release.package, target, report, self.cancel)
                header = bounded_header(target)
                if header[:4] != b"\x7fELF" or header[8:11] != b"AI\x02":
                    raise VeloxError("The verified download is not a supported AppImage.")
                report("Retaining the previous AppImage", 0.98)
                digest = hashlib.sha256()
                with previous.open("xb") as output:
                    while block := os.read(source, 1024 * 1024):
                        check_cancel(self.cancel)
                        digest.update(block)
                        output.write(block)
                    output.flush()
                    os.fsync(output.fileno())
                if identity(os.fstat(source)) != identity(before) or identity(
                    current.lstat()
                ) != identity(before):
                    raise VeloxError("The AppImage changed before updating. Try again.")
                os.chmod(previous, stat.S_IMODE(before.st_mode) & 0o777)
                os.chmod(target, 0o755)
                atomic_json(
                    folder / "update.json",
                    dict(
                        format="velox-appimage-update",
                        version=1,
                        target=str(current),
                        release=release.version,
                        previous_sha256=digest.hexdigest(),
                        current_sha256=release.sha256,
                    ),
                )
                check_cancel(self.cancel)
                descriptor = os.open(target, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
                try:
                    if hash_descriptor(descriptor, self.cancel) != release.sha256:
                        raise VeloxError("The prepared update changed. Nothing was installed.")
                finally:
                    os.close(descriptor)
                os.replace(target, current)
                directory = os.open(current.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
                try:
                    os.fsync(directory)
                finally:
                    os.close(directory)
                report("Update installed. Restart Velox when ready.", 1)
                return current, previous
            finally:
                os.close(source)


def bounded_header(path):
    with path.open("rb") as source:
        return source.read(16)
