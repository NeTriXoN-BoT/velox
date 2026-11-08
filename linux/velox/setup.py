from __future__ import annotations

import hashlib
import json
import os
import shutil
import ssl
import stat
import tempfile
import threading
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath

from .core import (
    Cancelled,
    Paths,
    VeloxError,
    atomic_json,
    bounded_read,
    file_hash,
    inside,
    lease,
    private_directory,
    read_json,
)
from .fileops import hash_descriptor, identity


def resource_root() -> Path:
    return Path(os.environ.get("VELOX_RESOURCES", Path(__file__).resolve().parents[1] / "assets"))


def sdk_lock() -> dict:
    return json.loads(bounded_read(resource_root() / "sdk.lock.json"))


class HttpsOnly(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, newurl):
        if urllib.parse.urlsplit(newurl).scheme != "https":
            raise VeloxError("An insecure download redirect was rejected.")
        return super().redirect_request(request, fp, code, message, headers, newurl)


def opener():
    import certifi

    certificate = os.environ.get("SSL_CERT_FILE") or certifi.where()
    context = ssl.create_default_context(cafile=certificate)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    return urllib.request.build_opener(HttpsOnly(), urllib.request.HTTPSHandler(context=context))


def fetch(url: str, maximum: int = 2 * 1024 * 1024) -> bytes:
    if urllib.parse.urlsplit(url).scheme != "https":
        raise VeloxError("Only HTTPS downloads are allowed.")
    try:
        request = urllib.request.Request(
            url, headers={"User-Agent": "Velox/Linux", "Accept-Encoding": "identity"}
        )
        with opener().open(request, timeout=25) as response:
            data = response.read(maximum + 1)
            if len(data) > maximum:
                raise VeloxError("The server response is too large.")
            return data
    except (OSError, urllib.error.URLError) as error:
        raise VeloxError(
            "The download could not be completed. Check your connection and try again."
        ) from error


def download(package: dict, path: Path, report, cancel: threading.Event) -> None:
    if urllib.parse.urlsplit(package["url"]).scheme != "https":
        raise VeloxError("Only HTTPS downloads are allowed.")
    private_directory(path.parent)
    with lease(path.with_name(path.name + ".download-lock")):
        _download(package, path, report, cancel)


def download_info(path):
    try:
        info = path.lstat()
    except FileNotFoundError:
        return None
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_nlink != 1:
        raise VeloxError("Download files must be private regular files without links.")
    return info


def publish_download(package, path, partial, target, cancel):
    target.flush()
    os.fsync(target.fileno())
    sealed = os.fstat(target.fileno())
    if not stat.S_ISREG(sealed.st_mode) or sealed.st_nlink != 1 or sealed.st_uid != os.getuid():
        raise VeloxError("Download files must be private regular files without links.")
    if (
        sealed.st_size != package["bytes"]
        or hash_descriptor(target.fileno(), cancel) != package["sha256"]
    ):
        current = partial.lstat()
        if (current.st_dev, current.st_ino) == (sealed.st_dev, sealed.st_ino):
            partial.unlink()
        raise VeloxError("The download checksum did not match. The file was not installed.")
    current = download_info(partial)
    if (
        current is None
        or identity(current) != identity(sealed)
        or identity(os.fstat(target.fileno())) != identity(sealed)
    ):
        raise VeloxError("The partial download changed before publishing.")
    os.replace(partial, path)
    published = download_info(path)
    if published is None or (published.st_dev, published.st_ino) != (sealed.st_dev, sealed.st_ino):
        raise VeloxError("The partial download changed before publishing.")
    directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)


