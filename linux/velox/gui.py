from __future__ import annotations

import dataclasses
import os
import threading
import math
from pathlib import Path

from PySide6.QtCore import QLibraryInfo, QLocale, QSize, QTimer, QTranslator, Qt, QUrl
from PySide6.QtGui import QDesktopServices, QPixmap
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QDialog,
    QFileDialog,
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QInputDialog,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QProgressBar,
    QPushButton,
    QScrollArea,
    QVBoxLayout,
    QWidget,
    QDoubleSpinBox,
)

from .core import (
    CPU_CHOICES,
    DISPLAYS,
    PACKAGE,
    RAM_CHOICES,
    RATES,
    Settings,
    VeloxError,
    bounded_read,
    memory_label,
)
from .capture import Recording
from .controls import Controls
from .controllers import Reader as ControllerReader
from .experience import Experience
from .host import allocations, detect, requirements
from .i18n import Translator
from .jobs import Jobs
from .packages import FileBatch
from .performance import NativeSession
from .phoneview import BootScreen, PhoneView
from .recoveryview import RecoveryUI
from .setup import resource_root
from .storageview import StorageUI
from .theme import brand_icon, icon, stylesheet
from .telemetry import Sample as SensorSample, Sampler
from .updates import Updater, current_version
from .wallpaper import Wallpaper, builtin, fitted, picture
from .widgets import button, card, combo, label, row, scroll_page, text_dialog, toggle


PAGES = (
    ("overview", "Overview", "home"),
    ("library", "Library", "library"),
    ("instances", "Phones", "instances"),
    ("controls", "Controls", "keyboard"),
    ("display", "Display", "display"),
    ("performance", "Performance", "performance"),
    ("look", "Look", "look"),
    ("phone", "Phone", "phone"),
    ("recovery", "Recovery", "recovery"),
    ("system", "System", "settings"),
    ("updates", "Updates", "updates"),
)


