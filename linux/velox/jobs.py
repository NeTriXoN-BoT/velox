from __future__ import annotations

import traceback
from collections.abc import Callable

from PySide6.QtCore import QObject, QRunnable, QThreadPool, Signal, Slot

from .core import Cancelled, VeloxError


class Signals(QObject):
    done = Signal(object)
    failed = Signal(str)
    progress = Signal(str, float)


class Job(QRunnable):
    def __init__(self, action: Callable, progress=False):
        super().__init__()
        self.action, self.with_progress = action, progress
        self.signals = Signals()

    @Slot()
    def run(self):
        try:
            result = (
                self.action(self.signals.progress.emit) if self.with_progress else self.action()
            )
        except (VeloxError, Cancelled, OSError, ValueError) as error:
            self.signals.failed.emit(str(error))
        except Exception:
            traceback.print_exc()
            self.signals.failed.emit(
                "This action could not be completed. Your original phone data is retained."
            )
        else:
            self.signals.done.emit(result)


class Jobs(QObject):
    """Bounded concurrent work; duplicate refreshes never accumulate behind the UI."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.pool = QThreadPool(self)
        self.pool.setMaxThreadCount(3)
        self.active: dict[str, Job] = {}

    def submit(
        self,
        key,
        action,
        done=lambda _value: None,
        failed=lambda _message: None,
        progress=None,
        priority=0,
    ):
        if key in self.active:
            return False
        job = Job(action, progress=progress is not None)
        self.active[key] = job

        def success(value):
            self.active.pop(key, None)
            done(value)

        def failure(message):
            self.active.pop(key, None)
            failed(message)

        job.signals.done.connect(success)
        job.signals.failed.connect(failure)
        if progress:
            job.signals.progress.connect(progress)
        self.pool.start(job, priority)
        return True
