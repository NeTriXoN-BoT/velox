"""Native gamepad discovery and normalized reports; no desktop input injection."""

from __future__ import annotations

import ctypes as C
import copy
import contextlib
import dataclasses
import hashlib
import json
import math
import os
import struct
import subprocess
import threading
import time

from .core import VeloxError, file_hash, read_json
from .setup import resource_root


BUTTONS = (
    "A",
    "B",
    "X",
    "Y",
    "LB",
    "RB",
    "Back",
    "Start",
    "LS",
    "RS",
    "Up",
    "Down",
    "Left",
    "Right",
    "LT",
    "RT",
)
_lock = threading.RLock()


@dataclasses.dataclass(frozen=True)
class Frame:
    connected: bool = False
    buttons: int = 0
    lx: float = 0.0
    ly: float = 0.0
    rx: float = 0.0
    ry: float = 0.0
    lt: float = 0.0
    rt: float = 0.0


@dataclasses.dataclass(frozen=True)
class Device:
    identity: str
    name: str
    mapped: bool


def filtered(frame, deadzone=0.18, sensitivity=1.0, invert_y=False):
    if not frame.connected:
        return Frame()
    values = []
    for x, y in ((frame.lx, frame.ly), (frame.rx, frame.ry)):
        length = math.hypot(x, y)
        magnitude = min(1.0, max(0.0, (length - deadzone) / (1 - deadzone)) * sensitivity)
        values.extend(
            (x / length * magnitude, y / length * magnitude * (-1 if invert_y else 1))
            if length
            else (0.0, 0.0)
        )
    lt, rt = max(0.0, min(1.0, frame.lt)), max(0.0, min(1.0, frame.rt))
    buttons = frame.buttons & 0x3FFF
    if lt > 0.12:
        buttons |= 1 << 14
    if rt > 0.12:
        buttons |= 1 << 15
    return Frame(True, buttons, *values, lt, rt)


class Guid(C.Structure):
    _fields_ = [("data", C.c_uint8 * 16)]


