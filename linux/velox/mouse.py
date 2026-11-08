"""Focus-local mouse aim using each desktop's native capture mechanism."""

from __future__ import annotations

import ctypes as C
import time

from PySide6.QtCore import QObject, QPoint, QTimer, Qt, qVersion
from PySide6.QtGui import QCursor
from PySide6.QtWidgets import QApplication

from .core import VeloxError, file_hash, read_json
from .setup import resource_root


class MouseAim(QObject):
    def __init__(self, view):
        super().__init__(view)
        self.view, self.canvas, self.mapper = view, view.canvas, view.mapper
        self.mode = None
        self.last = None
        self.library = self.handle = None
        self.native_identity = None
        self.requested_at = 0.0
        self.saved_cursor = None
        self.timer = QTimer(self)
        self.timer.setInterval(8)
        self.timer.timeout.connect(self.poll)

    def start_drag(self):
        if self.mode:
            return True
        if not self.view.focused() or not self.view.isVisible() or self.view.control_editor:
            return False
        if self.mapper.start_aim():
            self.mode, self.last = "drag", None
            return True
        return False

    def toggle(self):
        if self.mode:
            self.end()
            return
        if (
            not self.view.focused()
            or not self.view.isVisible()
            or self.view.control_editor
            or not self.mapper.profile.enabled
            or not any(item.kind == 3 for item in self.mapper.profile.bindings)
        ):
            return
        platform = QApplication.platformName()
        if platform.startswith("wayland"):
            try:
                self.wayland()
            except (OSError, VeloxError, AttributeError, TypeError):
                self.end()
                self.view.failure.emit(
                    "Mouse capture is unavailable here. Hold the right mouse button to aim."
                )
            return
        if platform != "xcb":
            self.view.failure.emit(
                "Mouse capture is unavailable here. Hold the right mouse button to aim."
            )
            return
        if not self.mapper.start_aim():
            return
        self.canvas.grabMouse()
        if self.canvas.mouseGrabber() is not self.canvas:
            self.mapper.end_aim()
            return
        self.mode = "x11"
        self.saved_cursor = self.canvas.cursor()
        self.canvas.setCursor(Qt.CursorShape.BlankCursor)
        QCursor.setPos(self.canvas.mapToGlobal(self.canvas.rect().center()))

    def wayland(self):
        if tuple(map(int, qVersion().split(".")[:2])) != (6, 12):
            raise VeloxError("This capture adapter requires the bundled Qt version.")
        native = QApplication.instance().nativeInterface()
        identity = tuple(
            int(value or 0)
            for value in (
                native.display(),
                native.compositor(),
                native.pointer(),
                self.view.winId(),
            )
        )
        if any(value < 65536 for value in identity):
            raise VeloxError("Move the mouse onto the phone before capturing it.")
        if self.handle and identity != self.native_identity:
            self.destroy()
        if self.library is None:
            root = resource_root()
            path = root / "native/libvelox-pointer.so"
            expected = read_json(root / "resources.json", {}).get("native/libvelox-pointer.so")
            if not isinstance(expected, str) or len(expected) != 64 or file_hash(path) != expected:
                raise VeloxError("The mouse adapter did not pass its integrity check.")
            library = C.CDLL(str(path))
            pointer, integer = C.c_void_p, C.c_int
            for name, result, arguments in (
                ("velox_pointer_abi", integer, []),
                ("velox_pointer_create", pointer, [pointer] * 4),
                ("velox_pointer_region", None, [pointer] + [integer] * 4),
                ("velox_pointer_request", None, [pointer, integer]),
                (
                    "velox_pointer_poll",
                    integer,
                    [pointer, C.POINTER(C.c_double), C.POINTER(C.c_double)],
                ),
                ("velox_pointer_destroy", None, [pointer]),
            ):
                function = getattr(library, name)
                function.restype, function.argtypes = result, arguments
            if library.velox_pointer_abi() != 1:
                raise VeloxError("The mouse adapter belongs to a different Velox version.")
            self.library = library
        if not self.handle:
            self.handle = self.library.velox_pointer_create(*identity)
            self.native_identity = identity
            if not self.handle:
                raise VeloxError("The Wayland mouse connection is unavailable.")
        self.update_region()
        self.library.velox_pointer_request(self.handle, 1)
        self.mode, self.requested_at = "wayland-pending", time.monotonic()
        self.timer.start()

    def update_region(self):
        if self.handle:
            origin = self.canvas.mapTo(self.view, QPoint(0, 0))
            window = self.view.windowHandle()
            if window:
                margins = window.frameMargins()
                origin += QPoint(margins.left(), margins.top())
            self.library.velox_pointer_region(
                self.handle, origin.x(), origin.y(), self.canvas.width(), self.canvas.height()
            )
            # Wayland constraint regions take effect with the surface commit.
            # Request it even when the Android picture is currently static.
            if window:
                window.requestUpdate()

    def poll(self):
        if not self.view.focused() or not self.view.isVisible() or self.view.control_editor:
            self.end()
            return
        dx, dy = C.c_double(), C.c_double()
        status = self.library.velox_pointer_poll(self.handle, C.byref(dx), C.byref(dy))
        if status == 1:
            if self.mode == "wayland-pending":
                if not self.mapper.start_aim():
                    self.end()
                    return
                self.mode = "wayland"
                self.saved_cursor = self.canvas.cursor()
                self.canvas.setCursor(Qt.CursorShape.BlankCursor)
            self.mapper.aim_delta(
                dx.value / max(1, self.canvas.width()), dy.value / max(1, self.canvas.height())
            )
        elif status != 0 or self.mode == "wayland" or time.monotonic() - self.requested_at > 1:
            pending = self.mode == "wayland-pending"
            self.end()
            if pending:
                self.view.failure.emit(
                    "Mouse capture is unavailable here. Hold the right mouse button to aim."
                )

    def moved(self, point):
        if self.mode == "drag":
            if self.last is not None:
                delta = point - self.last
                self.mapper.aim_delta(
                    delta.x() / max(1, self.canvas.width()),
                    delta.y() / max(1, self.canvas.height()),
                )
            self.last = point
        elif self.mode == "x11" and self.view.focused():
            center = self.canvas.rect().center()
            delta = point - center
            if delta.x() or delta.y():
                self.mapper.aim_delta(
                    delta.x() / max(1, self.canvas.width()),
                    delta.y() / max(1, self.canvas.height()),
                )
                QCursor.setPos(self.canvas.mapToGlobal(center))

    def end(self):
        if self.handle:
            self.library.velox_pointer_request(self.handle, 0)
        if self.mode == "x11":
            self.canvas.releaseMouse()
        self.timer.stop()
        self.mode = self.last = None
        if self.saved_cursor is not None:
            self.canvas.setCursor(self.saved_cursor)
            self.saved_cursor = None
        self.mapper.end_aim()

    def destroy(self):
        self.end()
        if self.handle:
            self.library.velox_pointer_destroy(self.handle)
            self.handle = self.native_identity = None
