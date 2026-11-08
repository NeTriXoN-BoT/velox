from __future__ import annotations

from PySide6.QtCore import QByteArray, QRectF, Qt
from PySide6.QtGui import QIcon, QPainter, QPixmap
from PySide6.QtSvg import QSvgRenderer

from .wallpaper import logo


INK, MUTED, FAINT = "#f4f5fb", "#a3acc1", "#697388"
PURPLE, BLUE = "#9266ff", "#397cff"
PATHS = {
    "close": '<path d="m7 7 10 10M17 7 7 17"/>',
    "back": '<path d="m15 5-7 7 7 7"/>',
    "home": '<path d="m3 11 9-8 9 8M6 9v12h5v-7h3v7h4V9"/>',
    "recent": '<rect x="5" y="5" width="14" height="14" rx="2"/>',
    "power": '<path d="M12 2v9M6.4 5.8a9 9 0 1 0 11.2 0"/>',
    "volume-up": '<path d="m3 9 4 0 5-4v14l-5-4H3ZM16 8a6 6 0 0 1 0 8M19 5a10 10 0 0 1 0 14"/>',
    "volume-down": '<path d="m3 9 4 0 5-4v14l-5-4H3ZM16 8a6 6 0 0 1 0 8"/>',
    "rotate": '<path d="M4 9a8 8 0 1 1 1 9M4 3v6h6"/><rect x="10" y="8" width="5" height="9" rx="1"/>',
    "fullscreen": '<path d="M8 3H3v5m13-5h5v5M3 16v5h5m8 0h5v-5"/>',
    "camera": '<path d="M3 7h5l2-3h4l2 3h5v14H3Z"/><circle cx="12" cy="13" r="4"/>',
    "record": '<circle cx="12" cy="12" r="8"/><circle cx="12" cy="12" r="3"/>',
    "display": '<rect x="2" y="3" width="20" height="14" rx="2"/><path d="M8 22h8m-4-5v5"/>',
    "settings": '<path d="m9 3 6 0 1 3 3 1 2 5-2 5-3 1-1 3H9l-1-3-3-1-2-5 2-5 3-1Z"/><circle cx="12" cy="12" r="3"/>',
    "keyboard": '<rect x="2" y="5" width="20" height="14" rx="2"/><path d="M5 9h2m2 0h2m2 0h2m2 0h2M5 12h2m2 0h2m2 0h2m2 0h2M7 16h10"/>',
    "library": '<rect x="3" y="4" width="4" height="16" rx="1"/><rect x="10" y="4" width="4" height="16" rx="1"/><path d="m17 4 4-1 3 16-4 1Z"/>',
    "phone": '<rect x="6" y="2" width="12" height="20" rx="3"/><path d="M10 18h4"/>',
    "instances": '<rect x="8" y="3" width="12" height="16" rx="2"/><path d="M16 22H4V6"/>',
    "performance": '<path d="M3 18a9 9 0 1 1 18 0M12 14l5-7M3 18h18"/>',
    "look": '<path d="M12 3a9 9 0 1 0 0 18c5 0 2-5 5-6 7-2 3-12-5-12Z"/><circle cx="7" cy="9" r=".7"/><circle cx="12" cy="6" r=".7"/><circle cx="17" cy="8" r=".7"/>',
    "recovery": '<path d="M4 8a9 9 0 1 1-1 8M4 3v5h5m3 0v5l3 2"/>',
    "updates": '<path d="M12 3v13m-5-5 5 5 5-5M3 17v5h18v-5"/>',
    "search": '<circle cx="10" cy="10" r="6"/><path d="m15 15 6 6"/>',
    "add": '<path d="M12 4v16M4 12h16"/>',
    "play": '<path d="m7 3 14 9L7 21Z"/>',
    "folder": '<path d="M2 5h8l2 3h10v13H2Z"/>',
    "more": '<circle cx="5" cy="12" r="1"/><circle cx="12" cy="12" r="1"/><circle cx="19" cy="12" r="1"/>',
    "star": '<path d="m12 2 3 6 7 1-5 5 1 7-6-3-6 3 1-7-5-5 7-1Z"/>',
    "clipboard": '<rect x="5" y="5" width="14" height="17" rx="2"/><rect x="9" y="2" width="6" height="5" rx="1"/>',
}


def icon(name: str, color=MUTED) -> QIcon:
    markup = (
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="{color}" '
        'stroke-width="1.65" stroke-linecap="round" stroke-linejoin="round">'
        + PATHS[name]
        + "</svg>"
    )
    renderer = QSvgRenderer(QByteArray(markup.encode()))
    result = QIcon()
    for ratio in (1, 2, 3):
        image = QPixmap(24 * ratio, 24 * ratio)
        image.fill(Qt.GlobalColor.transparent)
        painter = QPainter(image)
        renderer.render(painter)
        painter.end()
        image.setDevicePixelRatio(ratio)
        result.addPixmap(image)
    return result


