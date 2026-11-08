from __future__ import annotations

import base64
import dataclasses
import hashlib
import re
import shlex
import threading
import time
from pathlib import Path

from .core import (
    COMPONENT,
    Cancelled,
    Settings,
    VeloxError,
    atomic_bytes,
    atomic_json,
    file_hash,
    package,
    read_json,
)
from .setup import resource_root


@dataclasses.dataclass
class App:
    component: str
    package: str
    label: str
    game: bool = False
    favorite: bool = False
    updated: int = 0
    played: int = 0


class Experience:
    _install_lock = threading.RLock()

    def __init__(self, phone):
        self.phone = phone
        self.state_path = phone.folder / "experience.json"

    def ensure_package(self, name: str, filename: str) -> str:
        package(name)
        resource = resource_root() / filename
        manifest = read_json(resource_root() / "resources.json", {})
        expected = manifest.get(filename)
        if (
            not isinstance(expected, str)
            or not re.fullmatch("[a-f0-9]{64}", expected)
            or file_hash(resource) != expected
        ):
            raise VeloxError("The Android companion resource did not pass its integrity check.")
        record = self.phone.runtime()
        cache = getattr(self.phone, "_companions", {})
        key = (record["pid"], record["birth"], expected)
        if cache.get(name, (None, None))[0] == key:
            return cache[name][1]

        def installed():
            try:
                text = self.phone.shell("pm path " + name, timeout=8)
            except VeloxError:
                return ""
            lines = [line[8:] for line in text.splitlines() if line.startswith("package:")]
            if (
                len(lines) != 1
                or not lines[0].startswith(("/data/app/", "/system/app/", "/product/app/"))
                or len(lines[0]) > 1024
            ):
                return ""
            path = lines[0]
            digest = self.phone.shell(
                "/system/bin/sha256sum " + shlex.quote(path), timeout=10
            ).split()
            return path if digest and digest[0] == expected else ""

        with self._install_lock:
            path = installed()
            if not path:
                result = self.phone.adb("install", "-r", str(resource), timeout=120).decode(
                    errors="replace"
                )
                if "Success" not in result:
                    raise VeloxError(
                        "The signed Velox companion could not be installed. Existing apps and data were retained."
                    )
                path = installed()
            if not path:
                raise VeloxError(
                    "The installed Velox companion does not match this build. Game input remains disabled."
                )
            cache[name] = (key, path)
            self.phone._companions = cache
            return path

    def ensure(self) -> str:
        path = self.ensure_package("app.velox.home", "companion.apk")
        deadline = time.monotonic() + 30
        while True:
            if self.phone.cancel.is_set():
                raise Cancelled("Startup cancelled. Your apps and saves are kept.")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise VeloxError("Velox Home isn't ready yet. Reconnect shortly.")
            try:
                result = self.phone.shell(
                    "content call --uri content://app.velox.home.bridge --method hello",
                    timeout=min(8, remaining),
                )
                if "ok=true" in result:
                    return path
            except VeloxError:
                if not self.phone.alive():
                    raise
            if time.monotonic() >= deadline:
                raise VeloxError("Velox Home isn't ready yet. Reconnect shortly.")
            self.phone.cancel.wait(0.5)

    def sync(self, settings: Settings, *, from_host=True) -> Settings:
        self.ensure_package("app.velox.home", "companion.apk")
        fields = (
            self.phone.adb(
                "exec-out",
                "content",
                "read",
                "--uri",
                "content://app.velox.home.bridge/state",
                timeout=8,
            )
            .decode()
            .strip()
            .split("\t")
        )
        if len(fields) != 4 or not all(re.fullmatch(r"[0-9]{1,19}", field) for field in fields):
            raise VeloxError("The Android preference response was invalid.")
        revision, navigation, rotate, language = map(int, fields)
        if (
            revision > 2**63 - 1
            or navigation not in (0, 1, 2)
            or rotate not in (0, 1)
            or language not in (0, 1)
        ):
            raise VeloxError("The Android preferences are outside the supported range.")
        state = read_json(self.state_path, {})
        previous = state.get("revision", 0)
        host_values = [
            settings.navigation,
            int(settings.auto_rotate),
            int(settings.language == "de"),
        ]
        if not from_host and revision == previous and state.get("settings") == host_values:
            return settings
        if not from_host and revision > previous:
            updated = dataclasses.replace(
                settings,
                navigation=navigation,
                auto_rotate=bool(rotate),
                language="de" if language else "en",
            )
            if not self.phone.paths.save_if_current(self.phone.identity, settings, updated):
                return self.phone.paths.settings(self.phone.identity)
            settings = updated
            live = self.phone.running_settings or settings
            self.phone.apply_live(
                dataclasses.replace(
                    live,
                    navigation=settings.navigation,
                    auto_rotate=settings.auto_rotate,
                    language=settings.language,
                )
            )
        argument = f"{settings.navigation},{int(settings.auto_rotate)},{revision},{int(settings.language == 'de')}"
        result = self.phone.shell(
            "content call --uri content://app.velox.home.bridge --method sync --arg "
            + shlex.quote(argument),
            timeout=8,
        )
        if "ok=true" not in result:
            raise VeloxError("The phone did not accept its Velox preferences.")
        atomic_json(
            self.state_path,
            {
                "revision": revision,
                "settings": [
                    settings.navigation,
                    int(settings.auto_rotate),
                    int(settings.language == "de"),
                ],
            },
        )
        return settings

    def library(self, refresh=False) -> list[App]:
        file = self.phone.folder / "library.json"
        if not refresh or not self.phone.alive():
            values = read_json(file, [])
            if isinstance(values, list) and len(values) <= 2000:
                try:
                    return [
                        App(**value)
                        for value in values
                        if isinstance(value, dict)
                        and COMPONENT.fullmatch(value.get("component", ""))
                    ]
                except TypeError:
                    pass
            return []
        self.ensure()
        text = self.phone.adb(
            "exec-out",
            "content",
            "read",
            "--uri",
            "content://app.velox.home.bridge/apps",
            maximum=4 * 1024 * 1024,
        ).decode()
        apps = []
        for line in text.splitlines()[:2000]:
            fields = line.split("\t")
            if len(fields) != 6 or not COMPONENT.fullmatch(fields[0]) or len(fields[0]) > 400:
                continue
            try:
                label = base64.b64decode(fields[1], validate=True).decode("utf-8")
                if len(label) > 1024 or not label:
                    label = fields[0].split("/", 1)[0]
                updated, played = int(fields[4]), int(fields[5])
                if not 0 <= updated <= 2**63 - 1 or not 0 <= played <= 2**63 - 1:
                    continue
                apps.append(
                    App(
                        fields[0],
                        fields[0].split("/", 1)[0],
                        label,
                        fields[2] == "1",
                        fields[3] == "1",
                        updated,
                        played,
                    )
                )
            except (ValueError, UnicodeError):
                continue
        atomic_json(file, [dataclasses.asdict(app) for app in apps])
        return apps

    def icon(self, app: App) -> Path:
        if not COMPONENT.fullmatch(app.component):
            raise VeloxError("Invalid app component.")
        name = (
            hashlib.sha256(app.component.encode()).hexdigest()[:24]
            + "-"
            + str(app.updated)
            + ".png"
        )
        path = self.phone.folder / "app-icons" / name
        if path.exists():
            return path
        self.ensure()
        uri = "content://app.velox.home.bridge/icon?component=" + app.component
        data = self.phone.adb(
            "exec-out", "content", "read", "--uri", uri, maximum=512 * 1024, timeout=8
        )
        if not data.startswith(b"\x89PNG\r\n\x1a\n"):
            raise VeloxError("The phone returned an invalid app icon.")
        atomic_bytes(path, data)
        return path

    def call(self, method: str, component: str, value: bool | None = None) -> None:
        if method not in ("favorite", "game", "played") or not COMPONENT.fullmatch(component):
            raise VeloxError("Invalid library change.")
        self.ensure()
        argument = component + (("|" + str(int(value))) if value is not None else "")
        result = self.phone.shell(
            "content call --uri content://app.velox.home.bridge --method "
            + method
            + " --arg "
            + shlex.quote(argument)
        )
        if "ok=true" not in result:
            raise VeloxError("The phone did not accept this library change.")

    def open_home(self, default=False) -> None:
        self.ensure()
        if default:
            self.phone.shell("cmd package set-home-activity app.velox.home/.HomeActivity")
        self.phone.launch("app.velox.home/.HomeActivity")

    def open_settings(self) -> None:
        self.ensure()
        self.phone.launch("app.velox.home/.SettingsActivity")

    def frame_policy(self, identity=None, rate=0):
        if type(rate) is not int or rate not in (0, 15, 30, 60, 90, 120, 144):
            raise VeloxError("Unsupported frame limit.")
        path = self.ensure_package("app.velox.home", "companion.apk")
        arguments = "apply " + package(identity) + " " + str(rate) if identity else "restore"
        return self._frame_command(path, arguments)

    def restore_installed_policy(self):
        """Stop never installs an app just to clean up an optional frame limit."""
        record = self.phone.runtime()
        cached = getattr(self.phone, "_companions", {}).get("app.velox.home")
        if cached is not None and cached[0][:2] == (record["pid"], record["birth"]):
            return self._frame_command(cached[1], "restore")

    def _frame_command(self, path, arguments):
        command = (
            "CLASSPATH="
            + shlex.quote(path)
            + " app_process /system/bin app.velox.home.FramePolicy "
            + arguments
        )
        result = self.phone.shell(command, timeout=8)
        if not result.startswith(
            (
                "VELOX_FRAME_APPLIED ",
                "VELOX_FRAME_RESTORED",
                "VELOX_FRAME_UNSUPPORTED",
                "VELOX_FRAME_EXTERNAL",
            )
        ):
            raise VeloxError("Android did not accept its frame pacing policy.")
        return result

    def input_command(self) -> list[str]:
        path = self.ensure_package("app.velox.home", "companion.apk")
        command = (
            "CLASSPATH=" + shlex.quote(path) + " app_process /system/bin app.velox.home.InputBridge"
        )
        return [
            str(self.phone.hub.setup.verify_executable("platform-tools/adb")),
            "-s",
            self.phone.serial,
            "shell",
            command,
        ]
