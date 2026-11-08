from __future__ import annotations

import dataclasses
import math
import time
from pathlib import Path

from PySide6.QtCore import QObject, QTimer, Qt

from .core import (
    CPU_CHOICES,
    DISPLAYS,
    RAM_CHOICES,
    RATES,
    Settings,
    VeloxError,
    atomic_bytes,
    atomic_json,
    bounded_read,
    lease,
    package,
    read_json,
)


RESERVED = {18, 27, 91, 92, 119, 121, 164, 165}


@dataclasses.dataclass
class Binding:
    kind: int = 0
    key: int = 32
    x: float = 0.5
    y: float = 0.5
    x2: float = 0.7
    y2: float = 0.5
    radius: float = 0.12

    @classmethod
    def parse(cls, value):
        if not isinstance(value, dict):
            raise VeloxError("Invalid control binding.")
        try:
            result = cls(**value)
        except TypeError as error:
            raise VeloxError("Unsupported control fields.") from error
        if (
            type(result.kind) is not int
            or result.kind not in range(6)
            or type(result.key) is not int
            or not 1 <= result.key < 272
            or result.key in RESERVED
        ):
            raise VeloxError("Unsupported or reserved game control.")
        if any(
            type(getattr(result, key)) not in (float, int)
            or not math.isfinite(getattr(result, key))
            or not 0 <= getattr(result, key) <= 1
            for key in ("x", "y", "x2", "y2")
        ):
            raise VeloxError("Control coordinates must be inside the phone screen.")
        if (
            type(result.radius) not in (float, int)
            or not math.isfinite(result.radius)
            or not 0.02 <= result.radius <= 0.3
        ):
            raise VeloxError("Control radius must be between 2% and 30%.")
        return result


@dataclasses.dataclass
class Profile:
    enabled: bool = False
    bindings: list[Binding] = dataclasses.field(default_factory=list)
    pad_mode: int = 0
    pad_layout: int = 0
    pad_device: str = ""
    deadzone: float = 0.18
    sensitivity: float = 1.0
    invert_y: bool = False

    @classmethod
    def parse(cls, value):
        if not isinstance(value, dict):
            raise VeloxError("Invalid control profile.")
        try:
            result = cls(**{key: item for key, item in value.items() if key != "bindings"})
        except TypeError as error:
            raise VeloxError("Unsupported controller profile fields.") from error
        bindings = value.get("bindings", [])
        if not isinstance(bindings, list) or len(bindings) > 14:
            raise VeloxError("A profile supports up to 14 bindings.")
        result.bindings = [Binding.parse(binding) for binding in bindings]
        if len({binding.key for binding in result.bindings}) != len(result.bindings):
            raise VeloxError("A key can have only one binding in each orientation.")
        if (
            type(result.enabled) is not bool
            or type(result.invert_y) is not bool
            or type(result.pad_mode) is not int
            or result.pad_mode not in range(3)
            or type(result.pad_layout) is not int
            or result.pad_layout not in range(4)
            or not isinstance(result.pad_device, str)
            or len(result.pad_device) > 100
        ):
            raise VeloxError("Unsupported controller preferences.")
        if any(
            type(getattr(result, key)) not in (float, int)
            or not math.isfinite(getattr(result, key))
            or not low <= getattr(result, key) <= high
            for key, low, high in (("deadzone", 0.05, 0.4), ("sensitivity", 0.5, 2))
        ):
            raise VeloxError("Invalid controller sensitivity or deadzone.")
        return result


@dataclasses.dataclass
class Preset:
    enabled: bool = False
    display: int = -1
    rate: int = 0
    cores: int = -1
    memory: int = -1
    gpu: int = -1
    fullscreen: int = -1

    @classmethod
    def parse(cls, value):
        if not isinstance(value, dict):
            raise VeloxError("Invalid game preset.")
        try:
            result = cls(**value)
        except TypeError as error:
            raise VeloxError("Unsupported game preset fields.") from error
        if type(result.enabled) is not bool or any(
            type(getattr(result, key)) is not int
            for key in ("display", "rate", "cores", "memory", "gpu", "fullscreen")
        ):
            raise VeloxError("Invalid preset values.")
        if (
            result.display not in range(-1, len(DISPLAYS))
            or result.rate not in (0, *RATES)
            or result.cores not in (-1, *CPU_CHOICES)
            or result.memory not in (-1, *RAM_CHOICES)
            or result.gpu not in (-1, 0, 1, 2)
            or result.fullscreen not in (-1, 0, 1)
        ):
            raise VeloxError("The game preset contains unsupported values.")
        return result

    def apply(self, global_settings: Settings) -> Settings:
        result = dataclasses.replace(global_settings)
        if self.enabled:
            for key in ("display", "cores", "memory"):
                if getattr(self, key) >= 0:
                    setattr(result, key, getattr(self, key))
            if self.rate:
                result.rate = self.rate
            if self.gpu >= 0:
                result.gpu = ("auto", "host", "software")[self.gpu]
        return result