def brand_icon() -> QIcon:
    result = QIcon()
    for size in (32, 64, 128, 256):
        image = QPixmap(size, size)
        image.fill(Qt.GlobalColor.transparent)
        painter = QPainter(image)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        logo(painter, QRectF(1, 1, size - 2, size - 2))
        painter.end()
        result.addPixmap(image)
    return result


def stylesheet(factor=1.0):
    def n(value):
        return max(1, round(value * factor))

    return f"""
    QWidget {{ background: #0e1016; color: {INK}; font-size: {n(13)}px; }}
    QWidget#clear {{ background: transparent; }}
    QMainWindow, QDialog {{ background: #0e1016; }}
    QFrame#sidebar {{ background: #12141e; border-right: 1px solid #282c3b; }}
    QFrame#card {{ background: #1a1d27; border: 1px solid #262b3a; border-radius: {n(15)}px; }}
    QFrame#card QLabel, QFrame#card QCheckBox, QFrame#card QWidget#clear {{ background: transparent; }}
    QLabel#title {{ font-size: {n(25)}px; font-weight: 700; background: transparent; }}
    QLabel#subtitle {{ color: {MUTED}; background: transparent; }}
    QLabel#section {{ color: {FAINT}; font-size: {n(11)}px; font-weight: 600; background: transparent; }}
    QLabel#brand {{ font-size: {n(18)}px; font-weight: 700; background: transparent; }}
    QPushButton, QToolButton {{ background: #252a38; border: 1px solid transparent; border-radius: {n(10)}px;
        padding: {n(9)}px {n(15)}px; color: {INK}; }}
    QPushButton:hover, QToolButton:hover {{ background: #30364b; border-color: #524879; }}
    QPushButton:pressed, QToolButton:pressed {{ background: #3d3d5d; }}
    QPushButton:disabled, QToolButton:disabled {{ color: #626c80; background: #1a1e29; }}
    QPushButton#primary, QPushButton:checked {{ background: qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 {PURPLE},stop:1 {BLUE}); color: white; font-weight: 600; }}
    QPushButton#nav {{ text-align: left; background: transparent; color: {MUTED}; padding: {n(11)}px {n(15)}px; }}
    QPushButton#nav:checked {{ background: #282c3b; color: white; border-left: {n(3)}px solid {PURPLE}; }}
    QPushButton#nav:hover {{ background: #202433; color: white; }}
    QLineEdit, QTextEdit, QPlainTextEdit, QSpinBox, QDoubleSpinBox, QComboBox {{ background: #131824; color: {INK};
        border: 1px solid #343b50; border-radius: {n(9)}px; padding: {n(9)}px; selection-background-color: #7356d5; }}
    QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus, QComboBox:focus {{ border-color: {PURPLE}; }}
    QComboBox QAbstractItemView {{ background: #202536; color: {INK}; selection-background-color: #58498f; padding: {n(6)}px; }}
    QScrollArea {{ border: none; }}
    QScrollBar:vertical {{ background: transparent; width: {n(8)}px; }}
    QScrollBar::handle:vertical {{ background: #3b4258; border-radius: {n(4)}px; min-height: {n(30)}px; }}
    QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {{ height: 0px; }}
    QCheckBox {{ spacing: {n(12)}px; background: transparent; }}
    QCheckBox::indicator {{ width: {n(32)}px; height: {n(18)}px; border: 1px solid #4a526a; border-radius: {n(9)}px; background: #343b4c; }}
    QCheckBox::indicator:checked {{ background: {PURPLE}; border-color: {PURPLE}; image: none; }}
    QCheckBox::indicator:checked:hover {{ background: {BLUE}; }}
    QToolTip {{ background: #272b3c; color: white; border: 1px solid #464e68; padding: {n(7)}px; }}
    QProgressBar {{ border: none; background: #303448; border-radius: {n(5)}px; text-align: center; }}
    QProgressBar::chunk {{ background: {PURPLE}; border-radius: {n(5)}px; }}
    QMenu {{ background: #1c2030; border: 1px solid #343b50; padding: {n(6)}px; }}
    QMenu::item {{ padding: {n(8)}px {n(20)}px; border-radius: {n(6)}px; }}
    QMenu::item:selected {{ background: #343451; }}
    QGroupBox {{ border: 1px solid #343b50; border-radius: {n(10)}px; margin-top: {n(20)}px; padding: {n(14)}px; }}
    QGroupBox::title {{ subcontrol-origin: margin; left: {n(12)}px; color: {MUTED}; }}
    """