class MainWindow(QMainWindow):
    def __init__(self, hub, no_autostart=False):
        super().__init__()
        self.hub, self.phone = hub, hub.phone()
        self.settings = hub.paths.settings()
        self.tr = Translator(self.settings.language)
        self.qt_translator = QTranslator(self)
        self.jobs = Jobs(self)
        self.recovery = RecoveryUI(self)
        self.storage = StorageUI(self)
        self.updater = Updater(hub.paths)
        self.update_release, self.updated_image = None, None
        self.update_status, self.update_fraction = "", 0.0
        self.update_progress_widget = None
        self.telemetry = Sampler(hub.hardware)
        self.sensor_sample = SensorSample()
        self.sensor_labels = {}
        self.recovery.widgets = None
        self.pending_live = {}
        self.pending_wallpapers = {}
        self.pending_launch = {}
        self.pending_imports = {}
        self.transfer_cancel = threading.Event()
        self.setAcceptDrops(True)
        self.live_games = {}
        self.setup_widgets = None
        self.setup_message, self.setup_fraction = "", 0.0
        self.views = {}
        self.native_sessions = {}
        self.controllers = []
        self.page_name, self.page_generation = "overview", 0
        self.apps, self.search, self.filter, self.app_page = [], "", 0, 0
        self.wall_page = 0
        self._app_icon_labels = {}
        self.control_package, self.control_landscape = "", False
        self._closing = False
        self.status_text = ""
        self.setWindowTitle("Velox")
        self.setWindowIcon(brand_icon())
        screen = self.screen() or QApplication.primaryScreen()
        ratio, geometry = screen.devicePixelRatio(), screen.geometry()
        self.hub.hardware = detect(
            self.hub.paths.root,
            round(geometry.width() * ratio),
            round(geometry.height() * ratio),
            round(screen.refreshRate()),
        )
        root = QWidget()
        self.setCentralWidget(root)
        self.columns = QHBoxLayout(root)
        self.columns.setContentsMargins(0, 0, 0, 0)
        self.columns.setSpacing(0)
        self.sidebar = QFrame()
        self.sidebar.setObjectName("sidebar")
        self.sidebar.setFixedWidth(224)
        self.side = QVBoxLayout(self.sidebar)
        self.side.setContentsMargins(12, 22, 12, 16)
        self.side.setSpacing(6)
        self.columns.addWidget(self.sidebar)
        self.body = QWidget()
        self.body_layout = QVBoxLayout(self.body)
        self.body_layout.setContentsMargins(0, 0, 0, 0)
        self.columns.addWidget(self.body, 1)
        self.status = label("", "subtitle", True)
        self.statusBar().addWidget(self.status, 1)
        self.scale_ui()
        self.build_sidebar()
        self.navigate("overview")
        self.timer = QTimer(self)
        self.timer.setInterval(2000)
        self.timer.timeout.connect(self.tick)
        self.timer.start()
        self.jobs.submit("graphics", self.refine_hardware, self.hardware_ready, self.message)
        if not no_autostart and self.settings.auto_start and hub.setup.installed():
            QTimer.singleShot(250, self.start_phone)

    def refine_hardware(self):
        from .graphics import probe

        result = probe()
        if result.get("emulator_gpu") or result.get("renderer"):
            self.hub.hardware = dataclasses.replace(
                self.hub.hardware,
                gpu=result.get("emulator_gpu") or result["renderer"],
                software=bool(result.get("emulator_software", result.get("software", False))),
                dedicated_mb=result.get("dedicated_mb", 0),
            )
        self.hub.hardware_probed = True

    def hardware_ready(self, _value):
        if self._closing:
            return
        try:
            if self.phone.identity in self.hub.finish_initial_recommendations():
                self.settings = self.hub.paths.settings(self.phone.identity)
        except (OSError, VeloxError) as error:
            self.message(str(error))
        self.navigate(self.page_name)

    def scale_ui(self):
        screen = self.screen() or QApplication.primaryScreen()
        area, ratio = screen.availableGeometry(), screen.devicePixelRatio()
        physical_height = area.height() * ratio
        automatic = max(
            round(ratio * 100),
            150 if physical_height >= 1800 else 125 if physical_height >= 950 else 100,
        )
        requested = self.settings.ui_percent or automatic
        factor = min(
            requested / (100 * ratio), (area.width() - 28) / 900, (area.height() - 70) / 640
        )
        self.ui_factor = factor
        self.setStyleSheet(stylesheet(factor))
        self.resize(round(900 * factor), round(640 * factor))
        self.setMinimumSize(
            min(area.width() - 20, round(720 * factor)),
            min(area.height() - 60, round(500 * factor)),
        )
        self.sidebar.setFixedWidth(round(224 * factor))

    def build_sidebar(self):
        application = QApplication.instance()
        application.removeTranslator(self.qt_translator)
        QLocale.setDefault(QLocale("de_DE" if self.tr.language == "de" else "en_US"))
        if self.tr.language == "de" and self.qt_translator.load(
            "qtbase_de", QLibraryInfo.path(QLibraryInfo.LibraryPath.TranslationsPath)
        ):
            application.installTranslator(self.qt_translator)

        def clear(layout):
            while layout.count():
                item = layout.takeAt(0)
                if item.widget():
                    item.widget().hide()
                    item.widget().deleteLater()
                elif item.layout():
                    clear(item.layout())
                    item.layout().deleteLater()

        clear(self.side)
        brand = label("Velox", "brand")
        brand.setPixmap(brand_icon().pixmap(42, 42))
        row(self.side, brand, label("Velox", "brand"), None)
        self.side.addSpacing(18)
        navigation = QWidget()
        navigation.setObjectName("clear")
        links = QVBoxLayout(navigation)
        links.setContentsMargins(0, 0, 0, 0)
        links.setSpacing(6)
        area = QScrollArea()
        area.setObjectName("clear")
        area.viewport().setObjectName("clear")
        area.setFrameShape(QFrame.Shape.NoFrame)
        area.setWidgetResizable(True)
        area.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        area.setWidget(navigation)
        self.side.addWidget(area, 1)
        self.navigation_area = area
        self.navigation = {}
        for name, title, symbol in PAGES:
            nav = button(self.tr(title), lambda name=name: self.navigate(name))
            nav.setIcon(icon(symbol))
            nav.setCheckable(True)
            nav.setObjectName("nav")
            links.addWidget(nav)
            self.navigation[name] = nav
        links.addStretch()
        self.state_label = label("", "subtitle")
        self.side.addWidget(self.state_label)
        self.start_button = button(self.tr("Start phone"), self.start_phone, True)
        self.start_button.setIcon(icon("play", "#ffffff"))
        self.side.addWidget(self.start_button)
        self.status.setText(self.tr(self.status_text))
        self.tick()

    def message(self, text):
        if not self._closing:
            self.status_text = text
            self.status.setText(self.tr(text))

    def preference(self, key, value, *, live=False):
        if self.recovery.busy(self.phone.identity):
            self.message("This phone is busy with a maintenance action.")
            return
        setattr(self.settings, key, value)
        self.hub.paths.save(self.phone.identity, self.settings)
        if key == "language":
            self.tr.language = value
            self.build_sidebar()
            self.navigate(self.page_name)
        if key == "ui_percent":
            self.scale_ui()
        view = self.views.get(self.phone.identity)
        if view:
            view.settings = dataclasses.replace(self.settings)
            if key == "language":
                view.retranslate(value)
            view.canvas.surface.frame_enabled = self.settings.frame
            if key == "phone_percent":
                view.fit(view._orientation)
        if live:
            settings, phone = dataclasses.replace(self.settings), self.phone
            self.pending_live[phone.identity] = (phone, settings)
            self.drain_preferences(phone.identity)

    def drain_preferences(self, identity):
        if identity not in self.pending_live or "preferences-" + identity in self.jobs.active:
            return
        phone, settings = self.pending_live[identity]
        if phone.state != "running" or not phone.alive():
            return
        self.pending_live.pop(identity)

        def finished(_value):
            self.update_native(phone)
            self.drain_preferences(identity)

        def failed(message):
            self.message(message)
            self.drain_preferences(identity)

        live = self.session_settings(phone, settings)
        self.jobs.submit(
            "preferences-" + identity,
            lambda: (phone.apply_live(live), Experience(phone).sync(settings)),
            finished,
            failed,
            priority=2,
        )

    def session_settings(self, phone, settings=None):
        settings = settings or self.hub.paths.settings(phone.identity)
        view = self.views.get(phone.identity)
        if view and PACKAGE.fullmatch(view.current_app):
            try:
                return Controls(phone).preset(view.current_app).apply(settings)
            except (OSError, VeloxError):
                pass
        return dataclasses.replace(settings)

    def update_native(self, phone):
        if self._closing:
            return
        if phone.identity not in self.native_sessions:
            if not phone.alive():
                return
            self.native_sessions[phone.identity] = NativeSession(phone)
        view = self.views.get(phone.identity)
        focused = bool(view and view.focused())
        settings = self.session_settings(phone)
        identity = view.current_app if view and PACKAGE.fullmatch(view.current_app) else None
        self.native_sessions[phone.identity].request(settings, focused, identity)
        if view:
            view.settings.rate = settings.rate
            view.update_video_rate()

    def navigate(self, name):
        if self._closing:
            return
        self.page_name = name
        self.page_generation += 1
        self.setup_widgets = None
        self.sensor_labels = {}
        self.recovery.widgets = None
        self.update_progress_widget = None
        for key, widget in self.navigation.items():
            widget.setChecked(key == name)
        while self.body_layout.count():
            item = self.body_layout.takeAt(0)
            if item.widget():
                item.widget().hide()
                item.widget().deleteLater()
        if not self.hub.setup.installed() and name not in ("system", "recovery"):
            page = self.setup_page()
        else:
            page = getattr(self, "page_" + name)()
        if name not in ("recovery", "instances", "phone") and self.recovery.busy(
            self.phone.identity
        ):
            page.setEnabled(False)
        self.body_layout.addWidget(page)
        QTimer.singleShot(0, self.ensure_navigation)

    def ensure_navigation(self):
        if not self._closing and self.page_name in self.navigation:
            self.navigation_area.ensureWidgetVisible(self.navigation[self.page_name], 0, 12)

    def show_phone(self, phone=None):
        phone = phone or self.phone
        if phone.identity not in self.views:
            view = PhoneView(phone, Translator(self.hub.paths.settings(phone.identity).language))
            view.setStyleSheet(stylesheet())
            view.failure.connect(self.message)
            view.action.connect(lambda action, phone=phone: self.phone_action(phone, action))
            view.files_dropped.connect(lambda files, phone=phone: self.choose_files(files, phone))
            view.observed.connect(
                lambda _width, _height, _rotation, app, phone=phone: self.game_foreground(
                    phone, app
                )
            )
            view.focus_changed.connect(lambda phone=phone: self.update_native(phone))
            self.views[phone.identity] = view
        view = self.views[phone.identity]
        view.show() if view.isFullScreen() else view.showNormal()
        view.raise_()
        view.activateWindow()
        return view

    def select_phone(self, identity):
        self.storage.details = None
        self.phone = self.hub.phone(identity)
        self.settings = self.hub.paths.settings(identity)
        self.tr.language = self.settings.language
        self.apps = Experience(self.phone).library()
        self.search, self.app_page = "", 0
        self.build_sidebar()
        self.navigate(self.page_name)

    def start_phone(self, phone=None, session=None):
        if not self.hub.setup.installed():
            self.navigate("overview")
            return
        phone = phone or self.phone
        if self.recovery.busy(phone.identity):
            self.message("This phone is busy with a maintenance action.")
            return
        settings = session or self.hub.paths.settings(phone.identity)
        view = self.show_phone(phone)
        if phone.alive() and phone.state == "running":
            view.connect_live()
            return
        if phone.state == "starting":
            return
        view.booting()

        def ready(_value):
            if self._closing:
                return
            view.connect_live()
            self.drain_preferences(phone.identity)
            self.tick()
            self.refresh_library(phone)
            if getattr(phone, "navigation_notice", ""):
                self.message(phone.navigation_notice)
            if self.page_name in (
                "overview",
                "instances",
                "performance",
                "controls",
                "system",
                "display",
                "phone",
            ):
                self.navigate(self.page_name)
            if phone.identity in self.pending_launch:
                self.launch_game(phone, self.pending_launch[phone.identity])
            if phone.identity in self.pending_imports:
                self.choose_files(self.pending_imports.pop(phone.identity), phone)

        def failed(message):
            view.progress(message, 0)
            self.message(message)
            self.tick()

        self.jobs.submit(
            "start-" + phone.identity,
            lambda report: phone.start(settings, report),
            ready,
            failed,
            view.progress,
        )

    def stop_phone(self, phone=None, *, then=None):
        phone = phone or self.phone
        if self.recovery.busy(phone.identity):
            self.message("This phone is busy with a maintenance action.")
            return
        view = self.views.get(phone.identity)
        if view:
            view.hide()

        def stopped(_value):
            self.tick()
            if then and not self._closing:
                then()

        self.jobs.submit("stop-" + phone.identity, phone.stop, stopped, self.message)

    def restart_phone(self):
        phone = self.phone
        self.stop_phone(phone, then=lambda: self.start_phone(phone))

    def tick(self):
        if self._closing:
            return
        self.recovery.schedule_tick()
        if self.page_name == "system" and self.isVisible() and not self.isMinimized():
            self.refresh_sensors()
        for phone in list(self.hub._phones.values()):
            phone.observe_exit()
            self.update_native(phone)
            view = self.views.get(phone.identity)
            if (
                phone.state == "running"
                and phone.alive()
                and (view and view.focused() or phone is self.phone and self.isActiveWindow())
            ):
                self.sync_guest_preferences(phone)
        alive = self.phone.alive()
        self.state_label.setText(
            self.tr(
                "Starting…" if self.phone.state == "starting" else "Running" if alive else "Off"
            )
        )
        self.start_button.setText(self.tr("Show phone" if alive else "Start phone"))
        self.start_button.setEnabled(
            self.phone.state not in ("starting", "stopping")
            and not self.recovery.busy(self.phone.identity)
        )

    def sync_guest_preferences(self, phone):
        if (
            self.pending_live.get(phone.identity)
            or "preferences-" + phone.identity in self.jobs.active
        ):
            return
        settings = self.hub.paths.settings(phone.identity)

        def synced(value):
            if value == settings:
                return
            view = self.views.get(phone.identity)
            if view:
                view.settings.navigation, view.settings.auto_rotate = (
                    value.navigation,
                    value.auto_rotate,
                )
                view.settings.language = value.language
                view.retranslate(value.language)
            self.update_native(phone)
            if phone is self.phone:
                self.settings = value
                self.tr.language = value.language
                self.build_sidebar()
                self.navigate(self.page_name)

        self.jobs.submit(
            "sync-" + phone.identity,
            lambda: Experience(phone).sync(settings, from_host=False),
            synced,
            self.message,
        )

    def phone_action(self, phone, action):
        if action == "power":
            self.select_phone(phone.identity)
            if (
                QMessageBox.question(self, "Velox", self.tr("Stop this phone?"))
                == QMessageBox.StandardButton.Yes
            ):
                self.stop_phone(phone)
        elif action == "screenshot":
            self.jobs.submit(
                "capture-" + phone.identity,
                phone.screenshot,
                lambda file: self.message(str(file)),
                self.message,
            )
        elif action == "record":

            def recorded(file):
                view = self.views.get(phone.identity)
                if view:
                    view.record_button.setIcon(
                        icon("record", "#ff637f" if Recording(phone).active() else "#a3acc1")
                    )
                self.message(
                    str(file) if file else "Recording started. Click Record again to finish."
                )

            self.jobs.submit(
                "record-" + phone.identity, Recording(phone).toggle, recorded, self.message
            )
        elif action == "settings":
            self.jobs.submit(
                "guest-settings-" + phone.identity,
                Experience(phone).open_settings,
                failed=self.message,
            )
        elif action == "controls":
            view = self.views.get(phone.identity)
            if view:
                view.edit_controls()
        elif action in ("library", "display", "system"):
            self.select_phone(phone.identity)
            self.navigate(action)
            self.showNormal()
            self.raise_()
            self.activateWindow()

    def setup_page(self):
        page, body = scroll_page(
            self.tr("Android setup"), self.tr("Made for your desktop. Ready for your games.")
        )
        card(
            body,
            "Velox + Android",
            self.tr(
                "Automatic setup downloads about 2.2 GB from Google and needs at least 10 GB of free space."
            ),
        )
        issues = requirements(self.hub.hardware, self.hub.paths.root)
        for _kind, message in issues:
            card(body, self.tr(message))
        box = card(body, self.tr("Google's Android SDK terms"))
        box.addWidget(
            button(
                self.tr("Read terms"),
                lambda: text_dialog(
                    self,
                    self.tr("Google's Android SDK terms"),
                    bounded_read(resource_root() / "android-sdk-license.txt", 256 * 1024).decode(),
                    self.tr,
                ).exec(),
            )
        )
        accepted = QCheckBox(self.tr("I accept Google's Android SDK terms."))
        accepted.setChecked(self.hub.setup.accepted())
        box.addWidget(accepted)
        progress = QProgressBar()
        progress.setRange(0, 100)
        box.addWidget(progress)
        stage = label("", "subtitle", True)
        box.addWidget(stage)
        progress.setValue(round(self.setup_fraction * 100))
        stage.setText(self.tr(self.setup_message))
        self.setup_widgets = progress, stage

        def install():
            self.hub.setup.accept()
            self.hub.setup.cancel.clear()
            start.setEnabled(False)

            def ready(_value):
                self.navigate("overview")
                self.start_phone() if self.settings.auto_start else None

            def failed(message):
                self.setup_message = message
                self.navigate(self.page_name)

            self.jobs.submit("setup", self.hub.setup.install, ready, failed, self.setup_progress)

        start = button(self.tr("Accept and download"), install, True)
        start.setEnabled(accepted.isChecked() and not issues and "setup" not in self.jobs.active)
        accepted.toggled.connect(
            lambda value: start.setEnabled(value and not issues and "setup" not in self.jobs.active)
        )
        box.addWidget(start)
        if "setup" in self.jobs.active:
            box.addWidget(button(self.tr("Pause download"), self.hub.setup.cancel.set))
        body.addStretch()
        return page

    def setup_progress(self, message, fraction):
        self.setup_message, self.setup_fraction = message, fraction
        if self.setup_widgets:
            progress, stage = self.setup_widgets
            progress.setValue(round(fraction * 100))
            stage.setText(self.tr(message))

    def page_overview(self):
        page, body = scroll_page(
            self.tr("Your Android. Your play space."),
            self.tr("Your Android apps, at a comfortable desktop size."),
        )
        box = card(
            body,
            self.phone.metadata["name"],
            self.tr("Use Google Play, install your own apps or drop files into Velox."),
        )
        row(
            box,
            button(
                self.tr("Show phone" if self.phone.alive() else "Start phone"),
                self.start_phone,
                True,
            ),
            button(self.tr("Install app"), self.choose_files),
            button(self.tr("Stop phone"), self.stop_phone),
        )
        card(
            body,
            self.tr("Native acceleration"),
            self.tr(
                "Android uses KVM for native CPU acceleration. Your apps and saves stay on this PC."
            ),
        )
        _, width, height, _ = DISPLAYS[self.settings.display]
        cpu, ram = allocations(self.settings, self.hub.hardware)
        card(
            body,
            f"{width} × {height} · {self.settings.rate} Hz",
            f"{cpu} CPU · {max(3, ram)} GB · {self.hub.hardware.gpu}",
        )
        body.addStretch()
        return page

    def refresh_library(self, phone=None):
        phone = phone or self.phone

        def loaded(apps):
            if phone is self.phone:
                self.apps = apps
                if self.page_name == "library":
                    self.navigate("library")

        self.jobs.submit(
            "library-" + phone.identity,
            lambda: Experience(phone).library(refresh=True),
            loaded,
            self.message,
        )

    def page_library(self):
        page, body = scroll_page(
            self.tr("Your game library"), self.tr("Your games. Your apps. One play space.")
        )
        tools = QHBoxLayout()
        search = QLineEdit()
        search.setPlaceholderText(self.tr("Search games and apps"))
        search.setText(self.search)
        search.addAction(icon("search"), QLineEdit.ActionPosition.LeadingPosition)
        search.setClearButtonEnabled(True)
        tools.addWidget(search, 1)
        tools.addWidget(button(self.tr("Refresh"), self.refresh_library))
        tools.addWidget(button(self.tr("Install app"), self.choose_files, True))
        body.addLayout(tools)
        filters = QHBoxLayout()
        for index, title in enumerate(("All apps", "Favorites", "Games", "Recent")):
            widget = button(self.tr(title), lambda index=index: self.filter_library(index))
            widget.setCheckable(True)
            widget.setChecked(index == self.filter)
            filters.addWidget(widget)
        body.addLayout(filters)
        grid = QGridLayout()
        grid.setSpacing(12)
        body.addLayout(grid)
        footer = QHBoxLayout()
        body.addLayout(footer)
        self.apps = self.apps or Experience(self.phone).library()

        def populate():
            self._app_icon_labels = {}
            while grid.count():
                item = grid.takeAt(0)
                if item.widget():
                    item.widget().hide()
                    item.widget().deleteLater()
            while footer.count():
                item = footer.takeAt(0)
                if item.widget():
                    item.widget().hide()
                    item.widget().deleteLater()
            selected = [
                app
                for app in self.apps
                if self.search.casefold() in (app.label + " " + app.package).casefold()
                and (
                    self.filter == 0
                    or self.filter == 1
                    and app.favorite
                    or self.filter == 2
                    and app.game
                    or self.filter == 3
                    and app.played
                )
            ]
            selected.sort(
                key=(lambda app: (-app.played, app.label.casefold()))
                if self.filter == 3
                else lambda app: app.label.casefold()
            )
            total = max(1, math.ceil(len(selected) / 6))
            self.app_page = min(self.app_page, total - 1)
            for position, app in enumerate(selected[self.app_page * 6 : self.app_page * 6 + 6]):
                panel = QFrame()
                panel.setObjectName("card")
                box = QVBoxLayout(panel)
                box.setContentsMargins(14, 12, 14, 12)
                picture_label = label("")
                picture_label.setFixedSize(44, 44)
                picture_label.setProperty("veloxIconUpdated", str(app.updated))
                self._app_icon_labels[app.component] = picture_label
                star = button("★" if app.favorite else "☆", lambda app=app: self.favorite(app))
                star.setFixedWidth(38)
                row(box, picture_label, None, star)
                box.addWidget(label(app.label, wrap=True))
                box.addWidget(label(app.package, "section", True))
                row(
                    box,
                    button(self.tr("Open"), lambda app=app: self.open_app(app), True),
                    button(self.tr("Controls"), lambda app=app: self.game_controls(app)),
                )
                grid.addWidget(panel, position // 3, position % 3)
                phone = self.phone

                def loaded(
                    path, component=app.component, updated=app.updated, identity=phone.identity
                ):
                    target = self._app_icon_labels.get(component)
                    if (
                        self.page_name == "library"
                        and self.phone.identity == identity
                        and target
                        and target.property("veloxIconUpdated") == str(updated)
                    ):
                        target.setPixmap(
                            QPixmap(str(path)).scaled(
                                44,
                                44,
                                Qt.AspectRatioMode.KeepAspectRatio,
                                Qt.TransformationMode.SmoothTransformation,
                            )
                        )

                self.jobs.submit(
                    "icon-" + phone.identity + "-" + app.component + "-" + str(app.updated),
                    lambda app=app, phone=phone: Experience(phone).icon(app),
                    loaded,
                    priority=-1,
                )
            if not selected:
                grid.addWidget(
                    label(
                        self.tr(
                            "No matching apps."
                            if self.apps
                            else "Start the phone once to load your apps."
                        ),
                        "subtitle",
                        True,
                    ),
                    0,
                    0,
                )
            footer.addWidget(label(f"{len(selected)} · {self.app_page + 1} / {total}", "subtitle"))
            footer.addStretch()
            previous = button(self.tr("Previous"), lambda: turn(-1))
            previous.setEnabled(self.app_page > 0)
            next_button = button(self.tr("Next"), lambda: turn(1))
            next_button.setEnabled(self.app_page + 1 < total)
            footer.addWidget(previous)
            footer.addWidget(next_button)

        def turn(delta):
            self.app_page += delta
            populate()

        def changed(value):
            self.search = value
            self.app_page = 0
            populate()

        search.textChanged.connect(changed)
        populate()
        body.addStretch()
        return page

    def filter_library(self, index):
        self.filter = index
        self.app_page = 0
        self.navigate("library")

    def favorite(self, app):
        phone = self.phone

        def saved(_value):
            app.favorite = not app.favorite
            self.navigate("library") if self.page_name == "library" else None

        self.jobs.submit(
            "favorite-" + app.component,
            lambda: Experience(phone).call("favorite", app.component, not app.favorite),
            saved,
            self.message,
        )

    def open_app(self, app):
        phone = self.phone
        self.pending_launch[phone.identity] = app
        desired = Controls(phone).preset(app.package).apply(self.hub.paths.settings(phone.identity))
        if not phone.alive():
            self.start_phone(phone, desired)
            return
        if phone.state == "starting":
            self.show_phone(phone)
            return
        actual = Settings.from_dict(phone.runtime()["settings"])
        changed = desired.rate > actual.rate or any(
            getattr(desired, key) != getattr(actual, key)
            for key in ("display", "cores", "memory", "gpu", "root")
        )
        if changed:
            prompt = QMessageBox(self)
            prompt.setWindowTitle("Velox")
            prompt.setTextFormat(Qt.TextFormat.PlainText)
            prompt.setText(
                self.tr(
                    "Restart this phone to apply the game's screen, graphics and resource preset?"
                )
                + "\n\n"
                + app.label
            )
            restart = prompt.addButton(
                self.tr("Restart and play"), QMessageBox.ButtonRole.AcceptRole
            )
            current = prompt.addButton(
                self.tr("Use current phone"), QMessageBox.ButtonRole.ActionRole
            )
            prompt.addButton(self.tr("Cancel"), QMessageBox.ButtonRole.RejectRole)
            prompt.exec()
            if prompt.clickedButton() is restart:
                view = self.views.get(phone.identity)
                if view:
                    view.hide()
                self.jobs.submit(
                    "stop-" + phone.identity,
                    phone.stop,
                    lambda _value: self.start_phone(phone, desired),
                    self.message,
                )
                return
            if prompt.clickedButton() is not current:
                self.pending_launch.pop(phone.identity, None)
                return
        self.launch_game(phone, app)

    def launch_game(self, phone, app):
        self.show_phone(phone)

        def done(_value):
            self.pending_launch.pop(phone.identity, None)
            self.game_foreground(phone, app.package, force=True)

        self.jobs.submit(
            "launch-" + phone.identity,
            lambda: (phone.launch(app.component), Experience(phone).call("played", app.component)),
            done,
            self.message,
        )

    def game_foreground(self, phone, identity, force=False):
        if (
            not PACKAGE.fullmatch(identity)
            or phone.state != "running"
            or phone.identity in self.pending_launch
        ):
            return
        if not force and self.live_games.get(phone.identity) == identity:
            return
        self.live_games[phone.identity] = identity
        try:
            preset = Controls(phone).preset(identity)
            desired = preset.apply(self.hub.paths.settings(phone.identity))
            actual = Settings.from_dict(phone.runtime()["settings"])
            if preset.enabled and (
                desired.rate > actual.rate
                or any(
                    getattr(desired, key) != getattr(actual, key)
                    for key in ("display", "cores", "memory", "gpu", "root")
                )
            ):
                self.message("Restart the phone to apply this game's hardware preset.")
            view = self.views.get(phone.identity)
            if view:
                view.session_fullscreen(preset.fullscreen if preset.enabled else -1)
                view.settings.rate = desired.rate
            self.pending_live[phone.identity] = phone, desired
            self.drain_preferences(phone.identity)
        except (VeloxError, OSError) as error:
            self.message(str(error))

    def choose_files(self, files=None, phone=None):
        if self._closing:
            return
        phone = phone or self.phone
        if (
            phone.state == "stopping"
            or self.recovery.busy(phone.identity)
            or any("import-" in key and key.endswith(phone.identity) for key in self.jobs.active)
        ):
            self.message("Finish the current phone action before importing more files.")
            return
        if files is None or isinstance(files, bool):
            files, _filter = QFileDialog.getOpenFileNames(
                self, self.tr("Install apps or send files")
            )
        if not files:
            return
        if not phone.alive() or phone.state != "running":
            if self.recovery.confirm(
                "Start phone",
                self.tr("Start this phone before installing apps or sending these files?"),
                "Start phone",
            ):
                self.pending_imports[phone.identity] = list(files)
                self.start_phone(phone)
            return
        density = DISPLAYS[self.hub.paths.settings(phone.identity).display][3]

        def review(batch):
            if self._closing:
                self.jobs.submit("import-cleanup-" + phone.identity, batch.close)
                return
            text = "\n".join(
                (
                    self.tr("Install app") + ": " + op.destination
                    if op.kind == "install"
                    else op.destination
                )
                for op in batch.operations
            )
            if not self.recovery.confirm("Install apps or send files", text, "Continue"):
                self.jobs.submit("import-cleanup-" + phone.identity, batch.close)
                return

            def apply(report):
                try:
                    batch.apply(report)
                finally:
                    batch.close()

            self.jobs.submit(
                "import-apply-" + phone.identity,
                apply,
                lambda _value: self.refresh_library(phone),
                self.message,
                lambda message, _fraction: self.message(message),
            )

        self.jobs.submit(
            "import-prepare-" + phone.identity,
            lambda: FileBatch(phone, list(map(Path, files)), density, self.transfer_cancel),
            review,
            self.message,
        )

    def dragEnterEvent(self, event):
        from .drops import local_files

        if not self._closing and local_files(event.mimeData()):
            event.acceptProposedAction()

    def dropEvent(self, event):
        from .drops import local_files

        files = local_files(event.mimeData())
        if files and not self._closing:
            event.acceptProposedAction()
            self.choose_files(files)

    def page_instances(self):
        return self.storage.build()

    def create_phone(self):
        name, accepted = QInputDialog.getText(
            self, self.tr("New phone"), self.tr("Enter a phone name")
        )
        if accepted:
            try:
                self.select_phone(self.hub.create(name))
            except (VeloxError, OSError) as error:
                self.message(str(error))

    def rename_phone(self, item):
        if self.recovery.busy(item["id"]):
            self.message("This phone is busy with a maintenance action.")
            return
        name, accepted = QInputDialog.getText(
            self, self.tr("Rename"), self.tr("Enter a phone name"), text=item["name"]
        )
        if accepted:
            try:
                self.hub.rename(item["id"], name)
                self.navigate("instances")
            except (VeloxError, OSError) as error:
                self.message(str(error))

    def start_instance(self, identity):
        self.select_phone(identity)
        self.start_phone()

    def game_controls(self, app):
        self.control_package = app.package
        self.navigate("controls")

    def page_controls(self):
        from .controlview import KINDS, key_label

        page, body = scroll_page(
            self.tr("Control Studio"),
            self.tr("Only the focused phone receives your keyboard and mouse input."),
        )
        apps = self.apps or Experience(self.phone).library()
        unique = {app.package: app for app in apps}
        view = self.views.get(self.phone.identity)
        if view and view.control_editor:
            box = card(body, self.tr("Control Studio is open on the phone."))
            row(
                box,
                button(self.tr("Return to editor"), self.show_phone),
                button(
                    self.tr("Save and close"),
                    lambda: (view.control_editor.finish(True), self.navigate("controls")),
                ),
            )
            body.addStretch()
            return page
        if view and PACKAGE.fullmatch(view.current_app) and view.current_app not in unique:
            from .experience import App

            unique[view.current_app] = App(
                view.current_app + "/.Main", view.current_app, view.current_app
            )
        if not unique:
            card(body, self.tr("Start the phone once to load your apps."))
            body.addStretch()
            return page
        if self.control_package not in unique:
            self.control_package = next(iter(unique))
        identity = self.control_package
        tools = Controls(self.phone)
        profile, preset = tools.load(identity, self.control_landscape), tools.preset(identity)
        box = card(body, self.tr("Your game"))

        def select(value):
            self.control_package = value
            self.navigate("controls")

        box.addWidget(
            combo(
                [
                    (app.label, key)
                    for key, app in sorted(
                        unique.items(), key=lambda item: item[1].label.casefold()
                    )
                ],
                identity,
                select,
            )
        )

        def orientation(value):
            self.control_landscape = value
            self.navigate("controls")

        box.addWidget(
            combo(
                [(self.tr("Portrait"), False), (self.tr("Landscape"), True)],
                self.control_landscape,
                orientation,
            )
        )

        def enabled(value):
            profile.enabled = value
            self.save_controls(identity, profile)

        toggle(
            body,
            self.tr("Keyboard and mouse mapping"),
            self.tr("F10 pauses or resumes this game's controls."),
            profile.enabled,
            enabled,
        )
        box = card(
            body,
            self.tr("Your layout"),
            self.tr("Tap, movement sticks and swipes can be placed directly on the phone."),
        )
        editor = button(self.tr("Edit on phone"), lambda: self.edit_on_phone(unique[identity]))
        editor.setEnabled(self.phone.state == "running")
        row(
            box,
            editor,
            button(self.tr("Export"), lambda: self.export_controls(identity)),
            button(self.tr("Import"), lambda: self.import_controls(identity)),
        )
        if (self.phone.folder / "controls-undo.veloxcontrols").exists():
            box.addWidget(button(self.tr("Undo import"), self.undo_controls))
        for index, binding in enumerate(profile.bindings):
            row(
                box,
                label(
                    f"{self.tr(KINDS[binding.kind])} · {key_label(binding.key)} · {binding.x:.0%}, {binding.y:.0%}"
                ),
                None,
                button(
                    self.tr("Delete"),
                    lambda index=index: self.delete_binding(identity, profile, index),
                ),
            )
        box = card(
            body,
            self.tr("Controller"),
            self.tr(
                "Use native Android gamepad support or map buttons and sticks to screen touches."
            ),
        )

        def controller_value(key, value):
            setattr(profile, key, value)
            self.save_controls(identity, profile)

        box.addWidget(
            combo(
                [
                    (self.tr("Off"), 0),
                    (self.tr("Native Android controller"), 1),
                    (self.tr("Touch mapping"), 2),
                ],
                profile.pad_mode,
                lambda value: controller_value("pad_mode", value),
            )
        )
        devices = [(self.tr("Auto"), "")] + [
            (device.name, device.identity) for device in self.controllers
        ]
        if profile.pad_device and all(value != profile.pad_device for _name, value in devices):
            devices.append((self.tr("Saved controller (disconnected)"), profile.pad_device))
        row(
            box,
            combo(devices, profile.pad_device, lambda value: controller_value("pad_device", value)),
            button(self.tr("Refresh controllers"), self.refresh_controllers),
        )
        box.addWidget(
            combo(
                [
                    (self.tr(name), value)
                    for value, name in enumerate(("Auto", "Xbox", "PlayStation", "Generic"))
                ],
                profile.pad_layout,
                lambda value: controller_value("pad_layout", value),
            )
        )
        for title, key, low, high in (
            ("Deadzone", "deadzone", 0.05, 0.4),
            ("Sensitivity", "sensitivity", 0.5, 2.0),
        ):
            field = QDoubleSpinBox()
            field.setRange(low, high)
            field.setSingleStep(0.01 if key == "deadzone" else 0.1)
            field.setDecimals(2)
            field.setValue(getattr(profile, key))
            field.setKeyboardTracking(False)
            field.valueChanged.connect(lambda value, key=key: controller_value(key, value))
            row(box, label(self.tr(title)), field)
        invert = QCheckBox(self.tr("Invert Y"))
        invert.setChecked(profile.invert_y)
        invert.toggled.connect(lambda value: controller_value("invert_y", value))
        box.addWidget(invert)
        box = card(
            body,
            self.tr("Game preset"),
            self.tr("Inherit your phone defaults or choose settings just for this game."),
        )
        enabled = QCheckBox(self.tr("Use this game's preset"))
        enabled.setChecked(preset.enabled)
        enabled.toggled.connect(lambda value: self.save_preset_value(identity, "enabled", value))
        box.addWidget(enabled)
        inherit = self.tr("Phone defaults")
        for title, key, choices in (
            (
                "Screen resolution",
                "display",
                [(inherit, -1)]
                + [
                    (f"{name} · {width} × {height}", index)
                    for index, (name, width, height, _density) in enumerate(DISPLAYS)
                ],
            ),
            ("Refresh rate", "rate", [(inherit, 0)] + [(f"{rate} Hz", rate) for rate in RATES]),
            (
                "CPU cores",
                "cores",
                [(inherit, -1), (self.tr("Auto"), 0)]
                + [
                    (str(value), value)
                    for value in CPU_CHOICES
                    if value and value <= self.hub.hardware.cpu_limit
                ],
            ),
            (
                "Memory (GB)",
                "memory",
                [(inherit, -1), (self.tr("Auto"), 0)]
                + [
                    (memory_label(value, self.settings.language), value)
                    for value in RAM_CHOICES
                    if value and value <= self.hub.hardware.memory_limit
                ],
            ),
            (
                "Graphics",
                "gpu",
                [
                    (inherit, -1),
                    (self.tr("Auto"), 0),
                    (self.tr("GPU"), 1),
                    (self.tr("Software"), 2),
                ],
            ),
            ("Full screen", "fullscreen", [(inherit, -1), (self.tr("Off"), 0), (self.tr("On"), 1)]),
        ):
            row(
                box,
                label(self.tr(title)),
                combo(
                    choices,
                    getattr(preset, key),
                    lambda value, key=key: self.save_preset_value(identity, key, value),
                ),
            )
        body.addStretch()
        return page

    def save_controls(self, identity, profile):
        try:
            Controls(self.phone).save(identity, self.control_landscape, profile)
            view = self.views.get(self.phone.identity)
            if view:
                view.reload_controls()
        except (VeloxError, OSError) as error:
            self.message(str(error))
            self.navigate("controls")

    def refresh_controllers(self):
        def scan():
            reader = ControllerReader()
            try:
                if not reader.library:
                    raise VeloxError(
                        "The native controller library is unavailable. Repair this Velox installation."
                    )
                return reader.scan(force=True)
            finally:
                reader.close()

        def done(devices):
            self.controllers = devices
            if self.page_name == "controls":
                self.navigate("controls")

        self.jobs.submit("controllers", scan, done, self.message)

    def delete_binding(self, identity, profile, index):
        del profile.bindings[index]
        self.save_controls(identity, profile)
        self.navigate("controls")

    def edit_on_phone(self, app):
        phone, view = self.phone, self.show_phone()
        self.jobs.submit(
            "edit-launch-" + phone.identity,
            lambda: phone.launch(app.component),
            failed=self.message,
        )

        def ready(attempt=0):
            if view.current_app == app.package and view.canvas.frame:
                view.edit_controls(app.package)
            elif attempt < 30:
                QTimer.singleShot(200, lambda: ready(attempt + 1))
            else:
                self.message("The game did not become ready for its control editor.")

        QTimer.singleShot(250, ready)

    def export_controls(self, identity):
        from .core import atomic_bytes

        file, _filter = QFileDialog.getSaveFileName(
            self, self.tr("Export controls"), identity + ".veloxcontrols", "Velox (*.veloxcontrols)"
        )
        if file:
            try:
                atomic_bytes(Path(file), Controls(self.phone).exported(identity))
            except (VeloxError, OSError) as error:
                self.message(str(error))

    def import_controls(self, identity):
        file, _filter = QFileDialog.getOpenFileName(
            self, self.tr("Import controls"), filter="Velox (*.veloxcontrols)"
        )
        if not file:
            return
        phone, source = self.phone, Path(file)
        try:
            imported, preset, profiles = Controls.inspect(source)
            if imported != identity:
                raise VeloxError("This controls file belongs to another game.")
            text = (
                imported
                + "\n"
                + str(sum(len(profile.bindings) for profile in profiles))
                + " "
                + self.tr("bindings")
                + "\n\n"
                + self.tr(
                    "Replace both orientations and this game's preset? You can undo this import."
                )
            )
            if (
                QMessageBox.question(self, self.tr("Import controls"), text)
                != QMessageBox.StandardButton.Yes
            ):
                return
            Controls(phone).import_layout(source, identity)
            view = self.views.get(phone.identity)
            if view:
                view.reload_controls()
            self.navigate("controls")
        except (VeloxError, OSError) as error:
            self.message(str(error))

    def undo_controls(self):
        try:
            Controls(self.phone).undo()
            view = self.views.get(self.phone.identity)
            if view:
                view.reload_controls()
            self.navigate("controls")
        except (VeloxError, OSError) as error:
            self.message(str(error))

    def save_preset_value(self, identity, key, value):
        try:
            controls = Controls(self.phone)
            preset = controls.preset(identity)
            setattr(preset, key, value)
            controls.save_preset(identity, preset)
            view = self.views.get(self.phone.identity)
            if view and view.current_app == identity:
                self.game_foreground(self.phone, identity, force=True)
        except (VeloxError, OSError) as error:
            self.message(str(error))

    def page_display(self):
        page, body = scroll_page(
            self.tr("Display"), self.tr("Screen, rotation and Android navigation.")
        )
        box = card(
            body, self.tr("Phone resolution"), self.tr("These choices apply to the next boot.")
        )
        box.addWidget(
            combo(
                [
                    (f"{name} · {w} × {h}", index)
                    for index, (name, w, h, _dpi) in enumerate(DISPLAYS)
                ],
                self.settings.display,
                lambda value: self.preference("display", value),
            )
        )
        box.addWidget(
            combo(
                [(f"{rate} Hz", rate) for rate in RATES],
                self.settings.rate,
                lambda value: self.preference("rate", value, live=True),
            )
        )
        toggle(
            body,
            self.tr("Automatic rotation"),
            self.tr("The phone follows your game's portrait or landscape orientation."),
            self.settings.auto_rotate,
            lambda value: self.preference("auto_rotate", value, live=True),
        )
        box = card(body, self.tr("Android navigation"))
        box.addWidget(
            combo(
                [
                    (self.tr("Back · Home · Recents"), 0),
                    (self.tr("Recents · Home · Back"), 1),
                    (self.tr("Gestures"), 2),
                ],
                self.settings.navigation,
                lambda value: self.preference("navigation", value, live=True),
            )
        )
        notice = getattr(self.phone, "navigation_notice", "")
        if notice:
            box.addWidget(label(self.tr(notice), "subtitle", True))
        box.addWidget(
            label(
                self.tr("Reversed native buttons require Root and a compatible Android launcher."),
                "subtitle",
                True,
            )
        )
        box = card(body, self.tr("Desktop size"))
        for title, key in (("Velox interface", "ui_percent"), ("Phone view", "phone_percent")):
            box.addWidget(label(self.tr(title), "subtitle"))
            values = (
                (75, 100, 110, 125, 150, 175, 200)
                if key == "ui_percent"
                else (25, 33, 40, 50, 67, 75, 100)
            )
            box.addWidget(
                combo(
                    [(self.tr("Auto"), 0)] + [(f"{value}%", value) for value in values],
                    getattr(self.settings, key),
                    lambda value, key=key: self.preference(key, value),
                )
            )
        body.addStretch()
        return page

    def page_performance(self):
        page, body = scroll_page(
            self.tr("Performance"), self.tr("How much of this PC the phone may use.")
        )
        hardware = self.hub.hardware
        box = card(
            body,
            self.tr("Detected on this PC"),
            f"{hardware.cpu} · {hardware.threads} CPU · {hardware.memory} GB · {hardware.gpu} · {hardware.width} × {hardware.height} · {hardware.refresh} Hz",
        )
        box.addWidget(button(self.tr("Use recommended"), self.use_recommended))
        for title, choices, key, limit in (
            ("CPU CORES", CPU_CHOICES, "cores", hardware.cpu_limit),
            ("MEMORY (GB)", RAM_CHOICES, "memory", hardware.memory_limit),
        ):
            box = card(body, self.tr(title))
            grid = QGridLayout()
            box.addLayout(grid)
            effective = allocations(self.settings, hardware)[0 if key == "cores" else 1]
            for index, value in enumerate(choices):
                title = (
                    self.tr("Auto")
                    if not value
                    else memory_label(value, self.settings.language)
                    if key == "memory"
                    else str(value)
                )
                choice = button(
                    title,
                    lambda value=value, key=key: (
                        self.preference(key, value),
                        self.navigate("performance"),
                    ),
                )
                choice.setCheckable(True)
                choice.setChecked((effective if getattr(self.settings, key) else 0) == value)
                choice.setEnabled(not value or value <= limit)
                grid.addWidget(choice, index // 6, index % 6)
        box = card(body, self.tr("Graphics"))
        box.addWidget(
            combo(
                [
                    (self.tr("Auto"), "auto"),
                    (self.tr("GPU") + " · " + hardware.gpu, "host"),
                    (self.tr("Software"), "software"),
                ],
                self.settings.gpu,
                lambda value: self.preference("gpu", value),
            )
        )
        toggle(
            body,
            self.tr("Performance boost"),
            self.tr(
                "Use the installed GameMode service while this phone is focused. Standard scheduling is used when unavailable."
            ),
            self.settings.boost,
            lambda value: (self.preference("boost", value), self.update_native(self.phone)),
        )
        box = card(
            body,
            self.tr("Background frame limit"),
            self.tr(
                "Reduce Android's refresh rate while the phone is in the background. Your foreground setting is restored automatically."
            ),
        )
        box.addWidget(
            combo(
                [(self.tr("Unchanged"), 0)] + [(f"{rate} Hz", rate) for rate in (15, 30, 60)],
                self.settings.background_rate,
                lambda value: (
                    self.preference("background_rate", value),
                    self.update_native(self.phone),
                ),
            )
        )
        toggle(
            body,
            self.tr("Mute in background"),
            self.tr(
                "Mute only this phone's desktop audio streams. Android volume and other apps keep their own settings."
            ),
            self.settings.background_mute,
            lambda value: (
                self.preference("background_mute", value),
                self.update_native(self.phone),
            ),
        )
        toggle(
            body,
            self.tr("Snappy animations"),
            self.tr("Faster screen transitions"),
            self.settings.snappy,
            lambda value: self.preference("snappy", value, live=True),
        )
        toggle(
            body,
            self.tr("Instant start"),
            self.tr("Quick boot may be unavailable after Vulkan games."),
            self.settings.quick_boot,
            lambda value: self.preference("quick_boot", value),
        )
        body.addWidget(
            label(
                self.tr("Unavailable choices are greyed out. Memory limits leave room for Linux."),
                "subtitle",
                True,
            )
        )
        body.addStretch()
        return page

    def use_recommended(self):
        hardware = self.hub.hardware
        if self.phone.alive():
            record = self.phone.runtime()
            if (
                record.get("graphics") == "software"
                and Settings.from_dict(record["settings"]).gpu != "software"
            ):
                hardware = dataclasses.replace(hardware, software=True)
        plan = self.hub.recommendations(self.phone.identity, hardware)
        for key in ("display", "rate", "cores", "memory", "gpu", "snappy", "quick_boot"):
            setattr(self.settings, key, getattr(plan, key))
        self.hub.paths.save(self.phone.identity, self.settings)
        self.navigate("performance")

    def page_look(self):
        page, body = scroll_page(
            self.tr("Make it yours"),
            self.tr("Choose a wallpaper now. It will be applied when the phone starts."),
        )
        box = card(body, self.tr("Wallpaper"))
        row(box, button(self.tr("Add pictures"), self.add_pictures))
        gallery, grid = Wallpaper(self.phone).gallery(), QGridLayout()
        box.addLayout(grid)
        pages = max(1, math.ceil(len(gallery) / 6))
        self.wall_page = min(self.wall_page, pages - 1)
        generation = self.page_generation
        for index, choice in enumerate(gallery[self.wall_page * 6 : self.wall_page * 6 + 6]):
            tile = QFrame()
            column = QVBoxLayout(tile)
            column.setContentsMargins(2, 2, 2, 2)
            widget = QPushButton()
            widget.setFixedSize(round(126 * self.ui_factor), round(188 * self.ui_factor))
            widget.setIconSize(QSize(round(114 * self.ui_factor), round(176 * self.ui_factor)))
            widget.setAccessibleName(choice["name"])
            widget.setToolTip(choice["name"])
            widget.clicked.connect(
                lambda _checked=False, choice=choice: self.choose_wallpaper(choice)
            )
            column.addWidget(widget, 0, Qt.AlignmentFlag.AlignHCenter)
            caption = label(choice["name"], wrap=True)
            caption.setMaximumWidth(round(180 * self.ui_factor))
            caption.setAlignment(Qt.AlignmentFlag.AlignCenter)
            column.addWidget(caption, 0, Qt.AlignmentFlag.AlignHCenter)
            grid.addWidget(tile, index // 3, index % 3)

            def thumbnail(choice=choice):
                if generation != self.page_generation:
                    return None
                return (
                    builtin(choice["builtin"], 180, 320, 0)
                    if "builtin" in choice
                    else fitted(picture(Path(choice["file"])), 180, 320, 0)
                )

            def ready(image, target=widget):
                if generation == self.page_generation and image is not None:
                    from PySide6.QtGui import QIcon

                    target.setIcon(QIcon(QPixmap.fromImage(image)))

            self.jobs.submit(
                "wall-thumb-" + str(generation) + "-" + str(index), thumbnail, ready, priority=-2
            )

        def turn(delta):
            self.wall_page += delta
            self.navigate("look")

        previous = button(self.tr("Previous"), lambda: turn(-1))
        previous.setEnabled(self.wall_page > 0)
        following = button(self.tr("Next"), lambda: turn(1))
        following.setEnabled(self.wall_page + 1 < pages)
        row(box, label(f"{self.wall_page + 1} / {pages}", "subtitle"), None, previous, following)
        box = card(body, self.tr("Boot screen"))
        box.addWidget(
            combo(
                [
                    (name, index)
                    for index, name in enumerate(
                        (
                            "Orbit",
                            "Hyperdrive",
                            "Neon Grid",
                            "Terminal",
                            "Minimal",
                            self.tr("Random"),
                        )
                    )
                ],
                self.settings.boot_style,
                lambda value: self.preference("boot_style", value),
            )
        )
        box.addWidget(button(self.tr("Preview"), self.preview_boot))
        toggle(
            body,
            self.tr("Phone frame"),
            "",
            self.settings.frame,
            lambda value: self.preference("frame", value),
        )
        box = card(body, self.tr("Language"))
        box.addWidget(
            combo(
                [("English", "en"), ("Deutsch", "de")],
                self.settings.language,
                lambda value: self.preference("language", value, live=True),
            )
        )
        body.addStretch()
        return page

    def choose_wallpaper(self, choice):
        phone, settings = self.phone, dataclasses.replace(self.settings)
        self.pending_wallpapers[phone.identity] = (phone, choice, settings)
        self.drain_wallpaper(phone.identity)

    def drain_wallpaper(self, identity):
        if identity not in self.pending_wallpapers or "wallpaper-" + identity in self.jobs.active:
            return
        phone, choice, settings = self.pending_wallpapers.pop(identity)

        def finished(_value):
            if identity in self.pending_wallpapers:
                self.drain_wallpaper(identity)
            else:
                self.message(
                    "Wallpaper applied."
                    if phone.alive() and phone.state == "running"
                    else "Wallpaper queued for next boot."
                )

        def failed(message):
            self.message(message)
            self.drain_wallpaper(identity)

        self.jobs.submit(
            "wallpaper-" + identity,
            lambda: Wallpaper(phone).select(choice, settings),
            finished,
            failed,
        )

    def add_pictures(self):
        files, _filter = QFileDialog.getOpenFileNames(
            self, self.tr("Add pictures"), filter="PNG / JPG (*.png *.jpg *.jpeg)"
        )
        phone = self.phone
        if files:
            self.jobs.submit(
                "add-pictures",
                lambda: Wallpaper(phone).add(list(map(Path, files))),
                lambda _value: self.navigate("look"),
                self.message,
            )

    def preview_boot(self):
        dialog = QDialog(self)
        dialog.setWindowTitle(self.tr("Boot screen"))
        dialog.resize(380, 680)
        layout = QVBoxLayout(dialog)
        layout.setContentsMargins(0, 0, 0, 0)
        screen = BootScreen(dialog, self.settings.boot_style)
        screen.progress = 0.55
        layout.addWidget(screen)
        dialog.exec()

    def page_phone(self):
        page, body = scroll_page(self.tr("Phone"), self.tr("Phone controls"))
        if self.recovery.progress_card(body):
            body.addStretch()
            return page
        from .rooting import Root

        root = Root(self.phone)
        available = root.available()
        alive = self.phone.alive()
        live_root = None
        if alive:
            try:
                live_root = self.phone.runtime().get("settings", {}).get("root")
            except (OSError, VeloxError):
                pass
        check = toggle(
            body,
            self.tr("Root (Magisk)"),
            self.tr("Real root for testing. Banking apps and some games refuse rooted phones."),
            self.settings.root,
            lambda value: self.preference("root", value),
        )
        check.setEnabled(available or self.settings.root)
        box = card(
            body,
            self.tr("Private Root image"),
            self.tr(
                "Start this phone without Root to prepare it. The shared Android image stays unchanged."
            ),
        )
        prepare = button(
            self.tr("Rebuild Root image" if available else "Prepare Root"), self.prepare_root
        )
        prepare.setEnabled(alive and self.phone.state == "running" and live_root is False)
        box.addWidget(prepare)
        if available:
            manager = button(
                self.tr("Open Magisk"),
                self.open_root_manager,
            )
            manager.setEnabled(alive and self.phone.state == "running")
            box.addWidget(manager)
            box.addWidget(
                label(
                    self.tr(
                        "After a rooted boot, open Magisk and finish its additional setup. In Superuser, tap + and enable Shell."
                    ),
                    "subtitle",
                    True,
                )
            )
        if (
            alive
            and self.phone.state == "running"
            and live_root is not None
            and live_root != self.settings.root
        ):
            box.addWidget(button(self.tr("Restart phone"), self.restart_phone, True))
        box = card(body)
        row(
            box,
            button("Velox Home", lambda: self.guest_action("home")),
            button("Velox Settings", lambda: self.guest_action("settings")),
        )
        box = card(body, self.tr("Phone number"))
        field = QLineEdit(self.settings.number)
        field.setPlaceholderText(self.tr("Emulator default"))

        def number():
            try:
                value = dataclasses.replace(self.settings, number=field.text().strip())
                Settings.from_dict(value.as_dict())
                self.preference("number", value.number)
            except VeloxError as error:
                self.message(str(error))
                field.setText(self.settings.number)

        field.editingFinished.connect(number)
        box.addWidget(field)
        toggle(
            body,
            self.tr("Show taps"),
            self.tr("Shows where you touch the screen, handy for recordings and tutorials."),
            self.settings.show_touches,
            lambda value: self.preference("show_touches", value, live=True),
        )
        toggle(
            body,
            self.tr("Clipboard shortcuts"),
            self.tr("Copy and paste text between this PC and the focused phone."),
            self.settings.clipboard_shortcuts,
            lambda value: self.preference("clipboard_shortcuts", value),
        )
        toggle(
            body,
            self.tr("Start phone automatically"),
            "",
            self.settings.auto_start,
            lambda value: self.preference("auto_start", value),
        )
        box = card(body, self.tr("Phone files"), str(self.phone.home))
        box.addWidget(
            button(
                self.tr("Open folder"),
                lambda: QDesktopServices.openUrl(QUrl.fromLocalFile(str(self.phone.folder))),
            )
        )
        body.addStretch()
        return page

    def prepare_root(self):
        from .rooting import Root

        phone = self.phone
        if not self.recovery.confirm(
            "Prepare Root",
            self.tr(
                "Root changes Android's security model. Banking apps and some games may refuse rooted phones. Download verified Magisk tools and prepare a private Root image for this phone?"
            ),
            "Prepare Root",
        ):
            return
        self.recovery.task(
            phone,
            "Preparing the private Root image",
            lambda manager, report: Root(phone, manager.cancel).prepare(report),
            cold=False,
            exclusive=True,
            page="phone",
        )

    def open_root_manager(self):
        from .rooting import Root

        phone = self.phone
        self.jobs.submit(
            "magisk-" + phone.identity,
            Root(phone).open_manager,
            lambda _value: self.show_phone(phone) if not self._closing else None,
            self.message,
        )

    def guest_action(self, action):
        phone = self.phone
        self.jobs.submit(
            "guest-" + phone.identity,
            Experience(phone).open_home if action == "home" else Experience(phone).open_settings,
            failed=self.message,
        )

    def page_system(self):
        page, body = scroll_page(self.tr("System"), self.tr("Native CPU acceleration with KVM"))
        hardware = self.hub.hardware
        box = card(body, self.tr("Live usage"))
        usage = QGridLayout()
        box.addLayout(usage)
        for column, (title, key) in enumerate(
            (("CPU", "cpu"), ("GPU", "gpu"), ("Memory", "memory"), ("VRAM", "vram"))
        ):
            usage.addWidget(label(self.tr(title), "section"), 0, column)
            widget = label("–", "title")
            usage.addWidget(widget, 1, column)
            self.sensor_labels[key] = (widget, "%", 0)
        box = card(body, self.tr("Live sensors"))
        grid = QGridLayout()
        grid.setHorizontalSpacing(18)
        grid.setVerticalSpacing(16)
        box.addLayout(grid)
        for index, (title, key, unit, decimals) in enumerate(
            (
                ("CPU temperature", "cpu_temperature", "°C", 0),
                ("GPU temperature", "gpu_temperature", "°C", 0),
                ("VRAM temperature", "vram_temperature", "°C", 0),
                ("CPU clock", "cpu_clock", "MHz", 0),
                ("GPU clock", "gpu_clock", "MHz", 0),
                ("GPU power", "gpu_power", "W", 0),
                ("CPU fan", "cpu_fan", "RPM", 0),
                ("GPU fan", "gpu_fan", "RPM", 0),
                ("GPU fan speed", "gpu_fan_percent", "%", 0),
                ("Phone CPU", "phone_cpu", "%", 1),
                ("Phone memory", "phone_memory", "GB", 1),
                ("VRAM used", "vram_used", "GB", 1),
            )
        ):
            panel = QWidget()
            panel.setObjectName("clear")
            vertical = QVBoxLayout(panel)
            vertical.setContentsMargins(0, 0, 0, 0)
            vertical.setSpacing(6)
            vertical.addWidget(label(self.tr(title), "section", True))
            widget = label("–")
            vertical.addWidget(widget)
            self.sensor_labels[key] = (widget, unit, decimals)
            grid.addWidget(panel, index // 3, index % 3)
        box.addWidget(
            label(
                self.tr(
                    "Unavailable sensors show –. Readings use native Linux interfaces and the installed GPU driver. No root access or extra sensor driver is required."
                ),
                "subtitle",
                True,
            )
        )
        self.update_sensors(self.sensor_sample)
        self.refresh_sensors()
        card(
            body,
            hardware.distro,
            hardware.cpu + f" · {hardware.threads} CPU · {hardware.memory} GB",
        )
        card(body, self.tr("Graphics driver"), hardware.gpu)
        card(body, self.tr("KVM access"), self.tr("Ready" if hardware.kvm else "Unavailable"))
        if self.phone.alive():
            record = self.phone.runtime()
            details = record.get("graphics_details", {})
            mode = self.tr(
                {"host": "Hardware graphics", "software": "Software graphics"}.get(
                    record.get("graphics"), "Unknown"
                )
            )
            card(
                body,
                self.tr("Active Android renderer"),
                mode + (" · " + details["renderer"] if details.get("renderer") else ""),
            )
            if (
                record.get("graphics") == "software"
                and Settings.from_dict(record["settings"]).gpu != "software"
            ):
                card(
                    body,
                    self.tr("Software fallback"),
                    self.tr(
                        "Android selected software graphics for compatibility with this driver. Update your host graphics driver or try GPU mode; restart to apply."
                    ),
                )
                if record["settings"]["gpu"] == "auto":
                    card(
                        body,
                        self.tr("Temporary frame limit"),
                        self.tr(
                            "Auto mode uses 30 Hz during a software fallback. Your preferred foreground rate is kept for the next hardware-accelerated boot."
                        ),
                    )
        box = card(body, self.tr("Startup diagnostics"))
        box.addWidget(
            button(
                self.tr("View diagnostics"),
                lambda: text_dialog(
                    self, self.tr("Startup diagnostics"), self.phone.diagnostics(), self.tr
                ).exec(),
            )
        )
        phone = self.phone
        box.addWidget(
            button(
                self.tr("Repair startup"),
                lambda: self.jobs.submit(
                    "repair-" + phone.identity,
                    phone.repair_startup,
                    lambda _value: self.message("Ready"),
                    self.message,
                ),
            )
        )
        body.addStretch()
        return page

    def page_updates(self):
        page, body = scroll_page(self.tr("Updates"), self.tr("Velox") + " " + current_version())
        busy = any(key.startswith("update-") for key in self.jobs.active)
        box = card(
            body,
            self.tr("Keep Velox up to date"),
            self.tr(
                "Linux updates are verified with Velox's release-signing key before installation."
            ),
        )
        box.addWidget(
            label(
                self.tr(self.update_status or "Check for a new release when you are ready."),
                "subtitle",
                True,
            )
        )
        check = button(self.tr("Check for updates"), self.check_updates, True)
        check.setEnabled(not busy and self.updated_image is None)
        box.addWidget(check)
        if self.update_release and not self.updated_image:
            release = self.update_release
            box = card(body, self.tr("New version") + " · " + release.version, release.notes)
            install = button(self.tr("Download and update"), self.install_update, True)
            install.setEnabled(not busy and bool(os.environ.get("APPIMAGE")))
            box.addWidget(install)
        if busy:
            progress = QProgressBar()
            progress.setRange(0, 100)
            progress.setValue(round(self.update_fraction * 100))
            box.addWidget(progress)
            self.update_progress_widget = progress
            if "update-install" in self.jobs.active:
                box.addWidget(button(self.tr("Cancel"), self.updater.cancel.set))
        if self.updated_image:
            current, previous = self.updated_image
            box = card(
                body,
                self.tr("Update ready"),
                self.tr("The previous AppImage is retained for recovery.") + "\n" + str(previous),
            )
            restart = button(self.tr("Restart Velox"), self.restart_velox, True)
            restart.setEnabled(not self.jobs.active)
            box.addWidget(restart)
        if not os.environ.get("APPIMAGE"):
            card(
                body,
                self.tr("AppImage updates"),
                self.tr("Open the packaged AppImage to use automatic updates."),
            )
        box = card(
            body,
            self.tr("Desktop integration"),
            self.tr(
                "Add Velox to this account's application menu. Keep the AppImage in its current location afterwards."
            ),
        )
        shortcut = button(self.tr("Add to application menu"), self.install_shortcut)
        shortcut.setEnabled(bool(os.environ.get("APPIMAGE")) and not busy)
        box.addWidget(shortcut)
        box.addWidget(
            button(
                self.tr("GitHub releases"),
                lambda: QDesktopServices.openUrl(
                    QUrl("https://github.com/NeTriXoN-BoT/velox/releases/latest")
                ),
            )
        )
        body.addStretch()
        return page

    def check_updates(self):
        from .updates import check

        self.update_status = "Checking for updates…"

        def ready(release):
            self.update_release = release
            self.update_status = (
                "A new Velox release is available." if release else "Velox is already up to date."
            )
            if not self._closing:
                self.navigate("updates")

        def failed(message):
            self.update_status = message
            self.message(message)
            if not self._closing:
                self.navigate("updates")

        if self.jobs.submit("update-check", check, ready, failed):
            self.navigate("updates")

    def install_update(self):
        release = self.update_release
        if not release or not self.recovery.confirm(
            "Update Velox",
            self.tr(
                "Download the verified AppImage and retain the current version for recovery? Running phones keep their apps and saves."
            ),
            "Download and update",
        ):
            return
        self.updater.cancel.clear()
        self.update_progress_widget = None
        self.update_status, self.update_fraction = "Downloading update…", 0.0

        def progress(message, fraction):
            self.update_status, self.update_fraction = message, fraction
            self.message(message)
            if self.page_name == "updates" and self.update_progress_widget:
                self.update_progress_widget.setValue(round(100 * fraction))

        def ready(value):
            self.updated_image = value
            self.update_status = "Update installed. Restart Velox when ready."
            if not self._closing:
                self.navigate("updates")

        def failed(message):
            self.update_status = message
            self.message(message)
            if not self._closing:
                self.navigate("updates")

        if self.jobs.submit(
            "update-install",
            lambda report: self.updater.apply(release, report),
            ready,
            failed,
            progress,
        ):
            self.navigate("updates")

    def restart_velox(self):
        if self.updated_image and not self.jobs.active:
            QApplication.instance().setProperty("veloxRestart", str(self.updated_image[0]))
            self.close()

    def install_shortcut(self):
        from .desktop import install

        self.jobs.submit(
            "desktop-shortcut",
            lambda: install(self.hub.paths),
            lambda _value: self.message("Velox was added to your application menu."),
            self.message,
        )

    def page_recovery(self):
        return self.recovery.build()

    def refresh_sensors(self):
        phone, hardware = self.phone, self.hub.hardware
        self.jobs.submit(
            "telemetry",
            lambda: self.telemetry.sample(phone, hardware),
            self.update_sensors,
            self.message,
        )

    def update_sensors(self, sample):
        self.sensor_sample = sample
        if self._closing or self.page_name != "system":
            return
        import time

        current = sample.phone == self.phone.identity and time.monotonic() - sample.captured < 5
        for key, (widget, unit, decimals) in self.sensor_labels.items():
            value = getattr(sample, key) if current else None
            formatted = f"{value:.{decimals}f}" if value is not None else ""
            if self.settings.language == "de":
                formatted = formatted.replace(".", ",")
            widget.setText(formatted + ("" if unit == "%" else " ") + unit if formatted else "–")

    def closeEvent(self, event):
        self._closing = True
        self.timer.stop()
        self.hub.setup.cancel.set()
        self.updater.cancel.set()
        self.transfer_cancel.set()
        self.recovery.cancel_all()
        for phone in self.hub._phones.values():
            if phone.state == "starting":
                phone.cancel.set()
        for view in self.views.values():
            view.hide()
        for view in self.views.values():
            view.mouse_aim.destroy()
        for view in self.views.values():
            view.pad.close()
        for session in self.native_sessions.values():
            session.close()
        self.hide()
        event.ignore()

        def finished():
            if (
                self.jobs.active
                or any(
                    view.jobs.active or not view.pad.closed.is_set() for view in self.views.values()
                )
                or any(not session.closed.is_set() for session in self.native_sessions.values())
            ):
                QTimer.singleShot(100, finished)
                return
            if self.hub.registration:
                self.hub.registration.close()
            self.telemetry.close()
            QApplication.quit()

        QTimer.singleShot(100, finished)
