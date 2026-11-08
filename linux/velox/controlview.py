from __future__ import annotations

import copy
import math

from PySide6.QtCore import QPointF, QRectF, Qt, Signal, QTimer
from PySide6.QtGui import QColor, QFont, QPainter, QPen
from PySide6.QtWidgets import QDialog, QHBoxLayout, QVBoxLayout, QWidget

from .controls import Binding, Controls, RESERVED, virtual_key
from .core import VeloxError
from .controllers import BUTTONS, Reader as ControllerReader, filtered
from .widgets import button, label


KINDS = (
    "Tap",
    "WASD stick",
    "Swipe",
    "Mouse aim",
    "Left controller stick",
    "Right controller stick",
)


class KeyCapture(QDialog):
    def __init__(self, parent, translate):
        super().__init__(parent)
        self.key = 0
        self.tr = translate
        self.setWindowTitle(translate("Choose a key"))
        layout = QVBoxLayout(self)
        self.message = label(
            translate("Press a key, mouse button or controller button. Escape cancels."), wrap=True
        )
        layout.addWidget(self.message)
        self.resize(300, 120)
        self.controller = ControllerReader()
        self.pad_neutral = False
        self.timer = QTimer(self)
        self.timer.setInterval(30)
        self.timer.timeout.connect(self.read_pad)
        self.timer.start()
        self.finished.connect(lambda _result: (self.timer.stop(), self.controller.close()))

    def read_pad(self):
        frame = filtered(self.controller.read())
        if not frame.buttons:
            self.pad_neutral = True
        elif self.pad_neutral:
            self.key = 256 + (frame.buttons & -frame.buttons).bit_length() - 1
            self.accept()

    def keyPressEvent(self, event):
        if event.key() == Qt.Key.Key_Escape:
            self.reject()
            return
        key = virtual_key(
            event.key(),
            event.nativeScanCode(),
            bool(event.modifiers() & Qt.KeyboardModifier.KeypadModifier),
        )
        if key < 1 or key in RESERVED:
            self.message.setText(self.tr("That key is reserved for the desktop or Velox."))
            return
        self.key = key
        self.accept()

    def mousePressEvent(self, event):
        keys = {
            Qt.MouseButton.LeftButton: 1,
            Qt.MouseButton.RightButton: 2,
            Qt.MouseButton.MiddleButton: 4,
        }
        if event.button() in keys:
            self.key = keys[event.button()]
            self.accept()


