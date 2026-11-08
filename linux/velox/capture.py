from __future__ import annotations

import json
import re
import time
import uuid

from .core import VeloxError, atomic_bytes, atomic_json, lease, private_directory, read_json


class Recording:
    def __init__(self, phone):
        self.phone = phone
        self.ledger = phone.folder / "recording.json"

    def active(self) -> bool:
        record = read_json(self.ledger, {})
        if not isinstance(record, dict) or not record or not self.phone.alive():
            return False
        started = record.get("started")
        if type(started) not in (int, float) or not 0 <= started < 1e12:
            return False
        live = self.phone.runtime()
        elapsed = time.monotonic() - started
        return (
            record.get("pid") == live["pid"]
            and record.get("birth") == live["birth"]
            and 0 <= elapsed < 3605
            and isinstance(record.get("file"), str)
            and re.fullmatch(r"Recording-[0-9]{8}-[0-9]{6}-[a-f0-9]{8}\.webm", record["file"])
            is not None
        )

    def toggle(self):
        with lease(self.phone.paths.locks / (self.phone.identity + ".recording")):
            if not self.phone.alive() or self.phone.state != "running":
                raise VeloxError("Start the phone before recording.")
            folder = private_directory(self.phone.folder / "captures")
            if self.active():
                record = read_json(self.ledger)
                response = self.phone.adb("emu", "screenrecord", "stop", timeout=15).decode(
                    errors="replace"
                )
                if "OK" not in response or "KO:" in response:
                    raise VeloxError("The recording could not be stopped yet. Try again shortly.")
                self.ledger.unlink(missing_ok=True)
                file = folder / record["file"]
                deadline = time.monotonic() + 5
                while (
                    not file.is_file() or file.stat().st_size < 100
                ) and time.monotonic() < deadline:
                    time.sleep(0.1)
                if not file.is_file() or file.stat().st_size < 100:
                    raise VeloxError("The recording ended without a usable video file.")
                return file
            name = time.strftime("Recording-%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:8] + ".webm"
            target = folder / name
            atomic_bytes(target, b"")
            try:
                response = self.phone.adb(
                    "emu",
                    "screenrecord",
                    "start",
                    "--bit-rate",
                    "12M",
                    "--time-limit",
                    "3600",
                    json.dumps(str(target), ensure_ascii=False),
                    timeout=10,
                ).decode(errors="replace")
                if "OK" not in response or "KO:" in response:
                    raise VeloxError(
                        "The phone couldn't start recording. Its existing recording, if any, was left intact."
                    )
                live = self.phone.runtime()
                atomic_json(
                    self.ledger,
                    {
                        "pid": live["pid"],
                        "birth": live["birth"],
                        "started": time.monotonic(),
                        "file": name,
                    },
                )
            except Exception:
                if target.exists() and not target.stat().st_size:
                    target.unlink()
                raise
            return None