class Reader:
    """SDL's gamepad APIs read only controllers, without global key/mouse hooks."""

    def __init__(self):
        self.library = None
        self.handles = {}
        self.devices = []
        self._indices = {}
        self._next_scan = 0.0
        self.error = ""
        root = resource_root()
        bundled = root / "native/libSDL2-2.0.so.0"
        try:
            if bundled.exists():
                expected = read_json(root / "resources.json", {}).get("native/libSDL2-2.0.so.0")
                if (
                    not isinstance(expected, str)
                    or len(expected) != 64
                    or file_hash(bundled) != expected
                ):
                    raise VeloxError("The controller library did not pass its integrity check.")
            self.library = C.CDLL(str(bundled) if bundled.exists() else "libSDL2-2.0.so.0")
            pointer, integer = C.c_void_p, C.c_int
            declarations = (
                ("SDL_InitSubSystem", integer, [C.c_uint32]),
                ("SDL_QuitSubSystem", None, [C.c_uint32]),
                ("SDL_SetHint", integer, [C.c_char_p, C.c_char_p]),
                ("SDL_GameControllerEventState", integer, [integer]),
                ("SDL_JoystickEventState", integer, [integer]),
                ("SDL_NumJoysticks", integer, []),
                ("SDL_IsGameController", integer, [integer]),
                ("SDL_JoystickGetDeviceInstanceID", integer, [integer]),
                ("SDL_JoystickGetDeviceGUID", Guid, [integer]),
                ("SDL_JoystickNameForIndex", C.c_char_p, [integer]),
                ("SDL_GameControllerOpen", pointer, [integer]),
                ("SDL_GameControllerGetJoystick", pointer, [pointer]),
                ("SDL_GameControllerGetAxis", C.c_int16, [pointer, integer]),
                ("SDL_GameControllerGetButton", C.c_uint8, [pointer, integer]),
                ("SDL_GameControllerClose", None, [pointer]),
                ("SDL_GameControllerUpdate", None, []),
                ("SDL_JoystickOpen", pointer, [integer]),
                ("SDL_JoystickClose", None, [pointer]),
                ("SDL_JoystickGetAttached", integer, [pointer]),
                ("SDL_JoystickGetAxis", C.c_int16, [pointer, integer]),
                ("SDL_JoystickGetButton", C.c_uint8, [pointer, integer]),
                ("SDL_JoystickGetHat", C.c_uint8, [pointer, integer]),
                ("SDL_JoystickNumAxes", integer, [pointer]),
                ("SDL_JoystickNumButtons", integer, [pointer]),
                ("SDL_JoystickNumHats", integer, [pointer]),
                ("SDL_JoystickGetSerial", C.c_char_p, [pointer]),
            )
            for name, result, arguments in declarations:
                function = getattr(self.library, name)
                function.restype, function.argtypes = result, arguments
            with _lock:
                self.library.SDL_SetHint(b"SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", b"1")
                if self.library.SDL_InitSubSystem(0x2200) != 0:
                    raise VeloxError("The native controller connection is unavailable.")
                self.library.SDL_GameControllerEventState(0)
                self.library.SDL_JoystickEventState(0)
        except (OSError, AttributeError, VeloxError) as error:
            self.error = str(error)
            self.library = None

    def scan(self, force=False):
        if not self.library:
            return []
        with _lock:
            if not force and time.monotonic() < self._next_scan:
                return list(self.devices)
            self._next_scan = time.monotonic() + 1
            self.library.SDL_GameControllerUpdate()
            found, identities = [], set()
            self._indices = {}
            ordinals = {}
            for index in range(min(32, max(0, self.library.SDL_NumJoysticks()))):
                instance = self.library.SDL_JoystickGetDeviceInstanceID(index)
                if instance < 0:
                    continue
                identities.add(instance)
                if instance not in self.handles:
                    mapped = bool(self.library.SDL_IsGameController(index))
                    handle = (
                        self.library.SDL_GameControllerOpen(index)
                        if mapped
                        else self.library.SDL_JoystickOpen(index)
                    )
                    if not handle:
                        continue
                    joystick = (
                        self.library.SDL_GameControllerGetJoystick(handle) if mapped else handle
                    )
                    guid = bytes(self.library.SDL_JoystickGetDeviceGUID(index).data).hex()
                    name = (self.library.SDL_JoystickNameForIndex(index) or b"Controller").decode(
                        errors="replace"
                    )[:180]
                    self.handles[instance] = (handle, joystick, mapped, guid, name)
                handle, joystick, mapped, guid, name = self.handles[instance]
                ordinal = ordinals.get(guid, 0)
                ordinals[guid] = ordinal + 1
                serial = self.library.SDL_JoystickGetSerial(joystick)
                tag = hashlib.sha256(serial).hexdigest()[:16] if serial else str(ordinal)
                identity = "sdl:" + guid + ":" + tag
                self._indices[identity] = instance
                found.append(Device(identity, name, mapped))
            for instance in list(self.handles):
                if instance not in identities:
                    handle, _joystick, mapped, _guid, _name = self.handles.pop(instance)
                    (
                        self.library.SDL_GameControllerClose
                        if mapped
                        else self.library.SDL_JoystickClose
                    )(handle)
            self.devices = found
            return list(found)

    def read(self, identity="", layout=0):
        if not self.library:
            return Frame()
        with _lock:
            devices = self.scan()
            if not devices:
                return Frame()
            chosen = next((device for device in devices if device.identity == identity), None)
            if identity and chosen is None:
                return Frame()
            chosen = chosen or devices[0]
            entry = self.handles.get(self._indices.get(chosen.identity))
            if not entry:
                return Frame()
            handle, joystick, mapped, _guid, name = entry
            if not self.library.SDL_JoystickGetAttached(joystick):
                self._next_scan = 0.0
                return Frame()
            self.library.SDL_GameControllerUpdate()
            if mapped:
                axes = [
                    self.library.SDL_GameControllerGetAxis(handle, axis) / 32767
                    for axis in range(6)
                ]
                button_map = (0, 1, 2, 3, 9, 10, 4, 6, 7, 8, 11, 12, 13, 14)
                buttons = sum(
                    bool(self.library.SDL_GameControllerGetButton(handle, button)) << index
                    for index, button in enumerate(button_map)
                )
            else:
                count = min(16, self.library.SDL_JoystickNumButtons(joystick))
                sony = (
                    layout == 2
                    or not layout
                    and any(
                        part in name.casefold()
                        for part in ("sony", "dualshock", "dualsense", "playstation")
                    )
                )
                button_map = (1, 2, 0, 3, 4, 5, 8, 9, 10, 11) if sony else tuple(range(10))
                buttons = sum(
                    (button < count and bool(self.library.SDL_JoystickGetButton(joystick, button)))
                    << index
                    for index, button in enumerate(button_map)
                )
                hat = (
                    self.library.SDL_JoystickGetHat(joystick, 0)
                    if self.library.SDL_JoystickNumHats(joystick)
                    else 0
                )
                for index, mask in enumerate((1, 4, 8, 2)):
                    if hat & mask:
                        buttons |= 1 << (10 + index)
                count = min(6, self.library.SDL_JoystickNumAxes(joystick))
                axes = [
                    self.library.SDL_JoystickGetAxis(joystick, axis) / 32767
                    if axis < count
                    else 0.0
                    for axis in range(6)
                ]
            axes = [max(-1.0, min(1.0, value)) for value in axes]
            return Frame(True, buttons, *axes[:4], max(0.0, axes[4]), max(0.0, axes[5]))

    def close(self):
        with _lock:
            if not self.library:
                return
            for handle, _joystick, mapped, _guid, _name in self.handles.values():
                (
                    self.library.SDL_GameControllerClose
                    if mapped
                    else self.library.SDL_JoystickClose
                )(handle)
            self.handles.clear()
            self.devices.clear()
            self._indices.clear()
            self.library.SDL_QuitSubSystem(0x2200)
            self.library = None


