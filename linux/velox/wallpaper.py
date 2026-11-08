from __future__ import annotations

import contextlib
import math
import os
import shlex
import uuid
from pathlib import Path

from PySide6.QtCore import QBuffer, QIODevice, QRectF, Qt
from PySide6.QtGui import (
    QColor,
    QFont,
    QImage,
    QImageReader,
    QLinearGradient,
    QPainter,
    QPainterPath,
    QPen,
)

from .core import (
    DISPLAYS,
    Settings,
    VeloxError,
    atomic_json,
    bounded_read,
    file_hash,
    lease,
    private_directory,
    read_json,
)
from .experience import Experience
from .setup import resource_root


BUILTINS = ("Aurora", "Velocity", "Midnight", "Prism", "Circuit", "Horizon")


def logo(painter: QPainter, rectangle: QRectF) -> None:
    gradient = QLinearGradient(rectangle.topLeft(), rectangle.bottomRight())
    gradient.setColorAt(0, QColor("#241653"))
    gradient.setColorAt(0.65, QColor("#171332"))
    gradient.setColorAt(1, QColor("#080d24"))
    painter.setBrush(gradient)
    border = QLinearGradient(rectangle.topLeft(), rectangle.bottomRight())
    border.setColorAt(0, QColor("#b5a1ff"))
    border.setColorAt(1, QColor("#397aff"))
    painter.setPen(QPen(border, max(1, rectangle.width() / 25)))
    painter.drawRoundedRect(rectangle, rectangle.width() * 0.23, rectangle.width() * 0.23)
    painter.save()
    painter.translate(rectangle.left(), rectangle.top())
    painter.scale(rectangle.width() / 100, rectangle.height() / 100)
    path = QPainterPath()
    path.moveTo(22, 24)
    path.lineTo(42, 24)
    path.lineTo(55, 62)
    path.lineTo(72, 24)
    path.lineTo(90, 24)
    path.lineTo(63, 82)
    path.lineTo(45, 82)
    path.closeSubpath()
    light = QLinearGradient(0, 20, 0, 84)
    light.setColorAt(0, QColor("#ffffff"))
    light.setColorAt(1, QColor("#9576f6"))
    painter.fillPath(path, light)
    painter.setPen(QPen(QColor("#7778df"), 4))
    painter.drawLine(14, 42, 29, 73)
    painter.drawLine(13, 59, 20, 74)
    painter.restore()


def branding(painter: QPainter, width: int, height: int, brand: int) -> None:
    if brand == 0:
        return
    size = width * (0.13 if brand == 1 else 0.15)
    y = height * (0.76 if brand == 1 else 0.50 if brand == 2 else 0.57)
    x = width * 0.065
    logo(painter, QRectF(x, y, size, size))
    if brand == 1:
        return
    painter.setPen(QColor("#f5f2ff"))
    painter.setFont(QFont("Sans Serif", max(12, round(width / 18)), QFont.Weight.Bold))
    painter.drawText(
        QRectF(x + size * 1.25, y, width - x - size * 1.4, size * 0.72),
        Qt.AlignmentFlag.AlignVCenter,
        "VELOX",
    )
    painter.setFont(QFont("Sans Serif", max(7, round(width / 57)), QFont.Weight.DemiBold))
    painter.setPen(QColor("#bbb0e3"))
    painter.drawText(
        QRectF(x + size * 1.25, y + size * 0.66, width - x - size * 1.4, size * 0.25),
        Qt.AlignmentFlag.AlignVCenter,
        "ANDROID GAMING PHONE",
    )


def fitted(source: QImage, width: int, height: int, brand=0) -> QImage:
    scaled = source.scaled(
        width,
        height,
        Qt.AspectRatioMode.KeepAspectRatioByExpanding,
        Qt.TransformationMode.SmoothTransformation,
    )
    result = scaled.copy(
        (scaled.width() - width) // 2, (scaled.height() - height) // 2, width, height
    )
    if brand:
        painter = QPainter(result)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        branding(painter, width, height, brand)
        painter.end()
    return result


