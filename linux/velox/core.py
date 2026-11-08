from __future__ import annotations

import contextlib
import dataclasses
import fcntl
import hashlib
import json
import os
import re
import stat
import tempfile
import time
from pathlib import Path
from typing import Any, Iterator


class VeloxError(Exception):
    """An actionable failure suitable for display in the application."""


class Cancelled(VeloxError):
    pass


class Busy(VeloxError):
    pass


INSTANCE = re.compile(r"(?:main|[a-f0-9]{32})\Z")
PACKAGE = re.compile(r"[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z0-9_]+)+\Z")
COMPONENT = re.compile(r"[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z0-9_]+)+/[A-Za-z0-9_.$]+\Z")
DISPLAYS = (
    ("HD+", 720, 1600, 320),
    ("FHD", 1080, 1920, 420),
    ("FHD+", 1080, 2400, 420),
    ("QHD+", 1440, 3200, 560),
    ("Fold", 1768, 2208, 420),
    ("Tablet", 1920, 1200, 240),
)
CPU_CHOICES = (0, 2, 4, 6, 8, 12, 16, 24, 32, 48, 64)
RAM_CHOICES = (0, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64)


# Portable profiles retain the historic integer 2. This image actually needs
# 2560 MiB; present that minimum honestly and round up only for host budgets.
def memory_mib(choice: int) -> int:
    return max(2560, choice * 1024)


def memory_label(choice: int, language="en") -> str:
    return ("2,5" if language == "de" else "2.5") if choice == 2 else str(choice)


RATES = (30, 60, 90, 120, 144)
MAX_JSON = 2 * 1024 * 1024


def private_directory(path: Path) -> Path:
    path = Path(path).absolute()
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid():
        raise VeloxError("Choose a private folder owned by your account.")
    if path.is_symlink():
        raise VeloxError("A linked folder cannot be used for private phone data.")
    if stat.S_IMODE(info.st_mode) != 0o700:
        os.chmod(path, 0o700, follow_symlinks=False)
        secured = path.lstat()
        if (
            not stat.S_ISDIR(secured.st_mode)
            or secured.st_uid != os.getuid()
            or stat.S_IMODE(secured.st_mode) != 0o700
        ):
            raise VeloxError(
                "This filesystem cannot protect private phone folders. Choose a local Linux filesystem."
            )
    return path


def regular(path: Path, *, maximum: int | None = None, owner: bool = False) -> os.stat_result:
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or (owner and info.st_uid != os.getuid()):
        raise VeloxError("A regular file is required.")
    if maximum is not None and info.st_size > maximum:
        raise VeloxError("This file is too large.")
    return info