class Controls:
    def __init__(self, phone):
        self.phone = phone

    def _path(self, identity):
        return self.phone.folder / "controls" / (package(identity) + ".json")

    def _read(self, identity):
        value = read_json(self._path(identity), None, 65536)
        if value is None:
            return Preset(), [Profile(), Profile()]
        if (
            not isinstance(value, dict)
            or value.get("format") != 1
            or value.get("package") != identity
            or not isinstance(value.get("profiles"), list)
            or len(value["profiles"]) != 2
        ):
            raise VeloxError("This game's saved controls are damaged or unsupported.")
        return Preset.parse(value.get("preset")), [
            Profile.parse(profile) for profile in value["profiles"]
        ]

    def _write(self, identity, preset, profiles):
        atomic_json(
            self._path(identity),
            {
                "format": 1,
                "package": identity,
                "preset": dataclasses.asdict(preset),
                "profiles": [dataclasses.asdict(profile) for profile in profiles],
            },
        )

    def _lock(self, identity):
        return lease(self.phone.paths.locks / ("controls-" + package(identity) + ".lock"))

    def load(self, identity: str, landscape=False) -> Profile:
        return self._read(identity)[1][int(landscape)]

    def save(self, identity: str, landscape: bool, profile: Profile):
        package(identity)
        Profile.parse(dataclasses.asdict(profile))
        with self._lock(identity):
            preset, profiles = self._read(identity)
            profiles[int(landscape)] = profile
            self._write(identity, preset, profiles)

    def preset(self, identity: str) -> Preset:
        return self._read(identity)[0]

    def save_preset(self, identity: str, preset: Preset):
        package(identity)
        Preset.parse(dataclasses.asdict(preset))
        with self._lock(identity):
            _previous, profiles = self._read(identity)
            self._write(identity, preset, profiles)

    def exported(self, identity: str) -> bytes:
        value, profiles = self._read(identity)
        return self._portable(identity, value, profiles)

    @staticmethod
    def _portable(identity, value, profiles):
        lines = [
            "velox-controls 1",
            "package " + package(identity),
            f"preset {int(value.enabled)} {value.display} {value.rate} {value.cores} {value.memory} {value.gpu} {value.fullscreen}",
        ]
        for landscape, profile in zip((False, True), profiles):
            lines.append(
                f"profile {int(landscape)} {int(profile.enabled)} {profile.pad_mode} {profile.pad_layout} {profile.deadzone:.8g} {profile.sensitivity:.8g} {int(profile.invert_y)} {len(profile.bindings)}"
            )
            for binding in profile.bindings:
                lines.append(
                    f"binding {binding.kind} {binding.key} {binding.x:.8g} {binding.y:.8g} {binding.x2:.8g} {binding.y2:.8g} {binding.radius:.8g}"
                )
        return ("\n".join([*lines, "end"]) + "\n").encode()

    @staticmethod
    def inspect(file: Path):
        try:
            words = iter(bounded_read(file, 65536).decode("utf-8").split())

            def expect(value):
                if next(words) != value:
                    raise ValueError("Unexpected control format")

            def number():
                return int(next(words))

            def flag():
                value = number()
                if value not in (0, 1):
                    raise ValueError("Invalid flag")
                return bool(value)

            expect("velox-controls")
            expect("1")
            expect("package")
            identity = package(next(words))
            expect("preset")
            preset = Preset.parse(
                dict(
                    zip(
                        ("enabled", "display", "rate", "cores", "memory", "gpu", "fullscreen"),
                        [flag(), *[number() for _ in range(6)]],
                    )
                )
            )
            profiles = []
            for orientation in (0, 1):
                expect("profile")
                if number() != orientation:
                    raise ValueError("Invalid orientation")
                enabled, mode, layout = flag(), number(), number()
                deadzone, sensitivity, invert, count = (
                    float(next(words)),
                    float(next(words)),
                    flag(),
                    number(),
                )
                if not 0 <= count <= 14:
                    raise ValueError("Too many controls")
                bindings = []
                for _ in range(count):
                    expect("binding")
                    bindings.append(
                        dict(
                            zip(
                                ("kind", "key", "x", "y", "x2", "y2", "radius"),
                                [number(), number(), *[float(next(words)) for _ in range(5)]],
                            )
                        )
                    )
                profiles.append(
                    Profile.parse(
                        {
                            "enabled": enabled,
                            "pad_mode": mode,
                            "pad_layout": layout,
                            "deadzone": deadzone,
                            "sensitivity": sensitivity,
                            "invert_y": invert,
                            "bindings": bindings,
                        }
                    )
                )
            expect("end")
            if next(words, None) is not None:
                raise ValueError("Trailing control data")
            return identity, preset, profiles
        except (StopIteration, ValueError, TypeError, UnicodeError) as error:
            raise VeloxError("The controls file is damaged or unsupported.") from error

    def import_layout(self, file: Path, expected_package: str):
        identity, preset, profiles = self.inspect(file)
        if identity != package(expected_package):
            raise VeloxError("This controls file belongs to another game.")
        with self._lock(identity):
            previous_preset, previous_profiles = self._read(identity)
            atomic_bytes(
                self.phone.folder / "controls-undo.veloxcontrols",
                self._portable(identity, previous_preset, previous_profiles),
            )
            # Both orientations and the preset commit together. An interrupted
            # import leaves either the previous bundle or the complete new one.
            self._write(identity, preset, profiles)

    def undo(self):
        file = self.phone.folder / "controls-undo.veloxcontrols"
        identity, preset, profiles = self.inspect(file)
        with self._lock(identity):
            self._write(identity, preset, profiles)
            file.unlink()