DESCRIPTOR = bytes(
    (
        5,
        1,
        9,
        5,
        161,
        1,
        5,
        9,
        25,
        1,
        41,
        16,
        21,
        0,
        37,
        1,
        117,
        1,
        149,
        16,
        129,
        2,
        5,
        1,
        9,
        57,
        21,
        0,
        37,
        7,
        53,
        0,
        70,
        59,
        1,
        101,
        20,
        117,
        4,
        149,
        1,
        129,
        66,
        117,
        4,
        149,
        1,
        129,
        3,
        101,
        0,
        9,
        48,
        9,
        49,
        9,
        50,
        9,
        53,
        22,
        1,
        128,
        38,
        255,
        127,
        54,
        1,
        128,
        70,
        255,
        127,
        117,
        16,
        149,
        4,
        129,
        2,
        5,
        2,
        9,
        197,
        9,
        196,
        21,
        0,
        38,
        255,
        0,
        53,
        0,
        70,
        255,
        0,
        117,
        8,
        149,
        2,
        129,
        2,
        192,
    )
)


def registration():
    return (
        json.dumps(
            {
                "id": 1,
                "command": "register",
                "name": "Velox Controller",
                "vid": 4617,
                "pid": 61441,
                "bus": "usb",
                "descriptor": list(DESCRIPTOR),
            },
            separators=(",", ":"),
        )
        + "\n"
    ).encode()


def report(frame):
    bits = (0, 1, 3, 4, 6, 7, 10, 11, 13, 14, -1, -1, -1, -1, 8, 9)
    buttons = sum(
        1 << bit for index, bit in enumerate(bits) if bit >= 0 and frame.buttons & (1 << index)
    )
    x = bool(frame.buttons & (1 << 13)) - bool(frame.buttons & (1 << 12))
    y = bool(frame.buttons & (1 << 11)) - bool(frame.buttons & (1 << 10))
    hat = {
        (0, -1): 0,
        (1, -1): 1,
        (1, 0): 2,
        (1, 1): 3,
        (0, 1): 4,
        (-1, 1): 5,
        (-1, 0): 6,
        (-1, -1): 7,
    }.get((x, y), 8)
    axes = [
        max(-32767, min(32767, round(value * 32767))) if math.isfinite(value) else 0
        for value in (frame.lx, frame.ly, frame.rx, frame.ry)
    ]
    payload = struct.pack(
        "<HBhhhhBB",
        buttons,
        hat,
        *axes,
        round(max(0.0, min(1.0, frame.lt)) * 255),
        round(max(0.0, min(1.0, frame.rt)) * 255),
    )
    return (
        json.dumps({"id": 1, "command": "report", "report": list(payload)}, separators=(",", ":"))
        + "\n"
    ).encode()


