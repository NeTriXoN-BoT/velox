from __future__ import annotations

import argparse
import os
from pathlib import Path
import sys
import subprocess


def main(arguments=None):
    parser = argparse.ArgumentParser(description="Velox Android gaming workspace")
    parser.add_argument("--data-dir", type=Path, help="Use a private Velox data folder")
    parser.add_argument(
        "--no-autostart", action="store_true", help="Open the launcher without starting Android"
    )
    options = parser.parse_args(arguments)
    if os.getuid() == 0:
        print(
            "Start Velox as your regular desktop account. KVM access is configured separately; do not run Velox with sudo.",
            file=sys.stderr,
        )
        return 1

    from PySide6.QtWidgets import QApplication, QMessageBox
    from .core import Busy, Paths, VeloxError
    from .gui import MainWindow
    from .phone import Hub
    from .singleinstance import Launcher, activate_existing

    # The ELF loader already knows the bundle paths. Child desktop services must
    # use their own distribution's libraries rather than inherit AppImage's GLib.
    os.environ.pop("LD_LIBRARY_PATH", None)

    application = QApplication(sys.argv[:1])
    application.setApplicationName("Velox")
    application.setOrganizationName("Velox")
    application.setDesktopFileName("app.velox.Velox")
    application.setQuitOnLastWindowClosed(False)
    hub = launcher = window = None
    try:
        hub = Hub(Paths(options.data_dir))
        try:
            launcher = Launcher(hub)
        except Busy:
            if activate_existing(hub.runtime / "launcher.sock"):
                return 0
            raise VeloxError(
                "Velox is starting or closing for this data folder. Try again shortly."
            )
        window = MainWindow(hub, no_autostart=options.no_autostart)

        def activate():
            if not window._closing:
                window.showNormal() if window.isMinimized() else window.show()
                window.raise_()
                window.activateWindow()

        launcher.activated.connect(activate)
        window.show()
        exit_code = application.exec()
    except (OSError, VeloxError) as error:
        print("Velox: " + str(error), file=sys.stderr)
        from PySide6.QtCore import QLocale
        from .i18n import Translator

        language = "de" if QLocale.system().name().startswith("de") else "en"
        try:
            if hub:
                language = hub.paths.settings().language
            text = (window.tr if window else Translator(language))(str(error))
        except (OSError, VeloxError):
            text = str(error)
        message = QMessageBox(
            QMessageBox.Icon.Critical, "Velox", text, QMessageBox.StandardButton.Close
        )
        from PySide6.QtCore import Qt

        message.setTextFormat(Qt.TextFormat.PlainText)
        message.exec()
        return 1
    finally:
        if launcher:
            launcher.close()
        if hub and hub.registration:
            hub.registration.close()
    restart = application.property("veloxRestart")
    if restart:
        # Launch only after the old activation socket and ownership lease close.
        from .phone import clean_environment

        subprocess.Popen(
            [restart, "--no-autostart", "--data-dir", str(hub.paths.root)],
            env=clean_environment(),
            start_new_session=True,
            close_fds=True,
        )
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