def virtual_key(key, scan=0, keypad=False):
    if keypad:
        if 48 <= key <= 57:
            return 96 + key - 48
        if key in (42, 43, 45, 46, 47):
            return {42: 106, 43: 107, 45: 109, 46: 110, 47: 111}[key]
    if key == 32 or 48 <= key <= 57 or 65 <= key <= 90:
        return key
    keys = {
        Qt.Key.Key_Backspace: 8,
        Qt.Key.Key_Tab: 9,
        Qt.Key.Key_Return: 13,
        Qt.Key.Key_Enter: 13,
        Qt.Key.Key_Shift: 16,
        Qt.Key.Key_Control: 17,
        Qt.Key.Key_Alt: 18,
        Qt.Key.Key_Escape: 27,
        Qt.Key.Key_Left: 37,
        Qt.Key.Key_Up: 38,
        Qt.Key.Key_Right: 39,
        Qt.Key.Key_Down: 40,
        Qt.Key.Key_Insert: 45,
        Qt.Key.Key_Delete: 46,
        Qt.Key.Key_Home: 36,
        Qt.Key.Key_End: 35,
        Qt.Key.Key_PageUp: 33,
        Qt.Key.Key_PageDown: 34,
    }
    keys.update(
        {
            59: 186,
            58: 186,
            61: 187,
            43: 187,
            44: 188,
            45: 189,
            46: 190,
            47: 191,
            96: 192,
            91: 219,
            92: 220,
            93: 221,
            39: 222,
            34: 222,
            0xD6: 186,
            0xDC: 219,
            0xC4: 222,
        }
    )
    if Qt.Key.Key_F1 <= key <= Qt.Key.Key_F24:
        return 112 + key - Qt.Key.Key_F1
    return keys.get(key, 0)