class ControlOverlay(QWidget):
    closed = Signal(bool)
    changed = Signal()

    def __init__(self, view, identity, landscape, translate):
        super().__init__(view.canvas)
        self.view, self.identity, self.landscape, self.tr = view, identity, landscape, translate
        self.controls = Controls(view.phone)
        self.original = self.controls.load(identity, landscape)
        self.profile = copy.deepcopy(self.original)
        self.selected = -1
        self.kind = None
        self.first = None
        self.dragging = None
        self.panel_top = False
        self.setObjectName("control-overlay")
        self.setStyleSheet("QWidget#control-overlay { background: transparent; }")
        self.setAttribute(Qt.WidgetAttribute.WA_NoSystemBackground)
        self.setMouseTracking(True)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.panel = QWidget(self)
        self.panel.setObjectName("card")
        box = QVBoxLayout(self.panel)
        box.setContentsMargins(8, 6, 8, 6)
        box.setSpacing(5)
        self.hint = label(translate("Choose a tool, then click the phone."), wrap=True)
        box.addWidget(self.hint)
        first_row, second_row = QHBoxLayout(), QHBoxLayout()
        box.addLayout(first_row)
        box.addLayout(second_row)
        for kind, name in enumerate(("Tap", "WASD", "Swipe", "Mouse aim", "Pad L", "Pad R")):
            tool = button(translate(name), lambda kind=kind: self.choose(kind))
            tool.setStyleSheet("padding: 6px;")
            tool.setToolTip(translate(KINDS[kind]))
            (first_row if kind < 3 else second_row).addWidget(tool)
        actions = QHBoxLayout()
        box.addLayout(actions)
        actions.addWidget(button(translate("Delete"), self.remove))
        actions.addWidget(button(translate("Cancel"), lambda: self.finish(False)))
        actions.addWidget(button(translate("Save"), lambda: self.finish(True), True))
        move = button("↕", self.move_panel)
        move.setToolTip(translate("Move tools to the other edge"))
        actions.addWidget(move)
        self.setGeometry(view.canvas.rect())
        self.show()
        self.raise_()
        self.setFocus()

    def resizeEvent(self, _event):
        self.panel.setFixedWidth(min(self.width() - 12, 620))
        self.panel.adjustSize()
        self.panel.move(
            max(6, (self.width() - self.panel.width()) // 2),
            8 if self.panel_top else self.height() - self.panel.height() - 8,
        )

    def move_panel(self):
        self.panel_top = not self.panel_top
        self.resizeEvent(None)

    def choose(self, kind):
        if kind in (1, 3, 4, 5):
            existing = next(
                (index for index, item in enumerate(self.profile.bindings) if item.kind == kind),
                None,
            )
            if existing is not None:
                self.selected, self.kind, self.first = existing, None, None
                self.hint.setText(self.tr("This control is already placed. Drag it to move it."))
                self.update()
                return
        if len(self.profile.bindings) >= 14:
            self.hint.setText(self.tr("A profile supports up to 14 bindings."))
            return
        self.kind, self.first = kind, None
        self.hint.setText(
            self.tr("Click the start and end of the swipe.")
            if kind == 2
            else self.tr("Click where this control should touch the phone.")
        )
        self.setFocus()
        self.update()

    def position(self, x, y):
        frame = self.view.canvas.frame
        if not frame:
            return QPointF()
        padding = 14 if self.view.canvas.surface.frame_enabled else 0
        ratio = min(
            (self.width() - padding) / frame.width, (self.height() - padding) / frame.height
        )
        width, height = frame.width * ratio, frame.height * ratio
        return QPointF(
            (self.width() - width) / 2 + x * width, (self.height() - height) / 2 + y * height
        )

    def paintEvent(self, _event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.fillRect(self.rect(), QColor(4, 6, 16, 65))
        font = QFont()
        font.setPixelSize(12)
        font.setBold(True)
        painter.setFont(font)
        for index, binding in enumerate(self.profile.bindings):
            center = self.position(binding.x, binding.y)
            radius = (
                max(18, binding.radius * min(self.width(), self.height()))
                if binding.kind in (1, 3, 4, 5)
                else 19
            )
            painter.setPen(QPen(QColor("#b49aff" if index == self.selected else "#7185ed"), 2))
            painter.setBrush(QColor(77, 61, 132, 140))
            painter.drawEllipse(center, radius, radius)
            if binding.kind == 2:
                end = self.position(binding.x2, binding.y2)
                painter.drawLine(center, end)
                painter.drawEllipse(end, 8, 8)
            name = (
                "WASD"
                if binding.kind == 1
                else "AIM"
                if binding.kind == 3
                else "L"
                if binding.kind == 4
                else "R"
                if binding.kind == 5
                else key_label(binding.key)
            )
            painter.setPen(QColor("#f4f2ff"))
            painter.drawText(
                QRectF(center.x() - radius, center.y() - 11, radius * 2, 22),
                Qt.AlignmentFlag.AlignCenter,
                name,
            )
        if self.first:
            painter.setPen(QPen(QColor("#ad89ff"), 2))
            painter.drawEllipse(self.position(*self.first), 10, 10)
        painter.end()

    def mousePressEvent(self, event):
        self.setFocus()
        point = self.view.canvas.point(event.position())
        if not point:
            return
        x, y = point[0] / 10000, point[1] / 10000
        if self.kind is not None:
            if self.kind == 2 and self.first is None:
                self.first = x, y
                self.update()
                return
            used = {binding.key for binding in self.profile.bindings}
            key = (
                87
                if self.kind == 1
                else next((value for value in range(248, 256) if value not in used), 0)
                if self.kind in (3, 4, 5)
                else 0
            )
            if not key:
                dialog = KeyCapture(self, self.tr)
                if dialog.exec() != QDialog.DialogCode.Accepted:
                    return
                key = dialog.key
            for binding in self.profile.bindings:
                if binding.key == key and binding.kind in (3, 4, 5) and self.kind not in (3, 4, 5):
                    binding.key = next(
                        (value for value in range(248, 256) if value not in used and value != key),
                        binding.key,
                    )
                    used.add(binding.key)
            if any(binding.key == key for binding in self.profile.bindings):
                self.hint.setText(self.tr("That key already has a binding in this orientation."))
                return
            start = self.first or (x, y)
            self.profile.bindings.append(
                Binding(self.kind, key, *start, x if self.first else min(1, x + 0.2), y, 0.12)
            )
            if self.kind in (4, 5) or key >= 256:
                self.profile.pad_mode = 2
            self.selected = len(self.profile.bindings) - 1
            self.kind, self.first = None, None
            self.hint.setText(
                self.tr("Drag a control to move it. Use the wheel to change its radius.")
            )
            self.update()
            return
        self.selected, self.dragging = -1, None
        for index, binding in reversed(list(enumerate(self.profile.bindings))):
            for endpoint in (True, False) if binding.kind == 2 else (False,):
                center = (
                    self.position(binding.x2, binding.y2)
                    if endpoint
                    else self.position(binding.x, binding.y)
                )
                if (
                    math.hypot(center.x() - event.position().x(), center.y() - event.position().y())
                    < 25
                ):
                    self.selected, self.dragging = index, endpoint
                    self.update()
                    return
        self.update()

    def mouseMoveEvent(self, event):
        if self.selected >= 0 and self.dragging is not None:
            point = self.view.canvas.point(event.position())
            if not point:
                return
            binding = self.profile.bindings[self.selected]
            if self.dragging:
                binding.x2, binding.y2 = point[0] / 10000, point[1] / 10000
            else:
                binding.x, binding.y = point[0] / 10000, point[1] / 10000
            self.update()

    def mouseReleaseEvent(self, _event):
        self.dragging = None

    def wheelEvent(self, event):
        if self.selected >= 0:
            binding = self.profile.bindings[self.selected]
            binding.radius = min(
                0.3, max(0.02, binding.radius + (0.01 if event.angleDelta().y() > 0 else -0.01))
            )
            self.update()

    def keyPressEvent(self, event):
        if event.key() == Qt.Key.Key_Delete:
            self.remove()
        elif event.key() == Qt.Key.Key_Escape:
            self.finish(False)

    def remove(self):
        if self.selected >= 0:
            del self.profile.bindings[self.selected]
            self.selected = -1
            self.update()

    def finish(self, save):
        if save:
            try:
                self.profile.enabled = True
                self.controls.save(self.identity, self.landscape, self.profile)
            except (VeloxError, OSError, TypeError) as error:
                self.hint.setText(str(error))
                return
        self.hide()
        self.closed.emit(save)
        self.deleteLater()


def key_label(key):
    if 256 <= key < 272:
        return "Pad " + BUTTONS[key - 256]
    return {
        1: "M1",
        2: "M2",
        4: "M3",
        16: "Shift",
        17: "Ctrl",
        32: "Space",
        37: "←",
        38: "↑",
        39: "→",
        40: "↓",
    }.get(key, chr(key) if 33 <= key <= 90 else f"F{key - 111}" if 112 <= key <= 135 else str(key))
