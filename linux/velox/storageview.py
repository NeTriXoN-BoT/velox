"""Velox's phone library, independent clones and reversible storage actions."""

from __future__ import annotations

from PySide6.QtCore import QUrl
from PySide6.QtGui import QDesktopServices
from PySide6.QtWidgets import QFileDialog, QInputDialog

from .storage import Storage
from .widgets import button, card, label, row, scroll_page


class StorageUI:
    def __init__(self, window):
        self.window = window
        self.details = None
        self.usage = {}
        self.removed = None
        self.removed_page = 0

    def task(self, phone, title, action, *, cold=True):
        self.usage.pop(phone.identity, None)
        self.removed = None

        def run(manager, report):
            return action(Storage(phone, manager.cancel), report)

        self.window.recovery.task(phone, title, run, cold=cold, page="instances")

    def clone(self, phone):
        window, tr = self.window, self.window.tr
        modes = [
            tr("Full copy — apps, saves and settings"),
            tr("Settings template — fresh Android"),
        ]
        mode, accepted = QInputDialog.getItem(
            window, tr("Clone phone"), tr("What should this copy include?"), modes, 0, False
        )
        if not accepted:
            return
        full = mode == modes[0]
        name, accepted = QInputDialog.getText(
            window,
            tr("Clone phone"),
            tr("Enter a phone name"),
            text=phone.metadata["name"] + " · " + tr("Copy"),
        )
        if not accepted:
            return
        if full and not window.recovery.confirm(
            "Clone phone",
            tr(
                "This creates an independent copy, including signed-in accounts and private app data. Only clone your own phone."
            ),
            "Create copy",
        ):
            return
        self.task(
            phone, "Cloning phone", lambda storage, report: storage.clone(name, report, full=full)
        )

    def remove(self, phone):
        window, tr = self.window, self.window.tr
        if not window.recovery.confirm(
            "Remove phone",
            phone.metadata["name"]
            + "\n\n"
            + tr(
                "The phone is moved to Removed phones. You can restore it later. External storage and backups are retained. No storage space is freed until you delete the retained copy yourself."
            ),
            "Remove phone",
        ):
            return
        if window.phone.identity == phone.identity:
            window.select_phone("main")
        self.details = None
        self.task(phone, "Removing phone", lambda storage, report: storage.remove(report))
        window.navigate("instances")

    def open_details(self, identity):
        self.window.select_phone(identity)
        self.details = identity
        self.window.navigate("instances")

    def relocate(self, phone):
        window, tr = self.window, self.window.tr
        parent = QFileDialog.getExistingDirectory(window, tr("Choose a folder for phone storage"))
        if parent and window.recovery.confirm(
            "Move phone storage",
            phone.metadata["name"]
            + "\n\n"
            + parent
            + "\n\n"
            + tr(
                "Copies and checks Android data before switching folders. The original stays available for Undo. Shared Android tools and backups stay where they are."
            ),
            "Move phone storage",
        ):
            self.task(
                phone,
                "Moving phone storage",
                lambda storage, report: storage.relocate(parent, report),
            )

    def undo_relocation(self, phone):
        window, tr = self.window, self.window.tr
        if window.recovery.confirm(
            "Undo storage move",
            tr(
                "Return to the previous storage copy? Changes made since the move stay in the moved copy, which is also retained."
            ),
            "Return to previous storage",
        ):
            self.task(
                phone,
                "Restoring previous storage",
                lambda storage, report: storage.undo_relocation(report),
            )

    def refresh_usage(self, phone):
        window = self.window

        def done(value):
            self.usage[phone.identity] = value
            if (
                not window._closing
                and window.page_name == "instances"
                and self.details == phone.identity
            ):
                window.navigate("instances")

        window.jobs.submit(
            "storage-usage-" + phone.identity,
            Storage(phone).usage,
            done,
            lambda error: done({"error": error}),
            priority=-1,
        )

    def refresh_removed(self):
        window = self.window

        def done(value):
            self.removed = value
            if not window._closing and window.page_name == "instances" and not self.details:
                window.navigate("instances")

        window.jobs.submit(
            "storage-removed",
            Storage(window.hub.phone()).list_removed,
            done,
            lambda error: done([{"error": error}]),
            priority=-1,
        )

    def build(self):
        window, tr = self.window, self.window.tr
        page, body = scroll_page(
            tr("Your phones"), tr("Every phone has its own apps, saves and settings.")
        )
        if window.recovery.progress_card(body):
            body.addStretch()
            return page
        if self.details:
            self.build_details(body)
        else:
            body.addWidget(button(tr("New phone"), window.create_phone, True))
            for item in window.hub.list():
                box = card(
                    body,
                    item["name"],
                    tr(
                        "Starting…"
                        if item.get("starting")
                        else "Running"
                        if item.get("running")
                        else "Off"
                    ),
                )
                if item.get("error"):
                    box.addWidget(label(tr(item["error"]), "subtitle", True))
                    continue
                row(
                    box,
                    button(tr("Select"), lambda identity=item["id"]: window.select_phone(identity)),
                    button(
                        tr("Show phone" if item.get("running") else "Start phone"),
                        lambda identity=item["id"]: window.start_instance(identity),
                    ),
                    button(tr("Rename"), lambda item=item: window.rename_phone(item)),
                )
                row(
                    box,
                    button(
                        tr("Clone phone"),
                        lambda identity=item["id"]: self.clone(window.hub.phone(identity)),
                    ),
                    button(tr("Storage"), lambda identity=item["id"]: self.open_details(identity)),
                )
            self.build_removed(body)
        body.addStretch()
        return page

    def build_removed(self, body):
        window, tr = self.window, self.window.tr
        box = card(
            body,
            tr("Removed phones"),
            tr(
                "Retained privately for recovery. Backups and relocated storage are kept separately."
            ),
        )
        row(
            box,
            button(
                tr("Open folder"),
                lambda: QDesktopServices.openUrl(
                    QUrl.fromLocalFile(str(window.hub.paths.root / "Removed"))
                ),
            ),
            button(tr("Refresh"), self.refresh_removed),
        )
        if self.removed is None:
            box.addWidget(label(tr("Loading…"), "subtitle"))
            self.refresh_removed()
            return
        values = self.removed
        self.removed_page = min(self.removed_page, max(0, (len(values) - 1) // 4))
        if not values:
            box.addWidget(label(tr("No removed phones."), "subtitle", True))
        for value in values[self.removed_page * 4 : self.removed_page * 4 + 4]:
            if value.get("error"):
                box.addWidget(label(tr(value["error"]), "subtitle", True))
                continue
            item = card(body, value["metadata"]["name"], window.recovery.date(value["created"]))

            def restore(value=value):
                if window.recovery.confirm(
                    "Restore phone",
                    value["metadata"]["name"]
                    + "\n\n"
                    + tr("Restore this retained phone with its apps, saves and settings?"),
                    "Restore phone",
                ):
                    self.task(
                        window.hub.phone(),
                        "Restoring phone",
                        lambda storage, report: storage.restore_removed(value["token"], report),
                        cold=False,
                    )

            item.addWidget(button(tr("Restore phone"), restore, True))
        if len(values) > 4:

            def turn(delta):
                self.removed_page += delta
                window.navigate("instances")

            previous = button(tr("Previous"), lambda: turn(-1))
            previous.setEnabled(self.removed_page > 0)
            following = button(tr("Next"), lambda: turn(1))
            following.setEnabled((self.removed_page + 1) * 4 < len(values))
            row(
                body,
                previous,
                label(f"{self.removed_page + 1} / {(len(values) + 3) // 4}"),
                following,
            )

    def build_details(self, body):
        window, tr = self.window, self.window.tr
        phone = window.hub.phone(self.details)

        def back():
            self.details = None
            window.navigate("instances")

        body.addWidget(button(tr("All phones"), back))
        box = card(body, phone.metadata["name"], tr("Phone storage"))
        row(
            box,
            button(
                tr("Open phone folder"),
                lambda: QDesktopServices.openUrl(QUrl.fromLocalFile(str(phone.home))),
            ),
            button(tr("Refresh"), lambda: self.refresh_usage(phone)),
        )
        usage = self.usage.get(phone.identity)
        if usage is None:
            box.addWidget(label(tr("Calculating storage…"), "subtitle", True))
            self.refresh_usage(phone)
        elif usage.get("error"):
            box.addWidget(label(tr(usage["error"]), "subtitle", True))
        else:
            box.addWidget(label(usage["home"], "subtitle", True))
            for name, key in (
                ("Android data (logical)", "phone"),
                ("Android data on disk", "allocated"),
                ("Desktop files", "desktop"),
                ("Old startup logs", "logs"),
            ):
                row(box, label(tr(name)), None, label(f"{usage[key] / 1024**3:.2f} GB"))
        cold = (
            phone.state == "stopped"
            and not phone.alive()
            and not window.recovery.busy(phone.identity)
        )
        clear = button(
            tr("Clear old logs"),
            lambda: self.task(
                phone,
                "Clearing old startup logs",
                lambda storage, report: storage.clear_logs(report),
            ),
        )
        clear.setEnabled(cold)
        box.addWidget(clear)
        move = button(tr("Move phone storage"), lambda: self.relocate(phone))
        move.setEnabled(cold and phone.avd.is_dir())
        box.addWidget(move)
        if usage and not usage.get("error") and usage.get("move"):
            previous = usage["move"]["old_home"]
            box = card(body, tr("Original storage retained"), previous)
            undo = button(tr("Undo storage move"), lambda: self.undo_relocation(phone))
            undo.setEnabled(cold)
            row(
                box,
                undo,
                button(
                    tr("Open folder"),
                    lambda: QDesktopServices.openUrl(QUrl.fromLocalFile(previous)),
                ),
            )
        if phone.identity != "main":
            box = card(
                body,
                tr("Remove phone"),
                tr(
                    "Keeps a recoverable copy. Shared Android tools and other phones are untouched."
                ),
            )
            remove = button(tr("Remove phone"), lambda: self.remove(phone))
            remove.setEnabled(cold)
            box.addWidget(remove)
