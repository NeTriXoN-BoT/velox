from __future__ import annotations

import contextlib
import dataclasses
import hashlib
import json
import os
import re
import selectors
import secrets
import shlex
import signal
import socket
import subprocess
import threading
import tempfile
import time
import uuid
from pathlib import Path

from .core import (
    COMPONENT,
    DISPLAYS,
    INSTANCE,
    Paths,
    Settings,
    VeloxError,
    Cancelled,
    atomic_bytes,
    atomic_json,
    bounded_read,
    inside,
    lease,
    memory_mib,
    now,
    private_directory,
    proc_identity,
    read_json,
    safe_name,
)
from .host import allocations, detect, kvm_available, recommended
from .capabilities import cpu_count, record_cpu_limit
from .avdlock import AvdLock
from .logs import open_log
from .setup import Setup


def clean_environment() -> dict[str, str]:
    environment = dict(os.environ)
    # AppImage's Qt/Python library paths must not leak into Google's own Qt, GPU
    # loader, system file dialogs or command-line tools.
    for key in (
        "LD_LIBRARY_PATH",
        "LD_PRELOAD",
        "PYTHONHOME",
        "PYTHONPATH",
        "QT_PLUGIN_PATH",
        "QT_QPA_PLATFORM_PLUGIN_PATH",
    ):
        environment.pop(key, None)
    environment["PATH"] = "/usr/local/bin:/usr/bin:/bin"
    return environment


def bounded_process(
    arguments: list[str],
    *,
    environment=None,
    data=None,
    timeout=15,
    maximum=4 * 1024 * 1024,
    cancel=None,
) -> bytes:
    """Run only an argv list; bound output and terminate only this owned child."""
    if timeout <= 0 or maximum < 1 or (data is not None and not isinstance(data, bytes)):
        raise ValueError("Invalid subprocess bounds")
    if cancel is not None and cancel.is_set():
        raise Cancelled("Cancelled. Your original phone is unchanged.")
    process = subprocess.Popen(
        arguments,
        stdin=subprocess.PIPE if data is not None else subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=environment,
        close_fds=True,
    )
    output = bytearray()
    deadline = time.monotonic() + timeout
    try:
        with selectors.DefaultSelector() as selector:
            os.set_blocking(process.stdout.fileno(), False)
            selector.register(process.stdout, selectors.EVENT_READ)
            offset = 0
            if data:
                os.set_blocking(process.stdin.fileno(), False)
                selector.register(process.stdin, selectors.EVENT_WRITE)
            elif process.stdin:
                process.stdin.close()
            while selector.get_map():
                if cancel is not None and cancel.is_set():
                    raise Cancelled("Cancelled. Your original phone is unchanged.")
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise VeloxError(
                        "Android did not respond in time. Try again or use Startup repair."
                    )
                for key, _mask in selector.select(min(0.2, remaining)):
                    if key.fileobj is process.stdout:
                        block = os.read(key.fd, min(65536, maximum + 1 - len(output)))
                        if not block:
                            selector.unregister(key.fileobj)
                        else:
                            output.extend(block)
                            if len(output) > maximum:
                                raise VeloxError("Android returned too much data.")
                    else:
                        try:
                            offset += os.write(key.fd, data[offset : offset + 65536])
                        except BrokenPipeError:
                            offset = len(data)
                        if offset == len(data):
                            selector.unregister(key.fileobj)
                            process.stdin.close()
            try:
                process.wait(timeout=max(0.01, deadline - time.monotonic()))
            except subprocess.TimeoutExpired as error:
                raise VeloxError(
                    "Android did not respond in time. Try again or use Startup repair."
                ) from error
        result = bytes(output)
        if process.returncode:
            raise VeloxError(
                result.decode("utf-8", errors="replace").strip()[-1200:]
                or "The Android command failed."
            )
        return result
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        process.stdout.close()
        if process.stdin:
            process.stdin.close()