def builtin(index: int, width=1080, height=2400, brand=0) -> QImage:
    if index not in range(len(BUILTINS)):
        raise VeloxError("Unknown wallpaper.")
    image = QImage(width, height, QImage.Format.Format_ARGB32_Premultiplied)
    painter = QPainter(image)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)
    gradient = QLinearGradient(0, 0, width, height)
    palettes = (
        ("#1b1245", "#244f86", "#0c0b22"),
        ("#101c38", "#42247e", "#080b1c"),
        ("#080d1c", "#18183a", "#050812"),
        ("#262053", "#34468a", "#141328"),
        ("#07162c", "#173952", "#0a0c25"),
        ("#261947", "#7d476b", "#18132c"),
    )
    for position, color in zip((0.0, 0.5, 1.0), palettes[index]):
        gradient.setColorAt(position, QColor(color))
    painter.fillRect(image.rect(), gradient)
    for line in range(32):
        path = QPainterPath()
        for step in range(65):
            x = step * width / 64
            y = (
                height * (0.25 + line * 0.016)
                + math.sin(step / 13 + index + line / 9) * height * 0.06
            )
            path.moveTo(x, y) if step == 0 else path.lineTo(x, y)
        color = QColor("#a890ff" if index % 2 else "#64bded")
        color.setAlpha(18 + line % 7 * 3)
        painter.setPen(QPen(color, max(1.0, width / 900)))
        painter.drawPath(path)
    branding(painter, width, height, brand)
    painter.end()
    return image


def picture(path: Path) -> QImage:
    buffer = QBuffer()
    buffer.setData(bounded_read(path, 32 * 1024 * 1024))
    buffer.open(QIODevice.OpenModeFlag.ReadOnly)
    reader = QImageReader(buffer)
    reader.setAutoTransform(True)
    size = reader.size()
    if (
        not size.isValid()
        or size.width() > 16384
        or size.height() > 16384
        or size.width() * size.height() > 32_000_000
    ):
        raise VeloxError("Choose a PNG or JPG with at most 32 million pixels and 32 MB.")
    if bytes(reader.format()).lower() not in (b"png", b"jpg", b"jpeg"):
        raise VeloxError("Choose a PNG or JPG picture.")
    image = reader.read()
    if image.isNull():
        raise VeloxError("That picture couldn't be opened.")
    return image


