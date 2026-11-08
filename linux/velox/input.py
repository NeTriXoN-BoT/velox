from __future__ import annotations

import contextlib
import base64
import os
import re
import selectors
import secrets
import subprocess
import threading
import time
from collections import deque

from .core import VeloxError
from .experience import Experience


class InputPipe:
    """Focus-local input. Slow Android I/O can never block the desktop event loop."""

    def __init__(
        self,
        phone,
        failed=lambda _message: None,
        display=lambda _width, _height, _rotation, _app: None,
    ):
        self.phone, self.failed = phone, failed
        self.display = display
        self._queue = deque()
        self._condition = threading.Condition()
        self._stop = threading.Event()
        self._thread = None
        self.ready = threading.Event()
        self._flushed = threading.Event()
        self.process = None
        self._transaction = threading.Lock()
        self._response = threading.Event()
        self._response_id = None
        self._response_result = None
        self._response_data = bytearray()
        self._response_size = None

    def start(self):
        if self._thread and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="Velox touch input", daemon=True)
        self._thread.start()

    def send(self, operation: str, contact=0, x=0, y=0):
        if (
            operation not in ("D", "M", "U", "C")
            or type(contact) is not int
            or not 0 <= contact <= 15
        ):
            raise ValueError("Invalid touch event")
        event = (operation, contact, min(10000, max(0, int(x))), min(10000, max(0, int(y))))
        with self._condition:
            if self._stop.is_set():
                return
            if operation == "C":
                self._queue.clear()
            elif operation == "M":
                for index in range(len(self._queue) - 1, -1, -1):
                    previous = self._queue[index]
                    if previous[0] in ("K", "P", "R"):
                        break
                    if previous[0] == "C":
                        break
                    if previous[1] == contact:
                        if previous[0] == "M":
                            self._queue[index] = event
                            return
                        break
            if len(self._queue) >= 256:
                self._queue.clear()
                self._queue.append(("C", 0, 0, 0))
                self.failed("Android input is busy. Contacts were released; try again shortly.")
            else:
                self._queue.append(event)
            self._condition.notify()

    def key(self, code, pressed, modifiers=0):
        if type(code) is not int or not 0 <= code <= 288 or modifiers & ~0x111003:
            raise ValueError("Invalid Android key event")
        with self._condition:
            if self._stop.is_set():
                return
            if len(self._queue) >= 256:
                self._queue.clear()
                self._queue.append(("C", 0, 0, 0))
                raise VeloxError("The phone input is busy. Wait a moment and try again.")
            self._queue.append(("K", code, 0 if pressed else 1, modifiers))
            self._condition.notify()

    def flush(self):
        if self._stop.is_set():
            raise VeloxError("The phone input connection is closed.")
        self._flushed.clear()
        with self._condition:
            self._queue.append(("P", 0, 0, 0))
            self._condition.notify()
        if not self._flushed.wait(3):
            raise VeloxError("The phone input did not respond in time.")

    def _request(self, command):
        with self._transaction:
            identity = secrets.token_hex(8)
            self._response.clear()
            self._response_id, self._response_result, self._response_size = identity, None, None
            self._response_data = bytearray()
            with self._condition:
                if self._stop.is_set():
                    raise VeloxError("The phone input connection is closed.")
                if len(self._queue) >= 256:
                    raise VeloxError("The phone input is busy. Wait a moment and try again.")
                self._queue.append(("R", command(identity), 0, 0))
                self._condition.notify()
            if not self._response.wait(6):
                raise VeloxError("The phone clipboard did not respond in time.")
            if isinstance(self._response_result, Exception):
                raise self._response_result
            return self._response_result

    def paste(self, text, preserve=True):
        content = text.encode("utf-8")
        if len(content) > 1024 * 1024 or b"\x00" in content:
            raise VeloxError("Choose text without NUL characters and smaller than 1 MB.")

        def command(identity):
            result = bytearray(f"B {identity} {int(preserve)} {len(content)}\n".encode())
            for offset in range(0, len(content), 2304):
                result.extend(b"T " + base64.b64encode(content[offset : offset + 2304]) + b"\n")
            result.extend(f"E {identity}\n".encode())
            return result

        self._request(command)

    def clipboard(self):
        return self._request(lambda identity: f"G {identity}\n".encode())

    def _clipboard_reply(self, line):
        identity = self._response_id
        if not identity or self._response.is_set():
            return
        if line.startswith(("TEXT " + identity + " ").encode()):
            self._response_result = (
                True if line.endswith(b" OK") else VeloxError("The phone couldn't paste that text.")
            )
            self._response.set()
            return
        prefix = ("CLIP " + identity + " ").encode()
        if not line.startswith(prefix):
            return
        value = line[len(prefix) :]
        try:
            if value == b"NONE":
                self._response_result = None
                self._response.set()
            elif re.fullmatch(rb"BEGIN [0-9]{1,7}", value):
                length = int(value[6:])
                if length > 1024 * 1024 or self._response_size is not None:
                    raise ValueError("Invalid clipboard size")
                self._response_size = length
            elif value.startswith(b"DATA "):
                if self._response_size is None or len(value) > 3077:
                    raise ValueError("Invalid clipboard block")
                block = base64.b64decode(value[5:], validate=True)
                if len(self._response_data) + len(block) > self._response_size:
                    raise ValueError("Clipboard size exceeded")
                self._response_data.extend(block)
            elif value == b"END" and self._response_size == len(self._response_data):
                self._response_result = self._response_data.decode("utf-8")
                self._response_data.clear()
                self._response.set()
            else:
                raise ValueError("Invalid clipboard response")
        except (ValueError, UnicodeError):
            self._response_result = VeloxError("The phone clipboard could not be read.")
            self._response_data.clear()
            self._response.set()

    def _run(self):
        process = None
        try:
            command = Experience(self.phone).input_command()
            command[-1] += " --display"
            if self._stop.is_set():
                return
            process = subprocess.Popen(
                command,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                env=self.phone.environment(),
                close_fds=True,
            )
            self.process = process
            os.set_blocking(process.stdin.fileno(), False)
            os.set_blocking(process.stdout.fileno(), False)
            pending = bytearray()
            reply = bytearray()
            deadline = time.monotonic() + 15
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                while not self._stop.is_set():
                    if process.poll() is not None:
                        raise VeloxError(
                            "The phone input connection ended. Reopen the phone view to reconnect."
                        )
                    if not self.ready.is_set() and time.monotonic() > deadline:
                        raise VeloxError("The phone input connection did not respond in time.")
                    if self.ready.is_set() and not pending:
                        with self._condition:
                            events = list(self._queue)
                            self._queue.clear()
                        for operation, contact, x, y in events:
                            if operation == "R":
                                pending.extend(contact)
                                continue
                            pending.extend(
                                (
                                    operation + "\n"
                                    if operation in ("C", "P")
                                    else f"U {contact}\n"
                                    if operation == "U"
                                    else f"{operation} {contact} {x} {y}\n"
                                ).encode()
                            )
                    if pending and len(selector.get_map()) == 1:
                        selector.register(process.stdin, selectors.EVENT_WRITE)
                    for key, _mask in selector.select(0.008 if self.ready.is_set() else 0.05):
                        if key.fileobj is process.stdin:
                            count = os.write(key.fd, pending)
                            del pending[:count]
                            if not pending:
                                selector.unregister(process.stdin)
                        else:
                            block = os.read(key.fd, 4096)
                            if not block:
                                raise VeloxError("The phone input connection closed.")
                            reply.extend(block)
                            if len(reply) > 65536 or b"VELOX_INPUT_ERROR" in reply:
                                raise VeloxError(
                                    "The phone refused game input. Reopen its view to reconnect."
                                )
                            while b"\n" in reply:
                                line, _separator, tail = reply.partition(b"\n")
                                reply[:] = tail
                                line = line.rstrip(b"\r")
                                self._clipboard_reply(line)
                                if line.strip() == b"VELOX_INPUT_READY":
                                    self.ready.set()
                                elif line.strip() == b"PONG":
                                    self._flushed.set()
                                match = re.fullmatch(
                                    rb"STATE ([0-9]{1,5}) ([0-9]{1,5}) ([0-3]) ([A-Za-z0-9_.-]{1,199})\r?",
                                    line,
                                )
                                if match:
                                    width, height, rotation = map(int, match.groups()[:3])
                                    if 0 < width <= 16384 and 0 < height <= 16384:
                                        self.display(width, height, rotation, match[4].decode())
        except (OSError, VeloxError) as error:
            if not self._stop.is_set():
                self.failed(str(error))
        finally:
            self.ready.clear()
            self._response_result = VeloxError("The phone input connection closed.")
            self._response.set()
            if process:
                # EOF is also an Android-side cancel-all operation. Do not leave
                # a held movement key behind after focus loss or a closed view.
                with contextlib.suppress(OSError):
                    os.write(process.stdin.fileno(), b"C\nQ\n")
                process.stdin.close()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=2)
                process.stdout.close()
            self.process = None

    def close(self):
        self._stop.set()
        with self._condition:
            self._queue.clear()
            self._condition.notify()
        if self._thread and self._thread is not threading.current_thread():
            self._thread.join(timeout=3)