def _download(package, path, report, cancel):
    if cancel.is_set():
        raise Cancelled("Setup paused. The verified partial download can resume later.")
    cached = download_info(path)
    if (
        cached is not None
        and cached.st_size == package["bytes"]
        and file_hash(path, cancel) == package["sha256"]
    ):
        return
    partial = path.with_name(path.name + ".part")
    initial = download_info(partial)
    existing = initial.st_size if initial is not None else 0
    if existing > package["bytes"]:
        raise VeloxError(
            "A partial download has an unexpected size. Remove it from Setup downloads and try again."
        )
    if initial is not None and existing == package["bytes"]:
        with os.fdopen(
            os.open(partial, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK), "rb"
        ) as target:
            if identity(os.fstat(target.fileno())) != identity(initial):
                raise VeloxError("The partial download changed before publishing.")
            publish_download(package, path, partial, target, cancel)
        report(package["name"], 1.0)
        return
    headers = {"User-Agent": "Velox/Linux", "Accept-Encoding": "identity"}
    if existing:
        headers["Range"] = f"bytes={existing}-"
    try:
        request = urllib.request.Request(package["url"], headers=headers)
        with opener().open(request, timeout=30) as response:
            if existing and response.status != 206:
                existing = 0
            if existing and not response.headers.get("Content-Range", "").startswith(
                f"bytes {existing}-"
            ):
                raise VeloxError("The server returned an invalid resumed download.")
            flags = (
                os.O_RDWR
                | os.O_CLOEXEC
                | os.O_NOFOLLOW
                | os.O_NONBLOCK
                | (os.O_CREAT | os.O_EXCL if initial is None else 0)
            )
            with os.fdopen(os.open(partial, flags, 0o600), "r+b") as target:
                opened = os.fstat(target.fileno())
                if (
                    not stat.S_ISREG(opened.st_mode)
                    or opened.st_uid != os.getuid()
                    or opened.st_nlink != 1
                    or (initial is not None and identity(initial) != identity(opened))
                ):
                    raise VeloxError(
                        "The partial download changed before writing. No file was modified."
                    )
                os.fchmod(target.fileno(), 0o600)
                if not existing:
                    target.truncate(0)
                target.seek(existing)
                done = existing
                while block := response.read(1024 * 1024):
                    if cancel.is_set():
                        raise Cancelled(
                            "Setup paused. The verified partial download can resume later."
                        )
                    done += len(block)
                    if done > package["bytes"]:
                        raise VeloxError("The download exceeds its expected size.")
                    target.write(block)
                    report(package["name"], done / package["bytes"])
                publish_download(package, path, partial, target, cancel)
    except (urllib.error.URLError, OSError) as error:
        raise VeloxError("The download was interrupted. Setup can resume it safely.") from error


def unpack(
    archive: Path, destination: Path, cancel: threading.Event, max_bytes=8 * 1024**3
) -> None:
    """Extract a verified SDK archive without permitting path or symlink escapes."""
    private_directory(destination)
    with zipfile.ZipFile(archive) as source:
        members = source.infolist()
        if len(members) > 50000 or sum(item.file_size for item in members) > max_bytes:
            raise VeloxError("The archive expands beyond its permitted size.")
        names = set()
        links = []
        for item in members:
            name = PurePosixPath(item.filename)
            if (
                name.is_absolute()
                or ".." in name.parts
                or "\\" in item.filename
                or ":" in item.filename
                or "\x00" in item.filename
            ):
                raise VeloxError("An unsafe archive path was rejected.")
            if item.filename.rstrip("/") in names:
                raise VeloxError("A repeated archive path was rejected.")
            names.add(item.filename.rstrip("/"))
            target = inside(destination.joinpath(*name.parts), destination)
            mode = item.external_attr >> 16
            if item.is_dir():
                private_directory(target)
                continue
            if stat.S_ISLNK(mode):
                link = source.read(item).decode("utf-8")
                if len(link) > 4096 or Path(link).is_absolute():
                    raise VeloxError("An unsafe archive link was rejected.")
                resolved = Path(os.path.normpath(target.parent / link))
                inside(resolved, destination)
                links.append((target, link))
                continue
            if stat.S_IFMT(mode) not in (0, stat.S_IFREG):
                raise VeloxError("An unsupported archive entry was rejected.")
            private_directory(target.parent)
            flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW
            with (
                source.open(item) as incoming,
                os.fdopen(
                    os.open(target, flags, 0o700 if mode & 0o111 else 0o600), "wb"
                ) as outgoing,
            ):
                copied = 0
                while chunk := incoming.read(1024 * 1024):
                    if cancel.is_set():
                        raise Cancelled("Setup cancelled. Existing Android files are unchanged.")
                    copied += len(chunk)
                    if copied > item.file_size:
                        raise VeloxError("An archive entry exceeded its declared size.")
                    outgoing.write(chunk)
                if copied != item.file_size:
                    raise VeloxError("An archive entry was truncated.")
        for target, link in links:
            private_directory(target.parent)
            os.symlink(link, target)
        for target, _ in links:
            try:
                resolved = target.resolve(strict=True)
            except (OSError, RuntimeError) as error:
                raise VeloxError("An invalid archive link was rejected.") from error
            if destination.resolve() not in resolved.parents:
                raise VeloxError("An archive link leaves the SDK folder.")


