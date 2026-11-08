from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QFrame,
    QHBoxLayout,
    QLabel,
    QPlainTextEdit,
    QPushButton,
    QScrollArea,
    QVBoxLayout,
    QWidget,
)


def label(text, role="", wrap=False):
    result = QLabel(text)
    result.setTextFormat(Qt.TextFormat.PlainText)
    if role:
        result.setObjectName(role)
    result.setWordWrap(wrap)
    return result


def button(text, action, primary=False):
    result = QPushButton(text)
    if primary:
        result.setObjectName("primary")
    result.clicked.connect(lambda _checked=False: action())
    return result


def card(parent_layout, title="", subtitle=""):
    panel = QFrame()
    panel.setObjectName("card")
    box = QVBoxLayout(panel)
    box.setContentsMargins(18, 16, 18, 16)
    box.setSpacing(12)
    if title:
        title_label = label(title, wrap=True)
        title_label.setStyleSheet("font-weight: 600; background: transparent;")
        box.addWidget(title_label)
    if subtitle:
        box.addWidget(label(subtitle, "subtitle", True))
    parent_layout.addWidget(panel)
    return box


def row(layout, *widgets):
    result = QHBoxLayout()
    result.setSpacing(10)
    for widget in widgets:
        result.addWidget(widget) if isinstance(widget, QWidget) else result.addStretch()
    layout.addLayout(result)
    return result


def combo(items, selected, changed):
    result = QComboBox()
    for text, value in items:
        result.addItem(text, value)
    result.setCurrentIndex(max(0, result.findData(selected)))
    result.currentIndexChanged.connect(lambda index: changed(result.itemData(index)))
    return result


def toggle(layout, title, subtitle, selected, changed):
    box = card(layout)
    check = QCheckBox(title)
    check.setChecked(selected)
    check.setAccessibleDescription(subtitle)
    check.toggled.connect(changed)
    box.addWidget(check)
    if subtitle:
        box.addWidget(label(subtitle, "subtitle", True))
    return check


def scroll_page(title, subtitle):
    page = QWidget()
    layout = QVBoxLayout(page)
    layout.setContentsMargins(28, 24, 28, 20)
    layout.setSpacing(16)
    layout.addWidget(label(title, "title"))
    layout.addWidget(label(subtitle, "subtitle", True))
    scroll = QScrollArea()
    scroll.setWidgetResizable(True)
    body = QWidget()
    contents = QVBoxLayout(body)
    contents.setContentsMargins(0, 0, 0, 0)
    contents.setSpacing(14)
    scroll.setWidget(body)
    layout.addWidget(scroll, 1)
    return page, contents


def text_dialog(parent, title, content, translate=lambda value: value):
    dialog = QDialog(parent)
    dialog.setWindowTitle(title)
    dialog.resize(720, 500)
    layout = QVBoxLayout(dialog)
    text = QPlainTextEdit()
    text.setReadOnly(True)
    text.setPlainText(content)
    layout.addWidget(text)
    controls = QDialogButtonBox()
    controls.addButton(translate("Close"), QDialogButtonBox.ButtonRole.RejectRole)
    controls.rejected.connect(dialog.reject)
    layout.addWidget(controls)
    return dialog