class HidPipe:
    def __init__(self, phone):
        if not phone.alive():
            raise VeloxError("Start the phone before connecting a controller.")
        executable = phone.hub.setup.verify_executable("platform-tools/adb")
        self.process = subprocess.Popen(
            [str(executable), "-s", phone.serial, "shell", "/system/bin/hid -"],
            env=phone.environment(),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            close_fds=True,
        )
        os.set_blocking(self.process.stdin.fileno(), False)
        os.set_blocking(self.process.stdout.fileno(), False)
        self.pending = registration() + report(Frame())
        self.next_report = b""
        self.last = b""
        self.output_size = 0

    def send(self, frame):
        message = report(frame)
        if message != self.last:
            self.next_report = message
            self.last = message
        self.pump()

    def pump(self):
        if self.process.poll() is not None:
            raise VeloxError("The Android controller connection ended.")
        try:
            data = os.read(self.process.stdout.fileno(), 4096)
        except BlockingIOError:
            data = b""
        self.output_size += len(data)
        if self.output_size > 65536 or b"Exception" in data or b"Error parsing" in data:
            raise VeloxError(
                "Android refused the virtual controller. Reopen the phone to reconnect."
            )
        if not self.pending:
            self.pending, self.next_report = self.next_report, b""
        if self.pending:
            try:
                count = os.write(self.process.stdin.fileno(), self.pending)
                self.pending = self.pending[count:]
            except BlockingIOError:
                pass

    def close(self):
        # EOF removes the Android HID device, releasing held buttons even if
        # its final neutral report could not fit in a stalled pipe.
        if self.process.poll() is None:
            with contextlib.suppress(OSError):
                os.write(self.process.stdin.fileno(), report(Frame()))
        self.process.stdin.close()
        try:
            self.process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=2)
        self.process.stdout.close()


class PadWorker:
    """Latest-frame gamepad delivery, isolated from Qt painting and input."""

    def __init__(self, phone, notify, failed):
        self.phone, self.notify, self.failed = phone, notify, failed
        self._condition = threading.Condition()
        self._configuration = None
        self._stop = False
        self._latest = None
        self._notified = False
        self.closed = threading.Event()
        self.devices = []
        self._thread = threading.Thread(
            target=self._run, name="Velox controller input", daemon=True
        )
        self._thread.start()

    def configure(self, profile, active):
        with self._condition:
            self._configuration = copy.deepcopy(profile), bool(active)
            self._condition.notify()

    def consume(self):
        with self._condition:
            frame, self._latest = self._latest, None
            self._notified = False
            return frame

    def publish(self, frame):
        with self._condition:
            self._latest = frame
            notify = not self._notified
            self._notified = True
        if notify:
            self.notify()

    def _run(self):
        reader = native = None
        previous, retry, applied = Frame(), 0.0, None
        try:
            while True:
                with self._condition:
                    if self._stop:
                        break
                    configuration = self._configuration
                if configuration is not applied:
                    previous = None
                    applied = configuration
                profile, active = configuration if configuration else (None, False)
                active = (
                    active and profile and profile.pad_mode != 0 and self.phone.state == "running"
                )
                if not active:
                    if native:
                        native.close()
                        native = None
                    if previous != Frame():
                        previous = Frame()
                        self.publish(previous)
                    with self._condition:
                        if not self._stop and self._configuration is configuration:
                            self._condition.wait()
                    continue
                try:
                    if reader is None:
                        if time.monotonic() < retry:
                            with self._condition:
                                self._condition.wait(0.2)
                            continue
                        reader = Reader()
                        if not reader.library:
                            raise VeloxError(
                                "The native controller library is unavailable. Repair this Velox installation."
                            )
                    frame = filtered(
                        reader.read(profile.pad_device, profile.pad_layout),
                        profile.deadzone,
                        profile.sensitivity,
                        profile.invert_y,
                    )
                    self.devices = list(reader.devices)
                    if profile.pad_mode == 1 and frame.connected:
                        if native is None and time.monotonic() >= retry:
                            native = HidPipe(self.phone)
                        if native:
                            native.send(frame)
                    elif native:
                        native.close()
                        native = None
                    if frame != previous:
                        previous = frame
                        self.publish(frame)
                except (OSError, VeloxError):
                    if reader and not reader.library:
                        reader = None
                    if native:
                        native.close()
                        native = None
                    if time.monotonic() >= retry:
                        self.failed(
                            "Controller input disconnected. Reopen the phone or reconnect your controller."
                        )
                        retry = time.monotonic() + 5
                    if previous != Frame():
                        previous = Frame()
                        self.publish(previous)
                with self._condition:
                    if not self._stop and self._configuration is configuration:
                        self._condition.wait(0.008)
        finally:
            if native:
                with contextlib.suppress(OSError):
                    native.close()
            if reader:
                reader.close()
            self.closed.set()

    def close(self):
        with self._condition:
            self._stop = True
            self._condition.notify()