class Wallpaper:
    def __init__(self, phone):
        self.phone = phone
        self.folder = private_directory(phone.folder / "wallpapers")

    def _lock(self):
        return lease(self.phone.paths.locks / (self.phone.identity + ".wallpaper"), blocking=True)

    def prepare_default(self, settings):
        if self.pending() or (self.phone.folder / "wallpaper-current.json").exists():
            return
        source = resource_root() / "wallpapers/Velox-chan - Windowlight.png"
        choice = (
            {"name": "Windowlight", "file": str(source)}
            if source.is_file()
            else {"name": "Aurora", "builtin": 0}
        )
        self.select(choice, settings, apply=False, only_if_empty=True)

    def gallery(self) -> list[dict]:
        result = [{"name": name, "builtin": index} for index, name in enumerate(BUILTINS)]
        for folder in (resource_root() / "wallpapers", self.folder):
            for file in sorted(folder.glob("*")):
                if (
                    file.suffix.lower() in (".png", ".jpg", ".jpeg")
                    and not file.is_symlink()
                    and not file.name.startswith("pending-")
                ):
                    caption = file.stem.removeprefix("Velox-chan - ")
                    if folder == self.folder:
                        parts = caption.split("-", 2)
                        if (
                            len(parts) == 3
                            and parts[0] == "custom"
                            and len(parts[1]) == 32
                            and all(c in "0123456789abcdef" for c in parts[1])
                        ):
                            caption = parts[2]
                    result.append({"name": caption, "file": str(file)})
        return result

    def add(self, files: list[Path]) -> int:
        count = 0
        for source in files[:32]:
            try:
                image = picture(source)
                name = (
                    "".join(
                        c if c.isprintable() and c not in '\\/:*?"<>|' else "_" for c in source.stem
                    ).strip(" .")
                    or "Wallpaper"
                )
                while len(name.encode("utf-8")) > 180:
                    name = name[:-1]
                target = self.folder / ("custom-" + uuid.uuid4().hex + "-" + name + ".png")
                if image.save(str(target), "PNG"):
                    os.chmod(target, 0o600)
                    count += 1
            except (VeloxError, OSError):
                continue
        return count

    def pending(self) -> dict:
        value = read_json(self.phone.folder / "wallpaper-pending.json", {})
        if not isinstance(value, dict):
            raise VeloxError("The wallpaper choice is damaged. Choose it again.")
        if value and (
            not isinstance(value.get("file"), str)
            or not re_name(value["file"])
            or not isinstance(value.get("sha256"), str)
            or len(value["sha256"]) != 64
        ):
            raise VeloxError("The wallpaper choice is damaged. Choose it again.")
        return value

    def select(self, choice: dict, settings: Settings, *, apply=True, only_if_empty=False) -> None:
        _, width, height, _ = DISPLAYS[settings.display]
        image = (
            builtin(choice["builtin"], width, height, settings.brand)
            if "builtin" in choice
            else fitted(picture(Path(choice["file"])), width, height, settings.brand)
        )
        name = "pending-" + uuid.uuid4().hex + ".png"
        target = self.folder / name
        if not image.save(str(target), "PNG"):
            raise VeloxError("The wallpaper choice couldn't be saved.")
        os.chmod(target, 0o600)
        digest = file_hash(target)
        committed = False
        try:
            with self._lock():
                previous = self.pending()
                if only_if_empty and (
                    previous or (self.phone.folder / "wallpaper-current.json").exists()
                ):
                    return
                atomic_json(
                    self.phone.folder / "wallpaper-pending.json",
                    {
                        "file": name,
                        "sha256": digest,
                        "name": str(choice.get("name", "Wallpaper"))[:120],
                    },
                )
                committed = True
                if previous:
                    (self.folder / previous["file"]).unlink(missing_ok=True)
        finally:
            if not committed:
                target.unlink(missing_ok=True)
        if apply and self.phone.alive() and self.phone.state == "running":
            self.apply_pending()

    def apply_pending(self) -> bool:
        with self._lock():
            pending = self.pending()
            if not pending:
                return False
            source = self.folder / pending["file"]
            if file_hash(source) != pending["sha256"]:
                raise VeloxError("The saved wallpaper changed or is missing. Choose it again.")
            Experience(self.phone).ensure_package("app.velox.helper", "helper.apk")
            remote = "/data/local/tmp/velox-wallpaper-" + uuid.uuid4().hex + ".png"
            try:
                self.phone.adb("push", str(source), remote, timeout=60)
                self.phone.shell(
                    "content write --uri content://app.velox.helper/wallpaper < "
                    + shlex.quote(remote),
                    timeout=30,
                )
                result = self.phone.shell(
                    "content call --uri content://app.velox.helper --method set", timeout=30
                )
                if "ok=true" not in result:
                    raise VeloxError("The phone didn't accept the wallpaper.")
                atomic_json(
                    self.phone.folder / "wallpaper-current.json",
                    {key: value for key, value in pending.items() if key != "file"},
                )
                (self.phone.folder / "wallpaper-pending.json").unlink(missing_ok=True)
                source.unlink(missing_ok=True)
                return True
            finally:
                with contextlib.suppress(VeloxError):
                    self.phone.shell("rm -f " + shlex.quote(remote), timeout=5)


def re_name(value: str) -> bool:
    import re

    return re.fullmatch(r"pending-[a-f0-9]{32}\.png", value) is not None