class Mapper(QObject):
    def __init__(self, parent, send):
        super().__init__(parent)
        self.send = send
        self.profile = Profile()
        self.held, self.sticks, self.swipes = set(), set(), {}
        self.width, self.height = 1, 1
        self.aiming = False
        self.aim_x = self.aim_y = 0.5
        self.timer = QTimer(self)
        self.timer.setInterval(16)
        self.timer.timeout.connect(self.animate)

    def configure(self, profile: Profile, width, height):
        self.release()
        self.profile = profile
        self.width, self.height = max(1, width), max(1, height)

    def key_event(self, key: int, down: bool) -> bool:
        if not self.profile.enabled and not (key >= 256 and self.profile.pad_mode == 2):
            return False
        if not any(
            (binding.kind == 1 and key in (65, 68, 83, 87))
            or binding.kind in (0, 2)
            and binding.key == key
            for binding in self.profile.bindings
        ):
            return False
        if (key in self.held) == down:
            return True
        self.held.add(key) if down else self.held.discard(key)
        for index, binding in enumerate(self.profile.bindings):
            if binding.kind == 1 and key in (65, 68, 83, 87):
                x, y = (
                    int(68 in self.held) - int(65 in self.held),
                    int(83 in self.held) - int(87 in self.held),
                )
                magnitude = math.hypot(x, y)
                if magnitude:
                    short = min(self.width, self.height)
                    self.send(
                        "M" if index in self.sticks else "D",
                        index,
                        binding.x + x / magnitude * binding.radius * short / self.width,
                        binding.y + y / magnitude * binding.radius * short / self.height,
                    )
                    self.sticks.add(index)
                elif index in self.sticks:
                    self.send("U", index, 0, 0)
                    self.sticks.discard(index)
            elif binding.key == key and binding.kind == 0:
                self.send("D" if down else "U", index, binding.x, binding.y)
            elif binding.key == key and binding.kind == 2 and down:
                self.send("D", index, binding.x, binding.y)
                self.swipes[index] = time.monotonic()
                self.timer.start()
        return True

    def animate(self):
        current = time.monotonic()
        for index, started in list(self.swipes.items()):
            binding = self.profile.bindings[index]
            progress = min(1.0, (current - started) / 0.25)
            self.send(
                "M",
                index,
                binding.x + (binding.x2 - binding.x) * progress,
                binding.y + (binding.y2 - binding.y) * progress,
            )
            if progress >= 1:
                self.send("U", index, 0, 0)
                del self.swipes[index]
        if not self.swipes:
            self.timer.stop()

    def pad_frame(self, frame):
        active = frame.connected and self.profile.pad_mode == 2
        for button in range(16):
            self.key_event(256 + button, active and bool(frame.buttons & (1 << button)))
        for index, binding in enumerate(self.profile.bindings):
            if binding.kind not in (4, 5):
                continue
            x, y = (frame.lx, frame.ly) if binding.kind == 4 else (frame.rx, frame.ry)
            if active and math.hypot(x, y) > 0.001:
                short = min(self.width, self.height)
                self.send(
                    "M" if index in self.sticks else "D",
                    index,
                    binding.x + x * binding.radius * short / self.width,
                    binding.y + y * binding.radius * short / self.height,
                )
                self.sticks.add(index)
            elif index in self.sticks:
                self.send("U", index, 0, 0)
                self.sticks.discard(index)

    def start_aim(self):
        binding = next((item for item in self.profile.bindings if item.kind == 3), None)
        if not self.profile.enabled or binding is None:
            return False
        self.aim_x, self.aim_y = binding.x, binding.y
        self.aiming = True
        self.send("D", 15, self.aim_x, self.aim_y)
        return True

    def aim_delta(self, dx, dy):
        if not self.aiming or not (dx or dy) or not math.isfinite(dx + dy):
            return
        self.aim_x += dx * 1.8
        self.aim_y += dy * 1.8
        if not (0.08 <= self.aim_x <= 0.92 and 0.08 <= self.aim_y <= 0.92):
            binding = next((item for item in self.profile.bindings if item.kind == 3), None)
            self.send("U", 15, 0, 0)
            self.aim_x, self.aim_y = (binding.x, binding.y) if binding else (0.5, 0.5)
            self.send("D", 15, self.aim_x, self.aim_y)
        else:
            self.send("M", 15, self.aim_x, self.aim_y)

    def end_aim(self):
        if self.aiming:
            self.send("U", 15, 0, 0)
            self.aiming = False

    def release(self):
        self.aiming = False
        self.timer.stop()
        self.held.clear()
        self.sticks.clear()
        self.swipes.clear()
        self.send("C", 0, 0, 0)