class Hub:
    def __init__(self, paths: Paths):
        self.paths = paths
        self.setup = Setup(paths)
        self.hardware = detect(paths.root)
        self.hardware_probed = False
        runtime_base = Path(os.environ.get("XDG_RUNTIME_DIR", "/tmp"))
        try:
            valid_runtime = (
                os.environ.get("XDG_RUNTIME_DIR")
                and runtime_base.is_dir()
                and not runtime_base.is_symlink()
                and runtime_base.stat().st_uid == os.getuid()
            )
        except OSError:
            valid_runtime = False
        root_identity = hashlib.sha256(str(paths.root.resolve()).encode()).hexdigest()[:16]
        try:
            runtime_parent = private_directory(
                runtime_base / "velox" if valid_runtime else Path("/tmp") / f"velox-{os.getuid()}"
            )
            self.runtime = private_directory(runtime_parent / root_identity)
        except (VeloxError, OSError):
            self.runtime = private_directory(Path("/tmp") / f"velox-{os.getuid()}" / root_identity)
        self.adb_socket = self.runtime / "adb.sock"
        if len(os.fsencode(self.adb_socket)) > 100:
            self.runtime = private_directory(Path("/tmp") / f"velox-{os.getuid()}" / root_identity)
            self.adb_socket = self.runtime / "adb.sock"
        self._phones: dict[str, Phone] = {}
        self._lock = threading.RLock()
        self.registration = None
        if not (paths.phone("main") / "phone.json").exists():
            self.create("Main phone", main=True)
        from .recovery import reconcile_all

        self.recovery_errors = reconcile_all(self)

    def phone(self, identity="main") -> Phone:
        with self._lock:
            if identity not in self._phones:
                self.paths.metadata(identity)
                self._phones[identity] = Phone(self, identity)
            return self._phones[identity]

    def list(self) -> list[dict]:
        result = []
        for path in sorted(self.paths.phones.iterdir(), key=lambda p: (p.name != "main", p.name)):
            if not INSTANCE.fullmatch(path.name) or path.is_symlink() or not path.is_dir():
                continue
            try:
                value = self.paths.metadata(path.name)
                phone = self.phone(path.name)
                value["running"] = phone.alive()
                value["starting"] = phone.state == "starting"
                value["settings"] = self.paths.settings(path.name).as_dict()
                result.append(value)
            except VeloxError as error:
                result.append(
                    {"id": path.name, "name": path.name, "error": str(error), "running": False}
                )
        return result

    def create(self, name: str, main=False, template: str | None = None) -> str:
        name = safe_name(name)
        with lease(self.paths.locks / "catalog.lock", blocking=True):
            if main and (self.paths.phone("main") / "phone.json").exists():
                self.paths.metadata("main")
                self.paths.settings("main")
                return "main"
            registrations = []
            for folder in self.paths.phones.iterdir():
                if (
                    INSTANCE.fullmatch(folder.name)
                    and folder.is_dir()
                    and not folder.is_symlink()
                    and (folder / "phone.json").exists()
                ):
                    registrations.append(self.paths.metadata(folder.name))
            if len(registrations) >= 9:
                raise VeloxError("Velox supports the main phone and up to eight additional phones.")
            used = {item["port"] for item in registrations}
            port = next(
                (n for n in range(5556, 5684, 2) if n not in used and self._ports_free(n)), None
            )
            if port is None:
                raise VeloxError("No free phone connection ports are available.")
            identity = "main" if main else uuid.uuid4().hex
            folder = self.paths.phone(identity)
            if folder.exists():
                raise VeloxError("This phone already exists.")
            settings = self.paths.settings(template) if template else self.recommendations()
            settings.auto_start = main
            if template:
                settings.root = settings.quick_boot = False
                settings.number = ""
            else:
                settings.cores = settings.memory = 0
            with tempfile.TemporaryDirectory(
                prefix=".creating-", dir=self.paths.phones
            ) as temporary:
                staging = Path(temporary)
                if template:
                    for subfolder in ("controls", "presets"):
                        source = inside(self.paths.phone(template) / subfolder, self.paths.phones)
                        if source.exists():
                            for item in source.iterdir():
                                if (
                                    item.is_file()
                                    and not item.is_symlink()
                                    and item.suffix == ".json"
                                ):
                                    atomic_bytes(
                                        staging / subfolder / item.name, bounded_read(item, 65536)
                                    )
                atomic_json(
                    staging / "phone.json",
                    {
                        "id": identity,
                        "name": name,
                        "port": port,
                        "grpc": port + 1000,
                        "home": "",
                        "personalize": True,
                        "auto_plan": self._settings_fingerprint(settings)
                        if not template and not self.hardware_probed
                        else "",
                    },
                )
                atomic_json(staging / "settings.json", settings.as_dict())
                os.rename(staging, folder)
                directory = os.open(self.paths.phones, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
                try:
                    os.fsync(directory)
                finally:
                    os.close(directory)
            return identity

    @staticmethod
    def _settings_fingerprint(settings):
        return hashlib.sha256(
            json.dumps(settings.as_dict(), sort_keys=True, separators=(",", ":")).encode()
        ).hexdigest()

    def recommendations(self, identity=None, hardware=None):
        occupied_cpu = occupied_ram = 0
        for item in self.list():
            if item["id"] == identity or not (item.get("running") or item.get("starting")):
                continue
            record = read_json(self.paths.phone(item["id"]) / "runtime.json", {})
            occupied_cpu += int(record.get("cores", 0))
            occupied_ram += max(3, int(record.get("memory", 3)))
        return recommended(hardware or self.hardware, occupied_cpu, occupied_ram)

    def finish_initial_recommendations(self):
        changed = []
        with lease(self.paths.locks / "catalog.lock", blocking=True):
            for folder in self.paths.phones.iterdir():
                if (
                    not INSTANCE.fullmatch(folder.name)
                    or folder.is_symlink()
                    or not folder.is_dir()
                ):
                    continue
                try:
                    metadata = self.paths.metadata(folder.name)
                    current = self.paths.settings(folder.name)
                except (OSError, VeloxError):
                    continue
                expected = metadata.get("auto_plan")
                if not expected:
                    continue
                phone = self.phone(folder.name)
                if (
                    phone.state == "stopped"
                    and not phone.alive()
                    and expected == self._settings_fingerprint(current)
                ):
                    plan = self.recommendations(folder.name)
                    updated = dataclasses.replace(
                        current, **{key: getattr(plan, key) for key in ("display", "rate", "gpu")}
                    )
                    if self.paths.save_if_current(folder.name, current, updated):
                        changed.append(folder.name)
                metadata.pop("auto_plan", None)
                atomic_json(folder / "phone.json", metadata)
        return changed

    @staticmethod
    def _ports_free(port: int) -> bool:
        sockets = []
        try:
            for value in (port, port + 1, port + 1000):
                handle = socket.socket()
                sockets.append(handle)
                handle.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                handle.bind(("127.0.0.1", value))
            return True
        except OSError:
            return False
        finally:
            for handle in sockets:
                handle.close()

    def rename(self, identity: str, name: str) -> None:
        with lease(self.paths.locks / "catalog.lock", blocking=True):
            metadata = self.paths.metadata(identity)
            metadata["name"] = safe_name(name)
            atomic_json(self.paths.phone(identity) / "phone.json", metadata)

    def reserve(self, identity: str, settings: Settings) -> tuple[int, int]:
        with lease(self.paths.locks / "catalog.lock", blocking=True):
            metadata = self.paths.metadata(identity)
            used = {
                item["port"]
                for item in self.list()
                if item["id"] != identity and not item.get("error")
            }
            if metadata["port"] in used or not self._ports_free(metadata["port"]):
                port = next(
                    (
                        port
                        for port in range(5556, 5684, 2)
                        if port not in used and self._ports_free(port)
                    ),
                    None,
                )
                if port is None:
                    raise VeloxError("No free phone connection ports are available.")
                metadata.update(port=port, grpc=port + 1000)
                atomic_json(self.paths.phone(identity) / "phone.json", metadata)
        hardware = detect(
            self.paths.root, self.hardware.width, self.hardware.height, self.hardware.refresh
        )
        cpu, ram = allocations(settings, hardware)
        other_cpu, other_ram = 0, 0
        for item in self.list():
            if item["id"] == identity:
                continue
            runtime = read_json(self.paths.phone(item["id"]) / "runtime.json", {})
            if item.get("running") or item.get("starting"):
                other_cpu += int(runtime.get("cores", 0))
                other_ram += max(3, int(runtime.get("memory", 3)))
        # Use the same probed GPU budget as the recommendation shown in the UI.
        # Fresh availability/affinity/cgroup limits still come from this start.
        planning = dataclasses.replace(
            hardware, dedicated_mb=self.hardware.dedicated_mb, software=self.hardware.software
        )
        plan = recommended(planning, other_cpu, other_ram)
        if not settings.cores:
            cpu = min(cpu, plan.cores or 1)
        if not settings.memory:
            ram = min(ram, plan.memory)
        budget = max(1, hardware.threads - max(1, hardware.threads // 4))
        if not settings.cores:
            cpu = max(
                (n for n in (1, 2, 4, 6) if n <= min(cpu, max(1, budget - other_cpu))), default=1
            )
        if other_cpu and cpu + other_cpu > budget:
            raise VeloxError(
                "Other phones are using the available CPU budget. Lower their allocations or stop one."
            )
        memory_budget = min(hardware.memory_limit - other_ram, hardware.available - 1)
        if not settings.memory:
            ram = max(
                (n for n in (2, 3, 4, 6, 8) if n <= ram and max(3, n) <= memory_budget), default=0
            )
        if not ram or max(3, ram) > memory_budget:
            raise VeloxError(
                "There isn't enough free memory to start safely. Lower other phones' allocations or close unused apps."
            )
        return cpu, ram


class Phone:
    def __init__(self, hub: Hub, identity: str):
        self.hub, self.paths, self.identity = hub, hub.paths, identity
        self.folder = hub.paths.phone(identity)
        self.state = "running" if self.alive() else "stopped"
        self.cancel = threading.Event()
        self.running_settings: Settings | None = None
        self._state_lock = threading.RLock()
        self._policy_lock = threading.RLock()
        self._listeners = []
        self._input = None
        self._background = None
        self._refresh_generation = 0
        self._audio_id = ""

    @property
    def metadata(self) -> dict:
        return self.paths.metadata(self.identity)

    @property
    def home(self) -> Path:
        return self.paths.data_home(self.identity)

    @property
    def avd(self) -> Path:
        return self.home / "avd/Gaming_Phone.avd"

    @property
    def serial(self) -> str:
        return "emulator-" + str(self.metadata["port"])

    def environment(self) -> dict[str, str]:
        environment = clean_environment()
        desktop_runtime = Path(os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}"))
        wayland = environment.get("WAYLAND_DISPLAY", "")
        if wayland and not Path(wayland).is_absolute():
            environment["WAYLAND_DISPLAY"] = str(desktop_runtime / wayland)
        pulse = desktop_runtime / "pulse/native"
        if not environment.get("PULSE_SERVER") and pulse.is_socket():
            environment["PULSE_SERVER"] = "unix:" + str(pulse)
        cookie = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "pulse/cookie"
        if not environment.get("PULSE_COOKIE") and cookie.is_file():
            environment["PULSE_COOKIE"] = str(cookie)
        environment.setdefault("PIPEWIRE_RUNTIME_DIR", str(desktop_runtime))
        # The SDK's headless executable ships audio stubs. Keep its regular
        # engine hidden and use the offscreen Qt platform: native Pulse audio
        # remains available without a second visible phone or X11 dependency.
        environment["QT_QPA_PLATFORM"] = "offscreen"
        if self._audio_id:
            environment["PULSE_PROP_application.id"] = self._audio_id
            environment["PULSE_PROP_application.name"] = "Velox"
        user = private_directory(self.paths.root / "android-user")
        environment.update(
            {
                "ANDROID_SDK_ROOT": str(self.paths.sdk),
                "ANDROID_HOME": str(self.paths.sdk),
                "ANDROID_USER_HOME": str(user),
                "ANDROID_EMULATOR_HOME": str(self.home),
                "ANDROID_AVD_HOME": str(self.home / "avd"),
                "ADB_SERVER_SOCKET": "localfilesystem:" + str(self.hub.adb_socket),
                "ANDROID_ADB_SERVER_PORT": str(
                    self.hub.registration.port if self.hub.registration else 5038
                ),
                "ANDROID_EMULATOR_WAIT_TIME_BEFORE_KILL": "20",
                "HOME": str(user),
                "ADB_VENDOR_KEYS": str(user / "adbkey"),
                "XDG_RUNTIME_DIR": str(self.hub.runtime),
                "XDG_CACHE_HOME": str(self.paths.cache / "android"),
                "XDG_CONFIG_HOME": str(user / "config"),
            }
        )
        return environment

    def _prepare_adb_key(self):
        user = private_directory(self.paths.root / "android-user")
        executable = self.hub.setup.verify_executable("platform-tools/adb")
        if not (user / "adbkey").exists():
            bounded_process(
                [str(executable), "keygen", str(user / "adbkey")], environment=self.environment()
            )
        os.chmod(user / "adbkey", 0o600)
        public = bounded_read(user / "adbkey.pub", 16384)
        atomic_bytes(self.home / "adbkey.pub", public)
        atomic_bytes(user / ".android/adbkey.pub", public)
        bounded_process(
            [str(executable), "start-server"], environment=self.environment(), timeout=10
        )
        if self.hub.registration is None:
            from .adb import Registration

            self.hub.registration = Registration(self.hub)

    def _owned(self, record: dict) -> bool:
        if not isinstance(record, dict):
            return False
        pid, birth = record.get("pid"), record.get("birth")
        if type(pid) is not int or type(birth) is not int or pid <= 1:
            return False
        identity = proc_identity(pid)
        expected = str(self.paths.sdk / "emulator/qemu/linux-x86_64/qemu-system-x86_64")
        if (
            identity is None
            or identity[0] != os.getuid()
            or identity[1].removesuffix(" (deleted)") not in (expected, expected + "-headless")
            or identity[2] != birth
        ):
            return False
        try:
            arguments = bounded_read(Path(f"/proc/{pid}/cmdline"), 1024 * 1024).split(b"\x00")
        except (OSError, VeloxError):
            return False
        try:
            port = arguments[arguments.index(b"-port") + 1]
            avd = arguments[arguments.index(b"-avd") + 1]
            if port != str(self.metadata["port"]).encode() or avd != b"Gaming_Phone":
                return False
            environment = bounded_read(Path(f"/proc/{pid}/environ"), 1024 * 1024).split(b"\x00")
            return b"ANDROID_AVD_HOME=" + str(self.home / "avd").encode() in environment
        except (OSError, ValueError, IndexError, VeloxError):
            return False

    def alive(self) -> bool:
        try:
            return self._owned(read_json(self.folder / "runtime.json", {}))
        except (VeloxError, OSError):
            return False

    def observe_exit(self) -> bool:
        # A crashed engine must not leave cold maintenance permanently disabled.
        # The UI never waits for a start/stop worker's state lock.
        if self.state != "running" or self.alive() or not self._state_lock.acquire(blocking=False):
            return False
        try:
            if self.state == "running" and not self.alive():
                self.state, self.running_settings = "stopped", None
                return True
            return False
        finally:
            self._state_lock.release()

    def runtime(self) -> dict:
        value = read_json(self.folder / "runtime.json", {})
        if not self._owned(value):
            raise VeloxError("The owned phone process is no longer running.")
        return value

    def adb(
        self, *arguments: str, timeout=15, maximum=4 * 1024 * 1024, data=None, cancel=None
    ) -> bytes:
        # An old task must not command another emulator after this engine exits.
        self.runtime()
        executable = self.hub.setup.verify_executable("platform-tools/adb")
        return bounded_process(
            [str(executable), "-s", self.serial, *map(str, arguments)],
            environment=self.environment(),
            timeout=timeout,
            maximum=maximum,
            data=data,
            cancel=cancel,
        )

    def shell(self, command: str, timeout=15, maximum=4 * 1024 * 1024, cancel=None) -> str:
        return (
            self.adb("shell", command, timeout=timeout, maximum=maximum, cancel=cancel)
            .decode("utf-8", errors="replace")
            .strip()
        )

    def _configure(self, settings: Settings, cpu: int, ram: int, *, home=None) -> None:
        home = Path(home) if home is not None else self.home
        avd = home / "avd/Gaming_Phone.avd"
        private_directory(avd)
        atomic_json(
            home / ".velox-owner.json",
            {
                "application": "velox",
                "phone": self.identity,
                "root": str(self.paths.root.resolve()),
            },
        )
        existing = {}
        config = avd / "config.ini"
        if config.exists():
            for line in bounded_read(config, 1024 * 1024).decode("utf-8").splitlines():
                if "=" in line:
                    key, value = line.split("=", 1)
                    existing[key.strip()] = value.strip()
        display = DISPLAYS[settings.display]
        values = {
            "AvdId": "Gaming_Phone",
            "avd.ini.displayname": "Velox",
            "avd.ini.encoding": "UTF-8",
            "PlayStore.enabled": "true",
            "abi.type": "x86_64",
            "image.sysdir.1": "system-images/android-35/google_apis_playstore/x86_64/",
            "tag.display": "Google Play",
            "tag.id": "google_apis_playstore",
            "hw.cpu.arch": "x86_64",
            "hw.cpu.ncore": str(min(cpu, 6)),
            "hw.ramSize": str(memory_mib(ram)),
            "vm.heapSize": "576",
            "hw.gpu.enabled": "yes",
            "hw.gpu.mode": "swiftshader"
            if settings.gpu == "software"
            else "auto"
            if settings.gpu == "auto"
            else "host",
            "disk.dataPartition.size": "32G",
            "hw.sdCard": "yes",
            "sdcard.size": "2048M",
            "hw.keyboard": "yes",
            "hw.mainKeys": "no",
            "hw.lcd.width": str(display[1]),
            "hw.lcd.height": str(display[2]),
            "hw.lcd.density": str(display[3]),
            "hw.initialOrientation": "portrait",
            "hw.accelerometer": "yes",
            "hw.gyroscope": "yes",
            "hw.sensors.orientation": "yes",
            "hw.audioInput": "yes",
            "hw.audioOutput": "yes",
            "hw.battery": "yes",
            "hw.gps": "yes",
            "hw.camera.back": "emulated",
            "hw.camera.front": "emulated",
            "hw.lcd.vsync": str(settings.rate),
            "showDeviceFrame": "no",
            "fastboot.forceColdBoot": "no" if settings.quick_boot else "yes",
            "fastboot.forceFastBoot": "yes" if settings.quick_boot else "no",
            "runtime.network.latency": "none",
            "runtime.network.speed": "full",
        }
        existing.update(values)
        atomic_bytes(
            config, "".join(f"{key}={value}\n" for key, value in existing.items()).encode()
        )
        atomic_bytes(
            home / "avd/Gaming_Phone.ini",
            f"avd.ini.encoding=UTF-8\npath={avd}\npath.rel=avd/Gaming_Phone.avd\ntarget=android-35\n".encode(),
        )

    def _discover_process(self, wrapper: subprocess.Popen) -> dict | None:
        expected = str(self.paths.sdk / "emulator/qemu/linux-x86_64/qemu-system-x86_64")
        candidates = [wrapper.pid]
        try:
            children = Path(f"/proc/{wrapper.pid}/task/{wrapper.pid}/children").read_text().split()
            candidates.extend(map(int, children))
        except OSError:
            pass
        for pid in candidates:
            identity = proc_identity(pid)
            if (
                identity
                and identity[0] == os.getuid()
                and identity[1] in (expected, expected + "-headless")
            ):
                record = {"pid": pid, "birth": identity[2]}
                if self._owned(record):
                    return record
        return None

    def _token(self, pid: int) -> str:
        # Discovery lives inside this phone's private emulator home, never another
        # application's Android directory. Confirm the discovery PID and port.
        candidates = (
            self.home / "avd/running" / f"pid_{pid}.ini",
            self.home / "running" / f"pid_{pid}.ini",
            self.paths.root / "android-user/avd/running" / f"pid_{pid}.ini",
            self.hub.runtime / "avd/running" / f"pid_{pid}.ini",
        )
        for file in candidates:
            try:
                fields = dict(
                    line.split("=", 1)
                    for line in bounded_read(file, 65536).decode().splitlines()
                    if "=" in line
                )
                fields = {key.strip(): value.strip() for key, value in fields.items()}
                token = fields.get("grpc.token", "")
                if (
                    fields.get("grpc.port") == str(self.metadata["grpc"])
                    and 16 <= len(token) <= 4096
                ):
                    return token
            except (OSError, VeloxError, UnicodeError):
                pass
        return ""

    def _snapshot_fingerprint(self) -> str:
        records = []
        paths = [
            self.avd / name
            for name in (
                "userdata-qemu.img.qcow2",
                "sdcard.img.qcow2",
                "snapshots/default_boot/snapshot.pb",
                "snapshots/default_boot/ram.img",
            )
        ]
        if not paths[2].is_file():
            return ""
        for path in paths:
            try:
                info = path.lstat()
                if not path.is_file() or path.is_symlink():
                    return ""
                records.append(
                    (
                        str(path.relative_to(self.avd)),
                        info.st_ino,
                        info.st_size,
                        info.st_mtime_ns,
                        info.st_ctime_ns,
                    )
                )
            except FileNotFoundError:
                records.append((str(path.relative_to(self.avd)), None))
        return hashlib.sha256(json.dumps(records).encode()).hexdigest()

    def start(
        self, settings: Settings | None = None, report=lambda _message, _fraction: None
    ) -> None:
        settings = settings or self.paths.settings(self.identity)
        Settings.from_dict(settings.as_dict())
        with self._state_lock:
            if self.alive():
                self.state = "running"
                return
            if self.state == "starting":
                raise VeloxError("This phone is already starting.")
            self.state = "starting"
            self.cancel.clear()
        wrapper = None
        try:
            if not self.hub.setup.installed():
                raise VeloxError("Finish Android setup first.")
            if not kvm_available():
                raise VeloxError(
                    "Your account needs KVM access before Android can start. Open Setup for the detected instructions."
                )
            with (
                lease(self.paths.locks / (self.identity + ".maintenance")),
                lease(self.paths.locks / "sdk.lock", shared=True),
                lease(self.paths.locks / "allocation.lock", blocking=True),
            ):
                from .recovery import Recovery

                if self.metadata.get("recovery"):
                    Recovery(self).before_start()
                cpu, ram = self.hub.reserve(self.identity, settings)
                private_directory(self.avd)
                with AvdLock(self.avd):
                    self._configure(settings, cpu, ram)
                self._prepare_adb_key()
                executable = self.hub.setup.verify_executable("emulator/emulator")
                self.hub.setup.verify_executable("emulator/qemu/linux-x86_64/qemu-system-x86_64")
                if not self.hub._ports_free(self.metadata["port"]):
                    raise VeloxError(
                        "This phone's ports are occupied. Close its other controls or choose another instance."
                    )
                arguments = [
                    str(executable),
                    "-avd",
                    "Gaming_Phone",
                    "-port",
                    str(self.metadata["port"]),
                    "-qt-hide-window",
                    "-grpc",
                    str(self.metadata["grpc"]),
                    "-grpc-use-token",
                    "-no-boot-anim",
                    "-no-metrics",
                    "-crash-report-mode",
                    "disabled",
                    "-accel",
                    "on",
                    "-gpu",
                    "swiftshader" if settings.gpu == "software" else settings.gpu,
                    "-vsync-rate",
                    str(settings.rate),
                    "-cores",
                    str(min(cpu, 6)),
                    "-memory",
                    str(memory_mib(ram)),
                ]
                snapshot = read_json(self.folder / "snapshot.json", {})
                signature = settings.signature(cpu, ram)
                fingerprint = self._snapshot_fingerprint()
                resume = (
                    settings.quick_boot
                    and snapshot.get("signature") == signature
                    and snapshot.get("clean") is True
                    and fingerprint
                    and snapshot.get("fingerprint") == fingerprint
                )
                atomic_json(self.folder / "snapshot.json", {"clean": False})
                if not settings.quick_boot:
                    arguments.append("-no-snapshot")
                elif not resume:
                    arguments.append("-no-snapshot-load")
                if settings.number:
                    arguments.extend(["-phone-number", settings.number])
                if settings.root:
                    from .rooting import Root

                    root_image = Root(self, self.cancel).verify()
                    arguments.extend(["-ramdisk", str(root_image)])
                if cpu > 6:
                    arguments.extend(["-qemu", "-smp", f"cores={cpu}"])
                report("Starting Android", 0.03)
                self._audio_id = "app.velox.phone." + secrets.token_hex(16)
                log = self.folder / "emulator.log"
                descriptor = open_log(log)
                with os.fdopen(descriptor, "wb") as output:
                    wrapper = subprocess.Popen(
                        arguments,
                        stdin=subprocess.DEVNULL,
                        stdout=output,
                        stderr=subprocess.STDOUT,
                        env=self.environment(),
                        start_new_session=True,
                        close_fds=True,
                    )
                started = time.monotonic()
                runtime = None
                while time.monotonic() - started < 20:
                    runtime = self._discover_process(wrapper)
                    if runtime:
                        break
                    if wrapper.poll() is not None:
                        raise VeloxError(
                            "Android exited during startup. Open Startup diagnostics for details."
                        )
                    time.sleep(0.1)
                if not runtime:
                    raise VeloxError("The owned Android process could not be identified.")
                runtime.update(
                    {
                        "settings": settings.as_dict(),
                        "cores": cpu,
                        "memory": max(3, ram),
                        "started": now(),
                        "memory_mib": memory_mib(ram),
                        "signature": signature,
                        "state": "starting",
                        "audio_id": self._audio_id,
                    }
                )
                atomic_json(self.folder / "runtime.json", runtime)
            started = time.monotonic()
            while True:
                if self.cancel.is_set():
                    raise Cancelled("Startup cancelled. Your apps and saves are kept.")
                if not self.alive():
                    raise VeloxError(
                        "Android stopped before startup finished. Open Startup diagnostics."
                    )
                elapsed = time.monotonic() - started
                try:
                    ready = self.shell("getprop sys.boot_completed", timeout=4) == "1"
                except VeloxError:
                    ready = False
                token = self._token(runtime["pid"])
                if token:
                    runtime["token"] = token
                    atomic_json(self.folder / "runtime.json", runtime)
                report("Loading apps and services", min(0.88, 0.08 + elapsed / 120))
                if ready and token:
                    break
                if elapsed > 240:
                    raise VeloxError(
                        "The phone took too long to start. Use Startup repair; apps and saves are retained."
                    )
                self.cancel.wait(0.8)
            report("Applying your settings", 0.92)
            self.running_settings = dataclasses.replace(settings)
            self.apply_live(settings)
            from .experience import Experience

            experience = Experience(self)
            experience.ensure()
            experience.frame_policy()
            experience.sync(settings)
            from .wallpaper import Wallpaper

            try:
                if self.metadata.get("personalize") is True:
                    Wallpaper(self).prepare_default(settings)
                    experience.open_home(default=True)
                    with lease(self.paths.locks / "catalog.lock", blocking=True):
                        metadata = self.metadata
                        metadata.pop("personalize", None)
                        atomic_json(self.folder / "phone.json", metadata)
                Wallpaper(self).apply_pending()
            except (OSError, VeloxError):
                # A damaged cosmetic choice must not take working Android down.
                # Retain the queue so the owner can choose or retry it later.
                runtime["wallpaper_pending"] = True
            try:
                count = cpu_count(self.shell("cat /sys/devices/system/cpu/present", timeout=4))
                if 0 < count < cpu:
                    runtime["cores"] = count
                    self.hub.hardware = dataclasses.replace(
                        self.hub.hardware, guest_limit=min(self.hub.hardware.guest_limit, count)
                    )
                    record_cpu_limit(self.paths, cpu, count)
            except (OSError, VeloxError):
                pass  # A missing optional capability probe must not stop working Android.
            runtime["state"] = "running"
            try:
                output = bounded_read(self.folder / "emulator.log", 32 * 1024 * 1024).decode(
                    errors="replace"
                )
                from .graphics import engine_details

                runtime["graphics_details"] = engine_details(output)
                runtime["graphics"] = runtime["graphics_details"]["mode"]
            except (OSError, VeloxError):
                runtime["graphics"] = "unknown"
            if runtime["graphics"] == "software" and settings.gpu == "auto" and settings.rate > 30:
                with contextlib.suppress(OSError, VeloxError):
                    self.refresh_rate(30)
            atomic_json(self.folder / "runtime.json", runtime)
            self.state = "running"
            report("Ready", 1.0)
        except Exception:
            self.cancel.set()
            if self.alive():
                with contextlib.suppress(VeloxError):
                    self.stop()
            elif wrapper is not None and wrapper.poll() is None:
                # Only the exact child handle started here, before ownership was published.
                wrapper.terminate()
                with contextlib.suppress(subprocess.TimeoutExpired):
                    wrapper.wait(timeout=10)
            self.state = "stopped" if not self.alive() else "running"
            raise

    def stop(self) -> None:
        with lease(self.paths.locks / (self.identity + ".maintenance")):
            try:
                self._stop_owned()
            finally:
                if self.state == "stopping":
                    self.state = "running" if self.alive() else "stopped"

    def _stop_owned(self) -> None:
        self.cancel.set()
        if not self.alive():
            self.state = "stopped"
            return
        record = self.runtime()
        settings = Settings.from_dict(record["settings"])
        self.state = "stopping"
        with self._policy_lock:
            if record.get("state") == "running":
                with contextlib.suppress(OSError, VeloxError):
                    from .experience import Experience

                    Experience(self).restore_installed_policy()
        if self._input:
            self._input.close()
        with contextlib.suppress(VeloxError):
            if settings.quick_boot:
                self.shell("sync", timeout=5)
                self.adb("emu", "kill", timeout=8)
            else:
                # Framework shutdown broadcasts let Android services flush
                # delayed preferences; direct kernel poweroff skips them.
                self.shell("sync; svc power shutdown", timeout=12)
        end = time.monotonic() + 25
        while self._owned(record) and time.monotonic() < end:
            time.sleep(0.15)
        if self._owned(record):
            try:
                descriptor = os.pidfd_open(record["pid"])
            except ProcessLookupError:
                descriptor = None
            except OSError as error:
                raise VeloxError(
                    "This kernel could not open a safe process handle. The running phone was left intact."
                ) from error
            if descriptor is not None:
                try:
                    if self._owned(record):
                        signal.pidfd_send_signal(descriptor, signal.SIGTERM)
                finally:
                    os.close(descriptor)
            end = time.monotonic() + 8
            while self._owned(record) and time.monotonic() < end:
                time.sleep(0.1)
        if self._owned(record):
            raise VeloxError(
                "The phone is still closing. Its process was left intact; try again shortly."
            )
        # The SDK may retain its AVD descriptor briefly after the main process
        # exits. Do not offer a restart or fingerprint unfinished disk writes.
        with AvdLock(self.avd, timeout=25):
            self._finish_stop(record, settings)

    def _finish_stop(self, record, settings):
        trusted = False
        if settings.quick_boot:
            try:
                log = bounded_read(self.folder / "emulator.log", 32 * 1024 * 1024).decode(
                    errors="replace"
                )
                trusted = (
                    "Saving snapshot 'default_boot'" in log
                    and "UNSUPPORTED_VK_APP" not in log
                    and "Not saving state" not in log
                )
            except (OSError, VeloxError):
                pass
        fingerprint = self._snapshot_fingerprint() if trusted else ""
        atomic_json(
            self.folder / "snapshot.json",
            {
                "signature": record.get("signature"),
                "clean": bool(trusted and fingerprint),
                "fingerprint": fingerprint,
            },
        )
        (self.folder / "runtime.json").unlink(missing_ok=True)
        self.state = "stopped"

    def apply_live(self, settings: Settings) -> None:
        from .graphics import effective_rate

        display_rate = effective_rate(self.runtime(), settings.rate)
        scale = ".5" if settings.snappy else "1"
        commands = [
            f"settings put global {name} {scale}"
            for name in (
                "window_animation_scale",
                "transition_animation_scale",
                "animator_duration_scale",
            )
        ]
        commands.extend(
            [
                f"settings put system peak_refresh_rate {display_rate}",
                f"settings put system min_refresh_rate {display_rate}",
                f"settings put system show_touches {int(settings.show_touches)}",
                f"settings put system accelerometer_rotation {int(settings.auto_rotate)}",
                "settings put system screen_off_timeout 2147483647",
            ]
        )
        self.shell("; ".join(commands), timeout=10)
        self.running_settings = dataclasses.replace(settings)
        self._refresh_generation += 1
        from .navigation import apply

        apply(self, settings)

    def refresh_rate(self, rate):
        if type(rate) is not int or rate not in (15, 30, 60, 90, 120, 144):
            raise VeloxError("Unsupported refresh rate.")
        self.shell(
            f"settings put system peak_refresh_rate {rate}; settings put system min_refresh_rate {rate}",
            timeout=4,
        )

    def repair_startup(self) -> None:
        if self.alive() or self.state in ("starting", "stopping"):
            raise VeloxError("Stop this phone before repairing startup.")
        with lease(self.paths.locks / (self.identity + ".maintenance")):
            atomic_json(self.folder / "snapshot.json", {"clean": False})
            self.hub.setup.verify_executable("emulator/emulator")
            config = self.avd / "config.ini"
            if config.exists():
                text = bounded_read(config, 1024 * 1024).decode()
                text = re.sub(
                    r"(?m)^fastboot\.forceColdBoot=.*$", "fastboot.forceColdBoot=yes", text
                )
                atomic_bytes(config, text.encode())

    def key(self, code: int) -> None:
        if type(code) is not int or not 0 <= code <= 288:
            raise VeloxError("Unsupported Android key.")
        self.shell("input keyevent " + str(code), timeout=5)

    def screenshot(self) -> Path:
        private_directory(self.folder / "captures")
        target = self.folder / "captures" / time.strftime("Velox %Y-%m-%d %H.%M.%S.png")
        picture = self.adb("exec-out", "screencap", "-p", timeout=15, maximum=32 * 1024 * 1024)
        if not picture.startswith(b"\x89PNG\r\n\x1a\n"):
            raise VeloxError("Android returned an invalid screenshot.")
        atomic_bytes(target, picture)
        return target

    def launch(self, component: str) -> None:
        if (
            not isinstance(component, str)
            or len(component) > 400
            or not COMPONENT.fullmatch(component)
        ):
            raise VeloxError("Invalid Android app component.")
        self.shell(
            "am start -W --user current -a android.intent.action.MAIN -c android.intent.category.LAUNCHER -f 270532608 -n "
            + shlex.quote(component),
            timeout=20,
        )

    def diagnostics(self) -> str:
        try:
            text = bounded_read(self.folder / "emulator.log", 32 * 1024 * 1024).decode(
                errors="replace"
            )[-24000:]
        except (OSError, VeloxError):
            text = "No startup log is available yet."
        for prefix in (str(Path.home()), str(self.paths.root), str(self.paths.sdk)):
            text = text.replace(prefix, "<private folder>")
        text = re.sub(
            r"(?im)(token|authorization|phone.number|adbkey)[^\n]*", r"\1=<redacted>", text
        )
        return text
