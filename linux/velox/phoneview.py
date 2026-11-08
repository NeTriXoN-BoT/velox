from __future__ import annotations

import math
import os
import random
import time

from PySide6.QtCore import (
    QEvent,
    QEasingCurve,
    QPoint,
    QPropertyAnimation,
    QRect,
    QRectF,
    QSize,
    Qt,
    QTimer,
    Signal,
)
from PySide6.QtGui import (
    QColor,
    QFont,
    QImage,
    QLinearGradient,
    QPainter,
    QPainterPath,
    QPen,
    QPlatformSurfaceEvent,
)
from PySide6.QtOpenGLWidgets import QOpenGLWidget
from PySide6.QtWidgets import (
    QApplication,
    QFrame,
    QHBoxLayout,
    QLabel,
    QScrollArea,
    QToolButton,
    QVBoxLayout,
    QWidget,
)

from .core import DISPLAYS, PACKAGE, VeloxError
from .controls import Controls, Mapper, Profile, virtual_key
from .controllers import Frame as PadFrame, PadWorker
from .input import InputPipe, KeyPipe
from .i18n import Translator
from .graphics import effective_rate
from .jobs import Jobs
from .mouse import MouseAim
from .rpc import Connection, Video
from .theme import BLUE, MUTED, PURPLE, brand_icon, icon
from .wallpaper import logo


def phone_size(work_width, work_height, natural_width, natural_height, requested=0, dpi_ratio=1.0):
    # All dimensions here are logical desktop pixels. Qt already applies the
    # compositor's device pixel ratio; applying it a second time makes 4K huge.
    fit = min((work_width - 90) / natural_width, (work_height - 60) / natural_height)
    if requested:
        factor = min(fit, requested / (100 * dpi_ratio))
    else:
        factor = min(fit * 0.82, 1440 / natural_width, 960 / natural_height)
    return max(160, round(natural_width * factor)), max(160, round(natural_height * factor))


def draw_frame(widget):
    painter = QPainter(widget)
    painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
    painter.fillRect(widget.rect(), QColor("#0e1016"))
    image = widget.image
    if image is not None:
        target = (
            QRectF(widget.rect()).adjusted(7, 7, -7, -7)
            if widget.frame_enabled
            else QRectF(widget.rect())
        )
        ratio = min(target.width() / image.width(), target.height() / image.height())
        size = QSize(round(image.width() * ratio), round(image.height() * ratio))
        target = QRectF(
            (widget.width() - size.width()) / 2,
            (widget.height() - size.height()) / 2,
            size.width(),
            size.height(),
        )
        if widget.frame_enabled:
            painter.setRenderHint(QPainter.RenderHint.Antialiasing)
            painter.setBrush(QColor("#080a10"))
            painter.setPen(QPen(QColor("#414657"), 2))
            painter.drawRoundedRect(target.adjusted(-5, -5, 5, 5), 25, 25)
            clip = QPainterPath()
            clip.addRoundedRect(target, 20, 20)
            painter.setClipPath(clip)
        # The pinned engine returns top-to-bottom RGBA, verified against PNG.
        # Paint directly from its immutable bytes; no per-frame flip/copy.
        painter.drawImage(target, image)
    painter.end()


class RasterSurface(QWidget):
    def __init__(self, parent):
        super().__init__(parent)
        self.image = None
        self.frame_enabled = parent.parentWidget().settings.frame
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self.setAttribute(Qt.WidgetAttribute.WA_OpaquePaintEvent)

    def paintEvent(self, _event):
        draw_frame(self)


class GpuSurface(QOpenGLWidget):
    def __init__(self, parent):
        super().__init__(parent)
        self.image = None
        self.frame_enabled = parent.parentWidget().settings.frame
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self.setUpdateBehavior(QOpenGLWidget.UpdateBehavior.NoPartialUpdate)

    def paintGL(self):
        draw_frame(self)


