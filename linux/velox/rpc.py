from __future__ import annotations

import dataclasses
import importlib
import mmap
import os
import shutil
import sys
import threading
import time
import uuid
from pathlib import Path

import grpc
from google.protobuf.empty_pb2 import Empty

from .core import VeloxError, private_directory
from .setup import resource_root


def protocol():
    folder = resource_root() / "protocol"
    if str(folder) not in sys.path:
        sys.path.insert(0, str(folder))
    return importlib.import_module("emulator_controller_pb2"), importlib.import_module(
        "emulator_controller_pb2_grpc"
    )


@dataclasses.dataclass(frozen=True)
class Frame:
    pixels: bytes
    width: int
    height: int
    rotation: int
    sequence: int
    timestamp_us: int


class FramePacer:
    def __init__(self):
        self.deadline = 0.0
        self.rate = 0

    def accepts(self, current, rate):
        rate = max(1, min(240, rate))
        interval = 1 / rate
        if rate != self.rate or current - self.deadline > interval * 2:
            self.rate = rate
            self.deadline = current
        if current + min(0.0005, interval * 0.05) < self.deadline:
            return False
        # Preserve phase across small delivery jitter. Restarting from every
        # received frame can turn a 60 Hz cap into irregular 40 Hz delivery.
        self.deadline += interval
        return True


class Connection:
    def __init__(self, phone):
        self.phone = phone
        self.pb, services = protocol()
        runtime = phone.runtime()
        token = runtime.get("token")
        if not isinstance(token, str) or not 16 <= len(token) <= 4096:
            raise VeloxError("The phone's authenticated display connection is not ready.")
        self.metadata = (("authorization", "Bearer " + token),)
        self.channel = grpc.insecure_channel(
            "127.0.0.1:" + str(phone.metadata["grpc"]),
            options=(
                ("grpc.max_receive_message_length", 96 * 1024 * 1024),
                ("grpc.keepalive_time_ms", 15000),
                ("grpc.keepalive_timeout_ms", 5000),
                ("grpc.enable_retries", 0),
            ),
        )
        self.stub = services.EmulatorControllerStub(self.channel)

    def rotate(self, quadrant: int) -> None:
        if quadrant not in range(4):
            raise VeloxError("Unsupported phone rotation.")
        self.stub.setPhysicalModel(
            self.pb.PhysicalModelValue(
                target=self.pb.PhysicalModelValue.ROTATION,
                value=self.pb.ParameterValue(data=[0.0, 0.0, float((0, 90, 180, 270)[quadrant])]),
                interpolation=self.pb.PhysicalModelValue.STEP,
            ),
            metadata=self.metadata,
            timeout=4,
        )

    def clipboard(self, text=None):
        if text is None:
            result = self.stub.getClipboard(Empty(), metadata=self.metadata, timeout=3).text
            if len(result) > 1024 * 1024:
                raise VeloxError("The phone clipboard is too large to copy.")
            return result
        if not isinstance(text, str) or len(text) > 1024 * 1024 or "\x00" in text:
            raise VeloxError("Choose text without NUL characters and smaller than 1 MB.")
        self.stub.setClipboard(self.pb.ClipData(text=text), metadata=self.metadata, timeout=3)

    def screenshot(self, width=1600, height=1600):
        return self.stub.getScreenshot(
            self.pb.ImageFormat(format=self.pb.ImageFormat.PNG, width=width, height=height),
            metadata=self.metadata,
            timeout=8,
        )

    def close(self) -> None:
        self.channel.close()


