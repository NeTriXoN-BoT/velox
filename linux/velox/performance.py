"""Focus-scoped host integration; no privileged or global tuning in Velox."""

from __future__ import annotations

import contextlib
import dataclasses
import threading
import time

from .core import Settings, VeloxError
from .audio import PhoneMute
from .experience import Experience
from .graphics import effective_rate
from .game_mode import GameMode


class NativeSession:
    """One coalescing worker per phone; focus changes cannot block input or Qt."""

    def __init__(self, phone):
        self.phone = phone
        self._condition = threading.Condition()
        self._desired = None
        self._stop = False
        self.closed = threading.Event()
        self.game_mode = GameMode()
        self.mute = PhoneMute()
        self._rate_key = None
        self._boost_attempt = None
        self._frame_key = None
        self._thread = threading.Thread(
            target=self._run, name="Velox native performance", daemon=True
        )
        self._thread.start()

    def request(self, settings, foreground, app=None):
        with self._condition:
            if not self._stop:
                self._desired = dataclasses.replace(settings), bool(foreground), app
                self._condition.notify()

    def _run(self):
        try:
            while True:
                with self._condition:
                    self._condition.wait_for(lambda: self._stop or self._desired is not None)
                    if self._stop:
                        break
                    settings, foreground, app = self._desired
                    self._desired = None
                try:
                    with self.phone._policy_lock:
                        self._apply(settings, foreground, app)
                except (OSError, VeloxError):
                    # Optional host services cannot stop a working phone.
                    # A later preference/focus change can reconnect them.
                    self._rate_key = None
        finally:
            try:
                with self.phone._policy_lock:
                    self._restore()
            finally:
                self.closed.set()

    def _apply(self, settings, foreground, app):
        if not self.phone.alive() or self.phone.state != "running":
            self.game_mode.close(self.phone)
            self.mute.update(self.phone, False)
            self._rate_key = self._boost_attempt = self._frame_key = None
            return
        record = self.phone.runtime()
        actual = Settings.from_dict(record["settings"])
        rate = effective_rate(record, min(settings.rate, actual.rate))
        if not foreground and settings.background_rate:
            rate = min(rate, settings.background_rate)
        # The emulated display has no 15 Hz mode. Asking Android
        # for that mode can select 60 Hz instead; native per-game
        # pacing supplies 15 FPS while the display stays at 30 Hz.
        display_rate = max(30, rate)
        key = (
            record["pid"],
            record["birth"],
            display_rate,
            self.phone._refresh_generation,
        )
        if key != self._rate_key:
            self.phone.refresh_rate(display_rate)
            self._rate_key = key
        frame_key = (
            record["pid"],
            record["birth"],
            app,
            rate if app and rate < actual.rate else 0,
        )
        if frame_key != self._frame_key:
            Experience(self.phone).frame_policy(app, frame_key[3])
            self._frame_key = frame_key
        boost_key = record["pid"], record["birth"], settings.boost and foreground
        if (
            boost_key != self._boost_attempt
            or self.game_mode.status == "releasing"
            or self.game_mode.status == "unavailable"
            and time.monotonic() >= self.game_mode.retry_at
        ):
            self.game_mode.update(self.phone, boost_key[2])
            self._boost_attempt = boost_key
        self.mute.update(self.phone, settings.background_mute and not foreground)

    def _restore(self):
        for _attempt in range(3):
            with contextlib.suppress(OSError, VeloxError):
                if self.game_mode.close(self.phone):
                    break
        self.game_mode.abandon()
        with contextlib.suppress(OSError, VeloxError):
            self.mute.update(self.phone, False)
        if self.phone.alive():
            with contextlib.suppress(OSError, VeloxError):
                Experience(self.phone).restore_installed_policy()
        if self.phone.alive() and self.phone.state == "running":
            with contextlib.suppress(OSError, VeloxError):
                settings = self.phone.running_settings or self.phone.paths.settings(
                    self.phone.identity
                )
                actual = Settings.from_dict(self.phone.runtime()["settings"])
                self.phone.refresh_rate(
                    effective_rate(self.phone.runtime(), min(settings.rate, actual.rate))
                )

    def close(self):
        with self._condition:
            self._stop = True
            self._desired = None
            self._condition.notify()
