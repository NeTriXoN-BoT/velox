"""Velox Vault's asynchronous, review-before-activation desktop view."""

from __future__ import annotations

import datetime
import time
from pathlib import Path

from PySide6.QtCore import Qt, QUrl
from PySide6.QtGui import QDesktopServices
from PySide6.QtWidgets import QFileDialog, QMessageBox, QProgressBar

from .core import Settings, VeloxError
from .experience import Experience
from .recovery import Recovery
from .schedules import Schedule
from .widgets import button, card, combo, label, row, scroll_page


class RecoveryUI:
    def __init__(self, window):
        self.window = window
        self.active = {}
        self.progress = {}
        self.rows = {}
        self.page = 0
        self.widgets = None
        self.next_schedule_scan = time.monotonic() + 30

    def busy(self, identity):
        operation = self.active.get(identity)
        return bool(operation and operation[1])

    def cancel_all(self):
        for manager, _cold in self.active.values():
            manager.cancel.set()

    def task(
        self, phone, title, action, *, cold=True, background=False, page="recovery", exclusive=False
    ):
        window = self.window
        identity = phone.identity
        if identity in self.active:
            window.message("This phone is busy with a maintenance action.")
            return
        if cold and (phone.alive() or phone.state != "stopped"):
            window.message("Stop this phone before this maintenance action.")
            return
        if any(
            not key.startswith(("wall-thumb-", "recovery-list-")) and key != "telemetry"
            for key in window.jobs.active
        ):
            window.message("Finish the current phone action before starting maintenance.")
            return
        try:
            manager = Recovery(phone)
        except (OSError, VeloxError) as error:
            window.message(str(error))
            return
        self.active[identity] = manager, cold or exclusive
        self.progress[identity] = title, 0.0

        def progress(message, fraction):
            self.progress[identity] = message, fraction
            if (
                self.widgets
                and window.phone.identity == identity
                and window.page_name in ("recovery", "instances", "phone")
            ):
                self.widgets[0].setText(window.tr(message))
                self.widgets[1].setValue(round(fraction * 1000))

        def finished(result=None, error=None):
            self.active.pop(identity, None)
            self.progress.pop(identity, None)
            self.rows.clear()
            if window._closing:
                return
            if (cold or exclusive) and window.phone.identity == identity:
                try:
                    window.settings = window.hub.paths.settings(identity)
                except (OSError, VeloxError):
                    window.settings = Settings()
                window.tr.language = window.settings.language
                try:
                    window.apps = Experience(phone).library()
                except (OSError, VeloxError):
                    window.apps = []
                window.build_sidebar()
            if error or not background:
                window.message(error or "Recovery action completed.")
            if window.phone.identity == identity or window.page_name in ("instances", "recovery"):
                window.navigate(window.page_name)
            window.tick()

        submitted = window.jobs.submit(
            "recovery-" + identity,
            lambda report: action(manager, report),
            lambda value: finished(value),
            lambda message: finished(error=message),
            progress,
            priority=-1,
        )
        if not submitted:
            self.active.pop(identity, None)
            self.progress.pop(identity, None)
            return
        if window.phone.identity == identity and not background:
            window.navigate(page)
        window.tick()

    def progress_card(self, body):
        window = self.window
        identity = window.phone.identity
        if identity not in self.active:
            return False
        manager, _cold = self.active[identity]
        message, fraction = self.progress[identity]
        box = card(body, window.tr("Phone maintenance"))
        status = label(window.tr(message), "subtitle", True)
        bar = QProgressBar()
        bar.setRange(0, 1000)
        bar.setTextVisible(False)
        bar.setValue(round(fraction * 1000))
        box.addWidget(status)
        box.addWidget(bar)
        box.addWidget(button(window.tr("Cancel safely"), manager.cancel.set))
        self.widgets = status, bar
        return True

    def schedule_tick(self):
        window = self.window
        if time.monotonic() < self.next_schedule_scan:
            return
        self.next_schedule_scan = time.monotonic() + 60
        if self.active or any(
            not key.startswith(("wall-thumb-", "recovery-list-")) and key != "telemetry"
            for key in window.jobs.active
        ):
            return

        def due():
            phones = [
                window.hub.phone(item["id"]) for item in window.hub.list() if not item.get("error")
            ]
            # Backups are disk-intensive. Wait until nobody is playing.
            if any(phone.state != "stopped" or phone.alive() for phone in phones):
                return None
            for phone in phones:
                if Schedule(phone).due():
                    return phone.identity
            return None

        def ready(identity):
            if not identity or window._closing or self.active:
                return
            if any(
                phone.alive() or phone.state != "stopped" for phone in window.hub._phones.values()
            ):
                return
            phone = window.hub.phone(identity)

            def run(manager, report):
                schedule = Schedule(phone)
                schedule.vault = manager.vault
                schedule.cancel = manager.cancel
                return schedule.run(report)

            self.task(phone, "Creating scheduled recovery point", run, background=True)

        window.jobs.submit("recovery-schedule-scan", due, ready, window.message, priority=-1)

    def schedule_card(self, body, phone, working):
        window, tr = self.window, self.window.tr
        box = card(
            body,
            tr("Backup schedule"),
            tr(
                "Runs only while Velox is open and all phones are stopped. Never deletes recovery points."
            ),
        )
        try:
            policy = Schedule(phone).read()
        except (OSError, VeloxError) as error:
            box.addWidget(label(tr(str(error)), "subtitle", True))
            return

        def change(key, value):
            try:
                current = Schedule(phone).read()
                current[key] = value
                Schedule(phone).save(
                    days=current["days"], keep=current["keep"], limit_gb=current["limit_gb"]
                )
            except (OSError, VeloxError) as error:
                window.message(str(error))
            self.next_schedule_scan = time.monotonic() + 3
            window.navigate("recovery")

        frequency = combo(
            [(tr("Off"), 0), (tr("Daily"), 1), (tr("Weekly"), 7)],
            policy["days"],
            lambda value: change("days", value),
        )
        frequency.setEnabled(not working)
        box.addWidget(frequency)
        box.addWidget(label(tr("Maximum scheduled recovery points"), "subtitle", True))
        count = combo(
            [(str(value), value) for value in sorted({1, 3, 5, 10, 20, policy["keep"]})],
            policy["keep"],
            lambda value: change("keep", value),
        )
        count.setEnabled(not working)
        box.addWidget(count)
        box.addWidget(label(tr("Scheduled storage limit (GB)"), "subtitle", True))
        size = combo(
            [
                (str(value) + " GB", value)
                for value in sorted({5, 10, 25, 50, 100, 250, 500, policy["limit_gb"]})
            ],
            policy["limit_gb"],
            lambda value: change("limit_gb", value),
        )
        size.setEnabled(not working)
        box.addWidget(size)
        box.addWidget(
            label(
                tr(
                    "At either limit, scheduling pauses. Move old scheduled backups out of the vault to make room. Manual backups are kept separately."
                ),
                "subtitle",
                True,
            )
        )
        if policy["last"]:
            box.addWidget(
                label(
                    tr("Last recovery point") + ": " + self.date(policy["last"]), "subtitle", True
                )
            )
        if policy["status"]:
            box.addWidget(label(tr(policy["status"]), "subtitle", True))
        if policy["paused"]:
            resume = button(tr("Resume schedule"), lambda: change("days", policy["days"]))
            resume.setEnabled(not working and bool(policy["days"]))
            box.addWidget(resume)

    def refresh(self, phone):
        window = self.window
        identity = phone.identity

        def loaded(value):
            self.rows[identity] = value
            if (
                not window._closing
                and window.phone.identity == identity
                and window.page_name == "recovery"
            ):
                window.navigate("recovery")

        def failed(message):
            self.rows[identity] = {"backups": [], "prepared": [], "error": message}
            if (
                not window._closing
                and window.phone.identity == identity
                and window.page_name == "recovery"
            ):
                window.navigate("recovery")

        def listing():
            manager = Recovery(phone)
            return {"backups": manager.vault.list(), "prepared": manager.list(), "error": ""}

        window.jobs.submit("recovery-list-" + identity, listing, loaded, failed)

    def choose(self):
        window = self.window
        folder = QFileDialog.getExistingDirectory(window, window.tr("Choose a Velox backup folder"))
        if folder:
            self.task(
                window.phone,
                "Preparing recovered phone",
                lambda manager, report: manager.prepare(Path(folder), report),
            )

    def confirm(self, title, message, accept):
        window = self.window
        dialog = QMessageBox(window)
        dialog.setWindowTitle(window.tr(title))
        dialog.setTextFormat(Qt.TextFormat.PlainText)
        dialog.setText(message)
        dialog.setStandardButtons(QMessageBox.StandardButton.Ok | QMessageBox.StandardButton.Cancel)
        dialog.button(QMessageBox.StandardButton.Ok).setText(window.tr(accept))
        dialog.button(QMessageBox.StandardButton.Cancel).setText(window.tr("Cancel"))
        dialog.setDefaultButton(QMessageBox.StandardButton.Cancel)
        return dialog.exec() == QMessageBox.StandardButton.Ok

    def activate(self, phone, value):
        window = self.window
        message = (
            window.tr("Restore this prepared backup to the selected phone?")
            + "\n\n"
            + value["label"]
            + "\n"
            + phone.metadata["name"]
            + "\n\n"
            + window.tr(
                "Apps, saves and settings will switch to this recovery point. Your current phone is kept for Undo."
            )
        )
        if value.get("rooted"):
            message += "\n\n" + window.tr("This recovery point enables Root (Magisk).")
        if self.confirm("Restore backup", message, "Restore backup"):
            self.task(
                phone,
                "Verifying prepared recovery",
                lambda manager, report: manager.activate(value["id"], report),
            )

    def undo(self, phone):
        message = self.window.tr(
            "Return to the phone you had before recovery? The recovered copy and its later changes are also retained."
        )
        if self.confirm("Undo recovery", message, "Return to previous phone"):
            self.task(
                phone, "Returning to previous phone", lambda manager, report: manager.undo(report)
            )

    def build(self):
        window = self.window
        phone, tr = window.phone, window.tr
        identity = phone.identity
        self.widgets = None
        page, body = scroll_page(
            tr("Velox Vault"),
            tr("Private recovery points. Your previous phone stays available for Undo."),
        )
        working = identity in self.active
        cold = phone.state == "stopped" and not phone.alive() and not working
        box = card(
            body,
            phone.metadata["name"],
            tr("Backups contain private app data and stay in your account's recovery folder."),
        )
        create = button(
            tr("Create recovery point"),
            lambda: self.task(
                phone,
                "Creating recovery point",
                lambda manager, report: manager.vault.create(report),
            ),
            True,
        )
        create.setEnabled(cold and phone.avd.is_dir() and window.hub.setup.installed())
        choose = button(tr("Choose backup"), self.choose)
        choose.setEnabled(cold and window.hub.setup.installed())
        row(box, create, choose)
        row(
            box,
            button(
                tr("Open recovery folder"),
                lambda: QDesktopServices.openUrl(QUrl.fromLocalFile(str(window.hub.paths.vault))),
            ),
            button(tr("Refresh"), lambda: self.refresh(phone)),
        )
        if working:
            manager, _cold = self.active[identity]
            message, fraction = self.progress[identity]
            box = card(body, tr("Recovery in progress"))
            status = label(tr(message), "subtitle", True)
            bar = QProgressBar()
            bar.setRange(0, 1000)
            bar.setTextVisible(False)
            bar.setValue(round(fraction * 1000))
            box.addWidget(status)
            box.addWidget(bar)
            box.addWidget(button(tr("Cancel safely"), manager.cancel.set))
            self.widgets = status, bar
        if phone.metadata.get("recovery"):
            box = card(
                body, tr("A way back"), tr("The phone from before recovery is retained separately.")
            )
            undo = button(tr("Undo recovery"), lambda: self.undo(phone))
            undo.setEnabled(cold)
            box.addWidget(undo)
        self.schedule_card(body, phone, working)
        values = self.rows.get(identity)
        if values is None:
            card(body, tr("Loading recovery points…"))
            self.refresh(phone)
        else:
            if values["error"]:
                card(body, tr("Recovery needs attention"), values["error"])
            for value in values["prepared"]:
                if value["phase"] != "prepared":
                    continue
                box = card(
                    body,
                    tr("Ready for review"),
                    value["label"] + " · " + self.date(value["created"]),
                )
                apply = button(
                    tr("Review and restore"), lambda value=value: self.activate(phone, value), True
                )
                apply.setEnabled(cold)
                check = button(
                    tr("Verify"),
                    lambda value=value: self.task(
                        phone,
                        "Verifying prepared recovery",
                        lambda manager, report: manager.verify(value["id"], report),
                    ),
                )
                check.setEnabled(cold)
                row(box, apply, check)
            backups = values["backups"]
            if not backups:
                card(
                    body,
                    tr("Your first recovery point"),
                    tr(
                        "Stop the phone after playing, then create a recovery point. It includes apps, saves, controls, wallpapers and settings."
                    ),
                )
            self.page = min(self.page, max(0, (len(backups) - 1) // 4))
            for value in backups[self.page * 4 : self.page * 4 + 4]:
                size = value["bytes"] / 1024**3
                detail = value["error"] or self.date(value["created"]) + f" · {size:.1f} GB"
                box = card(body, value["label"], detail)
                check = button(
                    tr("Verify"),
                    lambda value=value: self.task(
                        phone,
                        "Verifying phone backup",
                        lambda manager, report: manager.vault.verify(value["path"], report),
                        cold=False,
                    ),
                )
                check.setEnabled(not working)
                prepare = button(
                    tr("Prepare recovery"),
                    lambda value=value: self.task(
                        phone,
                        "Preparing recovered phone",
                        lambda manager, report: manager.prepare(value["path"], report),
                    ),
                )
                prepare.setEnabled(cold and not value["error"] and window.hub.setup.installed())
                row(box, check, prepare)
            if len(backups) > 4:

                def turn(delta):
                    self.page += delta
                    window.navigate("recovery")

                previous = button(tr("Previous"), lambda: turn(-1))
                previous.setEnabled(self.page > 0)
                following = button(tr("Next"), lambda: turn(1))
                following.setEnabled((self.page + 1) * 4 < len(backups))
                row(
                    body,
                    previous,
                    label(f"{self.page + 1} / {max(1, (len(backups) + 3) // 4)}"),
                    following,
                )
        if not cold and not working:
            card(
                body,
                tr("Cold recovery"),
                tr(
                    "Stop this phone to create or restore a recovery point. Other phones can keep running."
                ),
            )
        if window.hub.recovery_errors:
            card(
                body,
                tr("Recovery needs attention"),
                "\n".join(list(window.hub.recovery_errors.values())[:3]),
            )
        body.addStretch()
        return page

    def date(self, timestamp):
        date = datetime.datetime.fromtimestamp(timestamp).astimezone()
        return date.strftime(
            "%d.%m.%Y · %H:%M" if self.window.tr.language == "de" else "%Y-%m-%d · %H:%M"
        )