class Video:
    """Bounded latest-frame delivery: rendering never queues old video frames."""

    def __init__(self, connection: Connection, notify, error, width=1600, height=1600, shared=True):
        self.connection, self.notify, self.error = connection, notify, error
        self.width, self.height = min(3840, max(64, width)), min(3840, max(64, height))
        self.shared = shared
        self.latest: Frame | None = None
        self._pending = False
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._call = None
        self._thread = None
        self.received = 0
        self.dropped = 0
        self.rate = 144
        self._pacer = FramePacer()

    def start(self):
        if self._thread and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="Velox video", daemon=True)
        self._thread.start()

    def consume(self) -> Frame | None:
        with self._lock:
            result = self.latest
            self.latest = None
            self._pending = False
            return result

    def _run(self):
        while not self._stop.is_set():
            try:
                self._stream()
                return
            except grpc.RpcError as error:
                if self._stop.is_set():
                    return
                if self.shared and error.code() in (
                    grpc.StatusCode.INVALID_ARGUMENT,
                    grpc.StatusCode.UNIMPLEMENTED,
                ):
                    self.shared = False
                    continue
                self.error("The phone display disconnected. Reopen the phone view to reconnect.")
                return
            except (VeloxError, OSError, ValueError) as error:
                if not self._stop.is_set():
                    self.error(str(error))
                return

    def _stream(self):
        mapped = None
        descriptor = None
        file = None
        try:
            pb = self.connection.pb
            request = pb.ImageFormat(
                format=pb.ImageFormat.RGBA8888, width=self.width, height=self.height
            )
            if self.shared:
                try:
                    # Pixel traffic belongs in RAM, not an mmap file that can
                    # continuously dirty the user's SSD while a game is active.
                    root = private_directory(Path("/dev/shm") / f"velox-{os.getuid()}")
                    if shutil.disk_usage(root).free < self.width * self.height * 8:
                        raise OSError("Not enough shared memory")
                    file = root / (uuid.uuid4().hex + ".rgba")
                    descriptor = os.open(
                        file,
                        os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
                        0o600,
                    )
                    os.posix_fallocate(descriptor, 0, self.width * self.height * 4)
                    mapped = mmap.mmap(descriptor, self.width * self.height * 4)
                    request.transport.channel = pb.ImageTransport.MMAP
                    request.transport.handle = file.as_uri()
                except (OSError, VeloxError):
                    if descriptor is not None:
                        os.close(descriptor)
                        descriptor = None
                    if file is not None:
                        file.unlink(missing_ok=True)
                        file = None
                    self.shared = False
            self._call = self.connection.stub.streamScreenshot(
                request, metadata=self.connection.metadata
            )
            for packet in self._call:
                if self._stop.is_set():
                    break
                width, height = packet.format.width, packet.format.height
                count = width * height * 4
                if not width or not height:
                    continue  # The engine reports an inactive display during boot.
                if width > self.width or height > self.height or count > 64 * 1024 * 1024:
                    raise VeloxError("The phone returned an invalid video frame.")
                current = time.monotonic()
                if not self._pacer.accepts(current, self.rate):
                    self.dropped += 1
                    continue
                if mapped is not None:
                    if packet.image:
                        raise VeloxError("Unexpected video transport response.")
                    pixels = mapped[:count]
                else:
                    pixels = packet.image
                    if len(pixels) != count:
                        raise VeloxError("The phone returned an incomplete video frame.")
                frame = Frame(
                    pixels,
                    width,
                    height,
                    packet.format.rotation.rotation,
                    packet.seq,
                    packet.timestampUs,
                )
                signal = False
                with self._lock:
                    if self.latest is not None:
                        self.dropped += 1
                    self.latest = frame
                    self.received += 1
                    if not self._pending:
                        self._pending = True
                        signal = True
                if signal:
                    self.notify()
        finally:
            if self._call is not None:
                self._call.cancel()
            if mapped is not None:
                mapped.close()
            if descriptor is not None:
                os.close(descriptor)
            if file is not None:
                file.unlink(missing_ok=True)

    def close(self):
        self._stop.set()
        if self._call is not None:
            self._call.cancel()
        if self._thread and self._thread is not threading.current_thread():
            self._thread.join(timeout=3)
        with self._lock:
            self.latest = None
            self._pending = False