def bounded_read(path: Path, maximum: int = MAX_JSON) -> bytes:
    with os.fdopen(
        os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK), "rb"
    ) as source:
        info = os.fstat(source.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_size > maximum:
            raise VeloxError("This file is not a supported regular file or is too large.")
        content = source.read(maximum + 1)
        if len(content) > maximum:
            raise VeloxError("This file is too large.")
        return content


def read_json(path: Path, default: Any = None, maximum: int = MAX_JSON) -> Any:
    try:

        def unique(pairs):
            result = {}
            for key, value in pairs:
                if key in result:
                    raise ValueError("Repeated JSON key")
                result[key] = value
            return result

        return json.loads(
            bounded_read(path, maximum),
            object_pairs_hook=unique,
            parse_constant=lambda _: (_ for _ in ()).throw(ValueError("Nonfinite number")),
        )
    except FileNotFoundError:
        return default
    except (ValueError, UnicodeError, RecursionError) as error:
        raise VeloxError("The saved file is damaged or has an unsupported format.") from error


def atomic_bytes(path: Path, data: bytes, mode: int = 0o600) -> None:
    private_directory(path.parent)
    descriptor, temporary = tempfile.mkstemp(prefix="." + path.name + ".", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as target:
            os.fchmod(target.fileno(), mode)
            target.write(data)
            target.flush()
            os.fsync(target.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        with contextlib.suppress(FileNotFoundError):
            os.unlink(temporary)


def atomic_json(path: Path, data: Any) -> None:
    atomic_bytes(
        path,
        (
            json.dumps(data, ensure_ascii=False, sort_keys=True, indent=2, allow_nan=False) + "\n"
        ).encode(),
    )


def file_hash(path: Path, cancel=None) -> str:
    digest = hashlib.sha256()
    with os.fdopen(
        os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK), "rb"
    ) as source:
        before = os.fstat(source.fileno())
        if not stat.S_ISREG(before.st_mode) or before.st_nlink != 1:
            raise VeloxError("Choose a regular file without hard links.")
        while block := source.read(1024 * 1024):
            if cancel and cancel.is_set():
                raise Cancelled("Cancelled. Your original phone is unchanged.")
            digest.update(block)
        after = os.fstat(source.fileno())
        if (
            before.st_dev,
            before.st_ino,
            before.st_size,
            before.st_mtime_ns,
            before.st_ctime_ns,
            before.st_nlink,
        ) != (
            after.st_dev,
            after.st_ino,
            after.st_size,
            after.st_mtime_ns,
            after.st_ctime_ns,
            after.st_nlink,
        ):
            raise VeloxError(
                "A file changed while it was being verified. Stop the phone and try again."
            )
    return digest.hexdigest()


@contextlib.contextmanager
def lease(path: Path, blocking: bool = False, *, shared: bool = False) -> Iterator[None]:
    private_directory(path.parent)
    descriptor = os.open(
        path, os.O_CREAT | os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600
    )
    try:
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_nlink != 1:
            raise VeloxError("The phone lock does not belong to your account.")
        try:
            fcntl.flock(
                descriptor,
                (fcntl.LOCK_SH if shared else fcntl.LOCK_EX) | (0 if blocking else fcntl.LOCK_NB),
            )
        except BlockingIOError as error:
            raise Busy("This phone is already open or is undergoing maintenance.") from error
        yield
    finally:
        os.close(descriptor)


def inside(path: Path, root: Path, *, allow_root: bool = False) -> Path:
    path, root = Path(path).absolute(), Path(root).absolute()
    if ".." in path.parts or ".." in root.parts:
        raise VeloxError("Parent-directory paths are not permitted.")
    if (path == root and not allow_root) or (path != root and root not in path.parents):
        raise VeloxError("The selected path is outside the permitted phone folder.")
    current = root
    if current.is_symlink():
        raise VeloxError("Linked phone folders are not supported.")
    for part in path.relative_to(root).parts:
        current /= part
        if current.is_symlink():
            raise VeloxError("Linked files are not supported in phone storage.")
    return path


def safe_name(value: str) -> str:
    value = value.strip()
    if not 1 <= len(value) <= 40 or any(ord(char) < 32 or char in "[]=\x7f" for char in value):
        raise VeloxError("Use 1–40 characters without line breaks or INI symbols.")
    return value


def package(value: str) -> str:
    if not isinstance(value, str) or len(value) > 199 or not PACKAGE.fullmatch(value):
        raise VeloxError("Invalid Android package name.")
    return value


@dataclasses.dataclass
class Settings:
    display: int = 2
    rate: int = 120
    cores: int = 0
    memory: int = 0
    gpu: str = "auto"
    root: bool = False
    snappy: bool = True
    boost: bool = False
    quick_boot: bool = False
    boot_screen: bool = True
    frame: bool = True
    auto_update: bool = True
    show_touches: bool = False
    clipboard_shortcuts: bool = True
    auto_rotate: bool = True
    auto_start: bool = True
    navigation: int = 0
    brand: int = 2
    boot_style: int = 0
    cursor: int = 0
    number: str = ""
    language: str = "en"
    background_rate: int = 0
    background_mute: bool = False
    ui_percent: int = 0
    phone_percent: int = 0

    @classmethod
    def from_dict(cls, value: Any) -> Settings:
        result = cls()
        if not isinstance(value, dict):
            raise VeloxError("Invalid phone preferences.")
        for field in dataclasses.fields(cls):
            candidate = value.get(field.name, getattr(result, field.name))
            if type(candidate) is not type(getattr(result, field.name)):
                raise VeloxError("Invalid phone preference: " + field.name)
            setattr(result, field.name, candidate)
        if result.display not in range(len(DISPLAYS)) or result.rate not in RATES:
            raise VeloxError("Unsupported Android display preference.")
        if (
            result.cores not in CPU_CHOICES
            or result.memory not in RAM_CHOICES
            or result.gpu not in ("auto", "host", "software")
        ):
            raise VeloxError("Unsupported hardware preference.")
        if (
            result.navigation not in (0, 1, 2)
            or result.brand not in range(4)
            or result.boot_style not in range(6)
            or result.cursor not in range(11)
        ):
            raise VeloxError("Unsupported appearance preference.")
        if not re.fullmatch(r"\+?[0-9]{0,20}", result.number) or result.language not in (
            "en",
            "de",
        ):
            raise VeloxError("Invalid language or phone number.")
        if result.background_rate not in (0, 15, 30, 60):
            result.background_rate = 0
        if result.ui_percent and not 75 <= result.ui_percent <= 300:
            result.ui_percent = 0
        if result.phone_percent and not 10 <= result.phone_percent <= 100:
            result.phone_percent = 0
        return result

    def as_dict(self) -> dict:
        return dataclasses.asdict(self)

    def signature(self, actual_cores: int, actual_memory: int) -> str:
        fields = (
            self.display,
            self.rate,
            actual_cores,
            actual_memory,
            self.gpu,
            self.root,
            self.frame,
        )
        return hashlib.sha256(json.dumps(fields).encode()).hexdigest()


class Paths:
    def __init__(self, root: Path | None = None):
        default = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")) / "velox"
        # /home can be a distribution-managed alias (for example /var/home).
        # Process ownership, data markers and relocated storage need one path.
        self.root = private_directory(root or default).resolve(strict=True)
        self.cache = private_directory(self.root / "cache")
        self.phones = private_directory(self.root / "phones")
        self.sdk = self.root / "Android/Sdk"
        self.vault = private_directory(self.root / "Recovery")
        self.locks = private_directory(self.root / "locks")

    def phone(self, identity: str = "main") -> Path:
        if not INSTANCE.fullmatch(identity):
            raise VeloxError("Invalid phone identity.")
        return inside(self.phones / identity, self.phones)

    def settings(self, identity: str = "main") -> Settings:
        data = read_json(self.phone(identity) / "settings.json", {})
        return Settings.from_dict(data)

    def save(self, identity: str, settings: Settings) -> None:
        Settings.from_dict(settings.as_dict())
        target = self.phone(identity) / "settings.json"
        with lease(self.locks / (identity + ".settings"), blocking=True):
            atomic_json(target, settings.as_dict())

    def save_if_current(self, identity: str, previous: Settings, settings: Settings) -> bool:
        Settings.from_dict(settings.as_dict())
        target = self.phone(identity) / "settings.json"
        with lease(self.locks / (identity + ".settings"), blocking=True):
            if self.settings(identity) != previous:
                return False
            atomic_json(target, settings.as_dict())
            return True

    def metadata(self, identity: str) -> dict:
        data = read_json(self.phone(identity) / "phone.json", {})
        return validate_metadata(identity, data)

    def data_home(self, identity: str, *, metadata=None) -> Path:
        metadata = (
            self.metadata(identity) if metadata is None else validate_metadata(identity, metadata)
        )
        home = metadata.get("home")
        if home:
            result = Path(home)
            if (
                not result.is_absolute()
                or result.resolve() != result
                or result.is_symlink()
                or result == self.root
                or self.sdk in result.parents
                or any(ord(c) < 32 for c in str(result))
            ):
                raise VeloxError("Invalid relocated phone folder.")
            marker = read_json(result / ".velox-owner.json", {})
            if marker != {
                "application": "velox",
                "phone": identity,
                "root": str(self.root.resolve()),
            }:
                raise VeloxError("This storage folder does not belong to the selected phone.")
            private_directory(result)
            return result
        return self.phone(identity) / "Android"


def validate_metadata(identity, data):
    if not INSTANCE.fullmatch(identity):
        raise VeloxError("Invalid phone identity.")
    if (
        not isinstance(data, dict)
        or data.get("id") != identity
        or type(data.get("port")) is not int
    ):
        raise VeloxError("This phone's registration is damaged.")
    port = data["port"]
    if (
        port not in range(5556, 5684, 2)
        or type(data.get("grpc")) is not int
        or data["grpc"] != port + 1000
    ):
        raise VeloxError("Invalid phone connection ports.")
    if (
        not isinstance(data.get("name"), str)
        or safe_name(data["name"]) != data["name"]
        or not isinstance(data.get("home", ""), str)
        or len(data.get("home", "")) > 4096
    ):
        raise VeloxError("This phone's registration is damaged.")
    return data


def proc_identity(pid: int) -> tuple[int, str, int] | None:
    try:
        text = Path(f"/proc/{pid}/stat").read_text()
        end = text.rindex(")")
        start_time = int(text[end + 2 :].split()[19])
        uid = Path(f"/proc/{pid}").stat().st_uid
        return uid, os.readlink(f"/proc/{pid}/exe"), start_time
    except (OSError, ValueError, IndexError):
        return None


def now() -> int:
    return int(time.time())