class KeyPipe:
    def __init__(self, connection, input_pipe, failed=lambda _message: None):
        self.connection, self.failed = connection, failed
        self.input = input_pipe
        self._queue = deque()
        self._condition = threading.Condition()
        self._stop = False
        self._held = set()
        self._thread = threading.Thread(target=self._run, name="Velox keyboard input", daemon=True)
        self._thread.start()

    def send(self, name: str, pressed: bool):
        if not name or len(name) > 64:
            return
        with self._condition:
            if self._stop:
                return
            if len(self._queue) >= 256:
                self._queue.clear()
                self._queue.append((None, False))
            else:
                self._queue.append((name, pressed))
            self._condition.notify()

    def release(self):
        with self._condition:
            self._queue.clear()
            self._queue.append((None, False))
            self._condition.notify()

    def text(self, text, preserve=True):
        if not isinstance(text, str) or len(text) > 1024 * 1024 or "\x00" in text:
            raise VeloxError("Choose text without NUL characters and smaller than 1 MB.")
        if not text:
            return
        with self._condition:
            if self._stop:
                return
            if len(self._queue) >= 256:
                raise VeloxError("The phone input is busy. Wait a moment and try again.")
            self._queue.append((None, (text, preserve)))
            self._condition.notify()

    def copy(self, received):
        with self._condition:
            if self._stop:
                return
            if len(self._queue) >= 256:
                raise VeloxError("The phone input is busy. Wait a moment and try again.")
            self._queue.append((None, received))
            self._condition.notify()

    def _run(self):
        def event(name, pressed):
            keys = {
                "Backspace": 67,
                "Enter": 66,
                "Tab": 61,
                "Escape": 111,
                "Delete": 112,
                "ArrowLeft": 21,
                "ArrowRight": 22,
                "ArrowUp": 19,
                "ArrowDown": 20,
                "Home": 122,
                "End": 123,
                "Shift": 59,
                "Control": 113,
                "Alt": 57,
                "Meta": 117,
                "GoBack": 4,
                "GoHome": 3,
                "AppSwitch": 187,
                "Power": 26,
                "AudioVolumeUp": 24,
                "AudioVolumeDown": 25,
            }
            modifiers = (
                (0x1 if "Shift" in self._held else 0)
                | (0x2 if "Alt" in self._held else 0)
                | (0x1000 if "Control" in self._held else 0)
                | (0x10000 if "Meta" in self._held else 0)
            )
            if pressed and name in ("Shift", "Alt", "Control", "Meta"):
                modifiers |= {"Shift": 1, "Alt": 2, "Control": 0x1000, "Meta": 0x10000}[name]
            elif not pressed and name in ("Shift", "Alt", "Control", "Meta"):
                modifiers &= ~{"Shift": 1, "Alt": 2, "Control": 0x1000, "Meta": 0x10000}[name]
            code = keys.get(name)
            if len(name) == 1 and name.isascii():
                if name.isalpha():
                    code = 29 + ord(name.lower()) - 97
                    modifiers = modifiers | 1 if name.isupper() else modifiers & ~1
                elif name.isdigit():
                    code = 7 + int(name)
                elif name == " ":
                    code = 62
                else:
                    plain = {
                        ",": 55,
                        ".": 56,
                        "`": 68,
                        "-": 69,
                        "=": 70,
                        "[": 71,
                        "]": 72,
                        "\\": 73,
                        ";": 74,
                        "'": 75,
                        "/": 76,
                        "@": 77,
                        "+": 81,
                        "*": 17,
                        "#": 18,
                    }
                    shifted = {
                        "!": 8,
                        '"': 75,
                        "$": 11,
                        "%": 12,
                        "&": 14,
                        "(": 16,
                        ")": 7,
                        ":": 74,
                        "<": 55,
                        ">": 56,
                        "?": 76,
                        "^": 13,
                        "_": 69,
                        "{": 71,
                        "|": 73,
                        "}": 72,
                        "~": 68,
                    }
                    code = plain.get(name, shifted.get(name))
                    modifiers = modifiers | 1 if name in shifted else modifiers & ~1
            if code is not None:
                self.input.key(code, pressed, modifiers)

        try:
            while True:
                with self._condition:
                    self._condition.wait_for(lambda: self._stop or self._queue)
                    if self._stop:
                        break
                    name, pressed = self._queue.popleft()
                if name is None:
                    if callable(pressed):
                        # Read only after Android has consumed the preceding
                        # Copy key. A timer can race a busy input worker.
                        pressed(self.input.clipboard())
                        continue
                    if isinstance(pressed, tuple):
                        text, preserve = pressed
                        try:
                            self.input.paste(text, preserve)
                        except VeloxError:
                            self.failed(
                                "That text couldn't be entered. Try pasting the complete text with Ctrl+V."
                            )
                        continue
                    for held in list(self._held):
                        event(held, False)
                    self._held.clear()
                else:
                    event(name, pressed)
                    if pressed:
                        self._held.add(name)
                    else:
                        self._held.discard(name)
        except Exception:
            if not self._stop:
                self.failed("Keyboard input disconnected. Reopen the phone view to reconnect.")
        finally:
            for name in list(self._held):
                with contextlib.suppress(Exception):
                    event(name, False)
            self._held.clear()

    def close(self):
        with self._condition:
            self._stop = True
            self._condition.notify()
        self._thread.join(timeout=7)