class Setup:
    def __init__(self, paths: Paths):
        self.paths = paths
        self.cancel = threading.Event()
        self.lock = sdk_lock()

    def accepted(self) -> bool:
        value = read_json(self.paths.root / "sdk-license.json", {})
        return value.get("sha256") == self.lock["license_sha256"]

    def accept(self) -> None:
        license_text = bounded_read(resource_root() / "android-sdk-license.txt", 256 * 1024)
        if hashlib.sha256(license_text).hexdigest() != self.lock["license_sha256"]:
            raise VeloxError("The Android license does not match this build.")
        atomic_json(self.paths.root / "sdk-license.json", {"sha256": self.lock["license_sha256"]})

    def installed(self) -> bool:
        expected = self.lock["engine"] + "/" + self.lock["image"]
        value = read_json(self.paths.sdk / ".velox-sdk.json", {})
        return (
            value.get("identity") == expected
            and (self.paths.sdk / "emulator/emulator").is_file()
            and (self.paths.sdk / "platform-tools/adb").is_file()
            and (
                self.paths.sdk / "system-images/android-35/google_apis_playstore/x86_64/system.img"
            ).is_file()
        )

    def verify_executable(self, relative: str) -> Path:
        if relative not in (
            "emulator/emulator",
            "platform-tools/adb",
            "emulator/qemu/linux-x86_64/qemu-system-x86_64",
            "emulator/qemu/linux-x86_64/qemu-system-x86_64-headless",
            "emulator/qemu-img",
        ):
            raise VeloxError("Unsupported SDK executable.")
        path = inside(self.paths.sdk / relative, self.paths.sdk)
        record = read_json(self.paths.sdk / ".velox-sdk.json", {})
        files = record.get("executables", {})
        expected = files.get(relative)
        if not isinstance(expected, dict):
            raise VeloxError("Android setup must finish before this program can be used.")
        info = path.stat()
        stamp = [
            info.st_dev,
            info.st_ino,
            info.st_size,
            info.st_mtime_ns,
            info.st_ctime_ns,
            info.st_nlink,
        ]
        # A process-local cache avoids rehashing an emulator executable for every ADB command.
        cached = getattr(self, "_verified", {})
        if cached.get(relative) != stamp:
            if file_hash(path) != expected.get("sha256"):
                raise VeloxError(
                    "An Android executable changed. Repair Android setup before starting the phone."
                )
            cached[relative] = stamp
            self._verified = cached
        return path

    def install(self, report=lambda _message, _fraction: None) -> None:
        if not self.accepted():
            raise VeloxError("Read and accept Google's Android SDK license before setup.")
        if self.paths.sdk.exists():
            if self.installed():
                return
            raise VeloxError(
                "An incomplete SDK already exists. Use Setup repair to retain it and install a verified copy."
            )
        with lease(self.paths.locks / "sdk.lock"):
            archives = []
            for index, package in enumerate(self.lock["packages"]):
                path = self.paths.cache / package["url"].rsplit("/", 1)[-1]
                download(
                    package,
                    path,
                    lambda name, fraction: report(name, (index + fraction) / 4),
                    self.cancel,
                )
                archives.append((package, path))
            private_directory(self.paths.sdk.parent)
            staging = Path(tempfile.mkdtemp(prefix=".sdk-", dir=self.paths.sdk.parent))
            try:
                for index, (package, archive) in enumerate(archives):
                    report("Extracting " + package["name"], 0.75 + 0.07 * index)
                    target = staging / package["into"] if package["into"] else staging
                    unpack(archive, target, self.cancel)
                image = staging / "system-images/android-35/google_apis_playstore/x86_64"
                if not (image / "system.img").is_file():
                    raise VeloxError("The Android system image was not extracted correctly.")
                record = {
                    "identity": self.lock["engine"] + "/" + self.lock["image"],
                    "executables": {},
                }
                for relative in (
                    "emulator/emulator",
                    "platform-tools/adb",
                    "emulator/qemu/linux-x86_64/qemu-system-x86_64",
                    "emulator/qemu/linux-x86_64/qemu-system-x86_64-headless",
                    "emulator/qemu-img",
                ):
                    target = staging / relative
                    if not target.is_file() or target.is_symlink():
                        raise VeloxError("A required Android executable is missing.")
                    record["executables"][relative] = {"sha256": file_hash(target, self.cancel)}
                fingerprint = hashlib.sha256(
                    bounded_read(image / "source.properties", 128 * 1024)
                    + bounded_read(image / "build.prop", 256 * 1024)
                ).hexdigest()
                record["image"] = fingerprint
                atomic_json(staging / ".velox-sdk.json", record)
                if self.cancel.is_set():
                    raise Cancelled("Setup cancelled. Existing phones are unchanged.")
                os.rename(staging, self.paths.sdk)
                report("Android is ready.", 1.0)
            finally:
                if staging.exists():
                    shutil.rmtree(staging)

    def repair(self, report) -> None:
        with lease(self.paths.locks / "sdk.lock"):
            from .phone import Hub

            hub = Hub(self.paths)
            if any(item.get("running") or item.get("starting") for item in hub.list()):
                raise VeloxError("Stop all phones before repairing the Android engine.")
            if self.paths.sdk.exists():
                if self.paths.sdk.is_symlink():
                    raise VeloxError("Linked SDK folders cannot be repaired.")
                previous = self.paths.sdk.with_name("Sdk-previous")
                if previous.exists():
                    raise VeloxError(
                        "A previous SDK copy is retained. Move it elsewhere before repairing again."
                    )
                os.rename(self.paths.sdk, previous)
        self.install(report)