class BootScreen(QWidget):
    def __init__(self, parent, style=0):
        super().__init__(parent)
        self.style = random.randrange(5) if style == 5 else style
        self.message, self.progress = "Starting Android", 0.0
        self.started = time.monotonic()
        self.timer = QTimer(self)
        self.timer.setInterval(33)
        self.timer.timeout.connect(self.update)
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)

    def showEvent(self, _event):
        self.timer.start()

    def hideEvent(self, _event):
        self.timer.stop()

    def paintEvent(self, _event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        w, h, elapsed = self.width(), self.height(), time.monotonic() - self.started
        gradient = QLinearGradient(0, 0, w, h)
        gradient.setColorAt(0, QColor("#101427"))
        gradient.setColorAt(0.55, QColor("#17112a"))
        gradient.setColorAt(1, QColor("#090d17"))
        p.fillRect(self.rect(), gradient)
        center = QPoint(w // 2, h // 2 - 30)
        radius = min(w * 0.28, h * 0.18, 130)
        if self.style == 0:
            for index, color in enumerate((PURPLE, BLUE, "#63d6ed")):
                p.setPen(QPen(QColor(color), 2 + index))
                r = radius + 12 + index * 12
                p.drawArc(
                    QRectF(center.x() - r, center.y() - r, r * 2, r * 2),
                    round(elapsed * (100 + index * 25) * 16),
                    140 * 16,
                )
        elif self.style == 1:
            for index in range(48):
                angle = index * 2 * math.pi / 48
                position = ((elapsed * 0.5 + index * 0.137) % 1) ** 2
                r = radius + position * max(w, h)
                p.setPen(QPen(QColor(PURPLE if index % 3 else BLUE), 1 + position * 2))
                p.drawLine(
                    QPoint(
                        round(center.x() + math.cos(angle) * r),
                        round(center.y() + math.sin(angle) * r),
                    ),
                    QPoint(
                        round(center.x() + math.cos(angle) * (r + 15 + 70 * position)),
                        round(center.y() + math.sin(angle) * (r + 15 + 70 * position)),
                    ),
                )
        elif self.style == 2:
            p.setPen(QPen(QColor("#34316a"), 1))
            for x in range(-w, w * 2, 55):
                p.drawLine(center, QPoint(x, h))
            for index in range(10):
                y = int(h * 0.6 + ((index / 10 + elapsed * 0.08) % 1) ** 2 * h * 0.4)
                p.drawLine(0, y, w, y)
        elif self.style == 3:
            p.setFont(QFont("monospace", 10))
            p.setPen(QColor("#6d7d9c"))
            lines = (
                "velox / system online",
                "[ ok ] hardware acceleration",
                "[ ok ] display connection",
                "[ .. ] " + self.message.lower(),
            )
            for index, line in enumerate(lines):
                if elapsed > index * 0.3:
                    p.drawText(
                        QRectF(24, 34 + index * 23, w - 48, 25), Qt.AlignmentFlag.AlignLeft, line
                    )
            p.setPen(QColor(PURPLE))
            p.drawText(24, 146, "▌" if int(elapsed * 2) % 2 else "")
        if self.style == 4:
            p.setOpacity(0.7 + 0.3 * math.sin(elapsed * 2))
        logo(
            p,
            QRectF(
                center.x() - radius * 0.55, center.y() - radius * 0.55, radius * 1.1, radius * 1.1
            ),
        )
        p.setOpacity(1)
        font = QFont()
        font.setPixelSize(26)
        font.setWeight(QFont.Weight.Bold)
        p.setFont(font)
        p.setPen(QColor("#f1f1fb"))
        p.drawText(
            QRectF(12, center.y() + radius + 36, w - 24, 40), Qt.AlignmentFlag.AlignCenter, "VELOX"
        )
        font.setPixelSize(12)
        font.setWeight(QFont.Weight.Normal)
        p.setFont(font)
        p.setPen(QColor(MUTED))
        p.drawText(QRectF(12, h - 85, w - 24, 30), Qt.AlignmentFlag.AlignCenter, self.message)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QColor("#30334b"))
        p.drawRoundedRect(QRectF(w * 0.2, h - 43, w * 0.6, 3), 2, 2)
        p.setBrush(QColor(PURPLE))
        p.drawRoundedRect(QRectF(w * 0.2, h - 43, w * 0.6 * max(0.02, self.progress), 3), 2, 2)
        p.end()


class Canvas(QFrame):
    pointer = Signal(str, int, int)
    keyboard = Signal(str, bool, int, int, object)
    released = Signal()
    resized = Signal()
    mouse_button = Signal(int, bool)
    text_input = Signal(str)
    motion = Signal(object)

    def __init__(self, parent):
        super().__init__(parent)
        software = os.environ.get("QT_QPA_PLATFORM", "") in ("offscreen", "minimal")
        self.surface = RasterSurface(self) if software else GpuSurface(self)
        self.surface.show()
        self.frame = None
        self.touching = False
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setMouseTracking(True)
        self.setAttribute(Qt.WidgetAttribute.WA_InputMethodEnabled)
        self.setMinimumSize(160, 160)

    def resizeEvent(self, _event):
        self.surface.setGeometry(self.rect())
        parent = self.parentWidget()
        if getattr(parent, "boot", None):
            parent.boot.setGeometry(self.rect())
        if getattr(parent, "control_editor", None):
            parent.control_editor.setGeometry(self.rect())
        if getattr(parent, "mouse_aim", None):
            parent.mouse_aim.update_region()
        self.resized.emit()

    def showEvent(self, _event):
        QTimer.singleShot(150, self.check_graphics)

    def check_graphics(self):
        if isinstance(self.surface, GpuSurface) and self.isVisible():
            context = self.surface.context()
            if not context or not context.isValid():
                previous = self.surface
                self.surface = RasterSurface(self)
                self.surface.image = previous.image
                self.surface.setGeometry(self.rect())
                self.surface.show()
                self.surface.lower()
                previous.hide()
                previous.deleteLater()

    def present(self, frame):
        self.frame = frame  # Keep the immutable pixel buffer alive for QImage.
        self.surface.image = QImage(
            frame.pixels, frame.width, frame.height, frame.width * 4, QImage.Format.Format_RGBA8888
        )
        self.surface.update()

    def point(self, point):
        if not self.frame:
            return None
        padding = 14 if self.surface.frame_enabled else 0
        ratio = min(
            (self.width() - padding) / self.frame.width,
            (self.height() - padding) / self.frame.height,
        )
        width, height = self.frame.width * ratio, self.frame.height * ratio
        left, top = (self.width() - width) / 2, (self.height() - height) / 2
        if not self.touching and not (
            left <= point.x() < left + width and top <= point.y() < top + height
        ):
            return None
        return round(min(1, max(0, (point.x() - left) / width)) * 10000), round(
            min(1, max(0, (point.y() - top) / height)) * 10000
        )

    def mousePressEvent(self, event):
        self.setFocus(Qt.FocusReason.MouseFocusReason)
        if event.button() in (Qt.MouseButton.RightButton, Qt.MouseButton.MiddleButton):
            self.mouse_button.emit(2 if event.button() == Qt.MouseButton.RightButton else 4, True)
        point = self.point(event.position())
        if event.button() == Qt.MouseButton.LeftButton and point:
            self.touching = True
            self.pointer.emit("D", *point)
            event.accept()

    def mouseMoveEvent(self, event):
        self.motion.emit(event.position())
        point = self.point(event.position())
        if self.touching and point:
            self.pointer.emit("M", *point)

    def mouseReleaseEvent(self, event):
        if event.button() in (Qt.MouseButton.RightButton, Qt.MouseButton.MiddleButton):
            self.mouse_button.emit(2 if event.button() == Qt.MouseButton.RightButton else 4, False)
        if event.button() == Qt.MouseButton.LeftButton and self.touching:
            self.touching = False
            self.pointer.emit("U", 0, 0)

    def keyPressEvent(self, event):
        if event.isAutoRepeat() and event.key() in (
            Qt.Key.Key_F8,
            Qt.Key.Key_F10,
            Qt.Key.Key_F11,
            Qt.Key.Key_Escape,
        ):
            event.accept()
            return
        self.keyboard.emit(
            key_name(event), True, event.key(), event.nativeScanCode(), event.modifiers()
        )
        event.accept()

    def keyReleaseEvent(self, event):
        if not event.isAutoRepeat():
            self.keyboard.emit(
                key_name(event), False, event.key(), event.nativeScanCode(), event.modifiers()
            )
        event.accept()

    def focusOutEvent(self, _event):
        self.touching = False
        self.released.emit()

    def inputMethodEvent(self, event):
        if event.commitString():
            self.text_input.emit(event.commitString())
        event.accept()

    def inputMethodQuery(self, query):
        if query == Qt.InputMethodQuery.ImEnabled:
            return True
        if query == Qt.InputMethodQuery.ImCursorRectangle:
            return QRect(self.width() // 2, self.height() // 2, 1, 20)
        if query == Qt.InputMethodQuery.ImSurroundingText:
            return ""
        if query == Qt.InputMethodQuery.ImCursorPosition:
            return 0
        return super().inputMethodQuery(query)


def key_name(event):
    names = {
        Qt.Key.Key_Backspace: "Backspace",
        Qt.Key.Key_Return: "Enter",
        Qt.Key.Key_Enter: "Enter",
        Qt.Key.Key_Tab: "Tab",
        Qt.Key.Key_Escape: "Escape",
        Qt.Key.Key_Delete: "Delete",
        Qt.Key.Key_Left: "ArrowLeft",
        Qt.Key.Key_Right: "ArrowRight",
        Qt.Key.Key_Up: "ArrowUp",
        Qt.Key.Key_Down: "ArrowDown",
        Qt.Key.Key_Home: "Home",
        Qt.Key.Key_End: "End",
        Qt.Key.Key_Shift: "Shift",
        Qt.Key.Key_Control: "Control",
        Qt.Key.Key_Alt: "Alt",
        Qt.Key.Key_Meta: "Meta",
    }
    if 65 <= event.key() <= 90 and event.modifiers() & (
        Qt.KeyboardModifier.ControlModifier | Qt.KeyboardModifier.AltModifier
    ):
        return chr(event.key()).lower()
    return names.get(event.key(), event.text() if event.text().isprintable() else "")


class PhoneView(QWidget):
    action = Signal(str)
    failure = Signal(str)
    frame_ready = Signal()
    observed = Signal(int, int, int, str)
    hidden = Signal()
    clipboard_received = Signal(str, int)
    focus_changed = Signal()
    pad_ready = Signal()
    files_dropped = Signal(object)

    def __init__(self, phone, translate=None):
        super().__init__()
        self.setAcceptDrops(True)
        self.phone = phone
        self.settings = phone.paths.settings(phone.identity)
        self.tr = translate or Translator(self.settings.language)
        self._dock_labels = []
        self.setWindowTitle("Velox — " + phone.metadata["name"])
        self.setWindowIcon(brand_icon())
        self.jobs = Jobs(self)
        self.connection = self.video = self.input = self.keys = None
        self._generation = 0
        self._orientation = False
        self._followed = None
        self.current_app = "-"
        self._profile_key = None
        self.control_editor = None
        self.mapper = Mapper(self, self.mapped_touch)
        self.pad = PadWorker(phone, self.pad_ready.emit, self.failure.emit)
        self.pad_ready.connect(self.present_pad)
        self._raw_names = {}
        self._clipboard_generation = 0
        QApplication.clipboard().dataChanged.connect(self.clipboard_changed)
        self.clipboard_received.connect(self.copy_received)
        self._animation = None
        self._manual_fullscreen = False
        self.resize_timer = QTimer(self)
        self.resize_timer.setSingleShot(True)
        self.resize_timer.setInterval(350)
        self.resize_timer.timeout.connect(self.resize_stream)
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose, False)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(10, 10, 6, 10)
        layout.setSpacing(8)
        self.canvas = Canvas(self)
        self.mouse_aim = MouseAim(self)
        self.notice = QLabel(self.canvas)
        self.notice.setTextFormat(Qt.TextFormat.PlainText)
        self.notice.setWordWrap(True)
        self.notice.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self.notice.setStyleSheet(
            "background: #272b3d; color: #f3f3ff; border: 1px solid #625297; border-radius: 10px; padding: 12px;"
        )
        self.notice.hide()
        self.notice_timer = QTimer(self)
        self.notice_timer.setSingleShot(True)
        self.notice_timer.timeout.connect(self.notice.hide)
        self.failure.connect(self.show_notice)
        layout.addWidget(self.canvas, 1)
        dock = QFrame()
        dock.setObjectName("card")
        dock.setFixedWidth(50)
        column = QVBoxLayout(dock)
        column.setContentsMargins(4, 4, 4, 4)
        column.setSpacing(4)

        def button(name, label, action):
            widget = QToolButton()
            widget.setIcon(icon(name))
            widget.setIconSize(QSize(21, 21))
            widget.setToolTip(self.tr(label))
            widget.setAccessibleName(self.tr(label))
            widget.setFixedSize(42, 34)
            widget.setStyleSheet("padding: 6px;")
            widget.setFocusPolicy(Qt.FocusPolicy.TabFocus)
            widget.clicked.connect(action)
            column.addWidget(widget)
            self._dock_labels.append((widget, label))
            return widget

        close_button = button("close", "Close phone view (Android keeps running)", self.hide)
        brand = QLabel()
        brand.setPixmap(brand_icon().pixmap(32, 32))
        brand.setAlignment(Qt.AlignmentFlag.AlignCenter)
        column.addWidget(brand)
        for name, label, action in (
            ("back", "Back", lambda: self.android_key("GoBack")),
            ("home", "Home", lambda: self.android_key("GoHome")),
            ("recent", "Recent apps", lambda: self.android_key("AppSwitch")),
            ("volume-up", "Volume up", lambda: self.android_key("AudioVolumeUp")),
            ("volume-down", "Volume down", lambda: self.android_key("AudioVolumeDown")),
            ("rotate", "Rotate", self.rotate),
            ("camera", "Screenshot", lambda: self.action.emit("screenshot")),
            ("record", "Record", lambda: self.action.emit("record")),
            ("fullscreen", "Full screen", self.fullscreen),
            ("library", "Library", lambda: self.action.emit("library")),
            ("keyboard", "Controls", lambda: self.action.emit("controls")),
            ("display", "Screen resolution", lambda: self.action.emit("display")),
            ("performance", "System monitor", lambda: self.action.emit("system")),
            ("settings", "Velox Settings", lambda: self.action.emit("settings")),
            ("power", "Power", lambda: self.action.emit("power")),
        ):
            control = button(name, label, action)
            if name == "record":
                self.record_button = control
        column.addStretch()
        area = QScrollArea()
        area.setWidgetResizable(True)
        area.setFrameShape(QFrame.Shape.NoFrame)
        area.setWidget(dock)
        area.setFixedWidth(58)
        area.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        column.removeWidget(close_button)
        column.removeWidget(brand)
        rail = QFrame()
        rail.setObjectName("card")
        rail.setFixedWidth(58)
        rail_layout = QVBoxLayout(rail)
        rail_layout.setContentsMargins(0, 4, 0, 0)
        rail_layout.setSpacing(6)
        rail_layout.addWidget(close_button, 0, Qt.AlignmentFlag.AlignHCenter)
        rail_layout.addWidget(brand)
        rail_layout.addWidget(area, 1)
        layout.addWidget(rail)
        self.boot = BootScreen(self.canvas, self.settings.boot_style)
        self.boot.hide()
        self.canvas.pointer.connect(self.pointer)
        self.canvas.keyboard.connect(self.keyboard)
        self.canvas.mouse_button.connect(self.mouse_button)
        self.canvas.motion.connect(self.mouse_aim.moved)
        self.canvas.text_input.connect(self.input_text)
        self.canvas.released.connect(self.release)
        self.canvas.resized.connect(self.resize_timer.start)
        self.frame_ready.connect(self.present)
        self.observed.connect(self.observe)
        QApplication.instance().focusWindowChanged.connect(self.native_focus_changed)
        self.fit()

    def retranslate(self, language):
        if isinstance(self.tr, Translator):
            self.tr.language = language
        for widget, text in self._dock_labels:
            widget.setToolTip(self.tr(text))
            widget.setAccessibleName(self.tr(text))

    def focused(self):
        window = self.windowHandle()
        return bool(
            self.isVisible()
            and not self.isMinimized()
            and window
            and QApplication.focusWindow() is window
            and window.isActive()
        )

    def native_focus_changed(self, _window):
        self.update_video_rate()
        if not self.focused():
            self.release()
        else:
            self.update_pad()
        self.focus_changed.emit()

    def fit(self, landscape=False, animate=False):
        screen = self.screen()
        if not screen or self.isFullScreen() or self.isMaximized():
            return
        display = self.settings.display
        if self.phone.alive():
            from .core import Settings

            display = Settings.from_dict(self.phone.runtime()["settings"]).display
        _, width, height, _ = DISPLAYS[display]
        if landscape != (width > height):
            width, height = height, width
        work = screen.availableGeometry()
        width, height = phone_size(
            work.width(),
            work.height(),
            width,
            height,
            self.settings.phone_percent,
            screen.devicePixelRatio(),
        )
        target = QRect(self.x(), self.y(), width + 82, height + 20)
        target.moveLeft(max(work.left(), min(target.left(), work.right() - target.width() + 1)))
        target.moveTop(max(work.top(), min(target.top(), work.bottom() - target.height() + 1)))
        if self._animation:
            self._animation.stop()
        if animate and self.isVisible():
            self._animation = QPropertyAnimation(self, b"geometry", self)
            self._animation.setStartValue(self.geometry())
            self._animation.setEndValue(target)
            self._animation.setDuration(180)
            self._animation.setEasingCurve(QEasingCurve.Type.OutCubic)
            self._animation.start()
        else:
            self.setGeometry(target)

    def booting(self):
        self.boot.started = time.monotonic()
        self.boot.progress = 0
        self.boot.setGeometry(self.canvas.rect())
        self.boot.show()
        self.boot.raise_()

    def progress(self, message, fraction):
        self.boot.message = self.tr(message)
        self.boot.progress = fraction
        self.boot.update()

    def connect_live(self):
        if self.connection or "connect" in self.jobs.active:
            return
        if any(key.startswith("disconnect-") for key in self.jobs.active):
            QTimer.singleShot(100, self.connect_live)
            return
        generation = self._generation

        def attached(connection):
            if generation != self._generation or not self.isVisible() or self.isMinimized():
                connection.close()
                return
            self.connection = connection
            self.start_video()
            self.input = InputPipe(self.phone, self.failure.emit, self.observed.emit)
            self.input.start()
            self.keys = KeyPipe(connection, self.input, self.failure.emit)
            self.canvas.setFocus()
            self.update_pad()

        self.jobs.submit("connect", lambda: Connection(self.phone), attached, self.failure.emit)

    def start_video(self):
        from .core import Settings

        actual = Settings.from_dict(self.phone.runtime()["settings"])
        _, native_width, native_height, _density = DISPLAYS[actual.display]
        if (native_width > native_height) != self._orientation:
            native_width, native_height = native_height, native_width
        self.video = Video(
            self.connection,
            self.frame_ready.emit,
            self.failure.emit,
            max(64, min(native_width, 3840, round(self.canvas.width() * self.devicePixelRatioF()))),
            max(
                64, min(native_height, 3840, round(self.canvas.height() * self.devicePixelRatioF()))
            ),
        )
        self.update_video_rate(actual.rate)
        self.video.start()

    def update_video_rate(self, actual_rate=None):
        if not self.video:
            return
        if actual_rate is None:
            from .core import Settings

            try:
                actual_rate = Settings.from_dict(self.phone.runtime()["settings"]).rate
            except VeloxError:
                return
        screen = self.screen()
        monitor_rate = max(30, round(screen.refreshRate())) if screen else 60
        record = self.phone.runtime()
        normal = effective_rate(record, min(self.settings.rate, actual_rate, monitor_rate))
        self.video.rate = (
            normal if self.focused() else min(normal, self.settings.background_rate or 30, 30)
        )

    def resize_stream(self):
        if not self.video or not self.connection or self.isMinimized() or not self.isVisible():
            return
        width = max(320, min(3840, round(self.canvas.width() * self.devicePixelRatioF())))
        height = max(320, min(3840, round(self.canvas.height() * self.devicePixelRatioF())))
        if abs(width / self.video.width - 1) < 0.15 and abs(height / self.video.height - 1) < 0.15:
            return
        old, generation = self.video, self._generation
        self.video = None

        def replaced(_value):
            if (
                self.connection
                and generation == self._generation
                and self.isVisible()
                and not self.isMinimized()
            ):
                self.start_video()

        self.jobs.submit("resize-video", old.close, replaced, self.failure.emit)

    def present(self):
        if not self.video:
            return
        frame = self.video.consume()
        if frame:
            self.canvas.present(frame)
            landscape = frame.width > frame.height
            if landscape != self._orientation:
                self.release()
                self._orientation = landscape
                self.fit(landscape, animate=True)
                self.reload_controls()
            if self.boot.isVisible() and self.phone.state == "running":
                remaining = max(0, 0.65 - (time.monotonic() - self.boot.started))
                QTimer.singleShot(round(remaining * 1000), self.boot.hide)

    def observe(self, width, height, rotation, app):
        changed = self.current_app != app
        self.current_app = app
        if self.control_editor and (
            app != self.control_editor.identity or (width > height) != self.control_editor.landscape
        ):
            self.control_editor.finish(False)
        if changed:
            self.reload_controls()
        if self.settings.auto_rotate and rotation != self._followed and self.connection:
            connection = self.connection

            def applied(_value):
                self._followed = rotation

            self.jobs.submit(
                "rotation", lambda: connection.rotate(rotation), applied, self.failure.emit
            )

    def pointer(self, operation, x, y):
        if operation in ("D", "U") and self.mapper.key_event(1, operation == "D"):
            return
        if operation == "M" and 1 in self.mapper.held:
            return
        if self.input:
            self.input.send(operation, 14, x, y)

    def keyboard(self, name, pressed, key, scan=0, modifiers=Qt.KeyboardModifier.NoModifier):
        if key == Qt.Key.Key_Escape and self.mouse_aim.mode:
            if pressed:
                self.mouse_aim.end()
            return
        if key == Qt.Key.Key_F8:
            if pressed:
                self.mouse_aim.toggle()
            return
        if key == Qt.Key.Key_F11 and pressed:
            self.fullscreen()
            return
        if key == Qt.Key.Key_F10:
            if pressed and PACKAGE.fullmatch(self.current_app):
                try:
                    self.mouse_aim.end()
                    self.mapper.profile.enabled = not self.mapper.profile.enabled
                    self.mapper.release()
                    Controls(self.phone).save(
                        self.current_app, self._orientation, self.mapper.profile
                    )
                except (VeloxError, OSError) as error:
                    self.failure.emit(str(error))
                    self.reload_controls()
            return
        if self.mapper.key_event(
            virtual_key(key, scan, bool(modifiers & Qt.KeyboardModifier.KeypadModifier)), pressed
        ):
            return
        if self.settings.clipboard_shortcuts and modifiers & Qt.KeyboardModifier.ControlModifier:
            if key == Qt.Key.Key_V:
                if pressed and self.keys:
                    try:
                        self.keys.text(QApplication.clipboard().text(), preserve=False)
                    except VeloxError as error:
                        self.failure.emit(str(error))
                return
        if name and not name.isascii():
            if pressed:
                self.input_text(name)
            return
        identity = key, scan
        if self.keys:
            if pressed:
                self._raw_names[identity] = name
            else:
                name = self._raw_names.pop(identity, name)
            self.keys.send(name, pressed)
            if (
                self.settings.clipboard_shortcuts
                and pressed
                and key == Qt.Key.Key_C
                and modifiers & Qt.KeyboardModifier.ControlModifier
            ):
                generation = self._clipboard_generation
                try:
                    self.keys.copy(lambda text: self.clipboard_received.emit(text, generation))
                except VeloxError as error:
                    self.failure.emit(str(error))

    def input_text(self, text):
        if self.keys:
            try:
                self.keys.text(text)
            except VeloxError as error:
                self.failure.emit(str(error))

    def clipboard_changed(self):
        self._clipboard_generation += 1

    def copy_received(self, text, generation):
        if text is not None and self.focused() and generation == self._clipboard_generation:
            QApplication.clipboard().setText(text)

    def android_key(self, name):
        if self.keys:
            self.keys.send(name, True)
            self.keys.send(name, False)

    def release(self):
        self.mouse_aim.end()
        self._raw_names.clear()
        self.mapper.release()
        if self.input:
            self.input.send("C")
        if self.keys:
            self.keys.release()
        self.update_pad()

    def update_pad(self, active=None):
        if active is None:
            active = (
                self.isVisible()
                and not self.isMinimized()
                and self.focused()
                and self.input is not None
                and self.control_editor is None
            )
        self.pad.configure(self.mapper.profile, active)

    def present_pad(self):
        frame = self.pad.consume()
        if frame is None:
            return
        if (
            not self.focused()
            or not self.isVisible()
            or self.isMinimized()
            or self.control_editor
            or not self.input
        ):
            frame = PadFrame()
        self.mapper.pad_frame(frame)

    def mapped_touch(self, operation, contact, x, y):
        if self.input:
            self.input.send(operation, contact, round(x * 10000), round(y * 10000))

    def mouse_button(self, key, pressed):
        if key == 2 and not any(
            item.key == 2 and item.kind in (0, 2) for item in self.mapper.profile.bindings
        ):
            if pressed and self.mouse_aim.start_drag():
                return
            if not pressed and self.mouse_aim.mode == "drag":
                self.mouse_aim.end()
                return
        self.mapper.key_event(key, pressed)

    def reload_controls(self):
        from .core import VeloxError, PACKAGE

        self.mouse_aim.end()
        if not PACKAGE.fullmatch(self.current_app):
            self.mapper.configure(Profile(), 1, 1)
            self._profile_key = None
            self.update_pad()
            return
        try:
            profile = Controls(self.phone).load(self.current_app, self._orientation)
        except (VeloxError, OSError, TypeError) as error:
            self.failure.emit(str(error))
            profile = Profile()
        frame = self.canvas.frame
        self.mapper.configure(profile, frame.width if frame else 1, frame.height if frame else 1)
        self._profile_key = self.current_app, self._orientation
        self.update_pad()

    def edit_controls(self, identity=None):
        from .core import PACKAGE

        identity = identity or self.current_app
        if not PACKAGE.fullmatch(identity) or not self.canvas.frame:
            self.failure.emit("Open a game before placing its controls.")
            return
        if self.control_editor:
            self.control_editor.raise_()
            return
        from .controlview import ControlOverlay

        self.release()
        try:
            self.control_editor = ControlOverlay(self, identity, self._orientation, self.tr)
        except (VeloxError, OSError) as error:
            self.failure.emit(str(error))
            return
        self.update_pad(False)

        def closed(_saved):
            self.control_editor = None
            self.reload_controls()
            self.canvas.setFocus()

        self.control_editor.closed.connect(closed)

    def rotate(self):
        if self.connection:
            target = ((self._followed or 0) + 1) % 4
            connection = self.connection
            self._followed = target  # Do not immediately undo a manual rotation.
            self.jobs.submit(
                "rotation", lambda: connection.rotate(target), failed=self.failure.emit
            )

    def fullscreen(self):
        self.mouse_aim.end()
        self.showNormal() if self.isFullScreen() else self.showFullScreen()
        self._manual_fullscreen = self.isFullScreen()

    def session_fullscreen(self, value):
        desired = self._manual_fullscreen if value < 0 else bool(value)
        if desired != self.isFullScreen():
            self.mouse_aim.end()
            self.showFullScreen() if desired else self.showNormal()

    def showEvent(self, _event):
        if self.phone.state == "running":
            self.connect_live()

    def hideEvent(self, _event):
        self.mouse_aim.destroy()
        self.notice.hide()
        if self.control_editor:
            self.control_editor.finish(True)
        self.mapper.release()
        self.update_pad(False)
        self._generation += 1
        video, pipe, keys, connection = self.video, self.input, self.keys, self.connection
        self.video = self.input = self.keys = self.connection = None
        self._followed = None

        def disconnect():
            if video:
                video.close()
            if keys:
                keys.close()
            if pipe:
                pipe.close()
            if connection:
                connection.close()

        self.jobs.submit("disconnect-" + str(self._generation), disconnect)
        self.hidden.emit()
        self.focus_changed.emit()

    def closeEvent(self, event):
        self.hide()
        event.ignore()

    def dragEnterEvent(self, event):
        from .drops import local_files

        if local_files(event.mimeData()):
            event.acceptProposedAction()

    def dropEvent(self, event):
        from .drops import local_files

        files = local_files(event.mimeData())
        if files:
            event.acceptProposedAction()
            self.files_dropped.emit(files)

    def changeEvent(self, event):
        if event.type() == QEvent.Type.ActivationChange:
            self.update_video_rate()
            self.focus_changed.emit()
            if not self.focused():
                self.release()
            else:
                self.update_pad()
        if event.type() == QEvent.Type.WindowStateChange:
            if self.isMinimized():
                self.hideEvent(event)
            elif self.isVisible():
                self.connect_live()
        super().changeEvent(event)

    def show_notice(self, message):
        width = max(120, min(520, self.canvas.width() - 24))
        self.notice.setText(self.tr(message))
        self.notice.setFixedWidth(width)
        self.notice.adjustSize()
        self.notice.move(12, 12)
        self.notice.show()
        self.notice.raise_()
        self.notice_timer.start(4500)

    def event(self, event):
        if (
            isinstance(event, QPlatformSurfaceEvent)
            and hasattr(self, "mouse_aim")
            and event.surfaceEventType()
            == QPlatformSurfaceEvent.SurfaceEventType.SurfaceAboutToBeDestroyed
        ):
            self.mouse_aim.destroy()
        return super().event(event)
