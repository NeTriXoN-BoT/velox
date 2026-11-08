"""Opt-in cold backups with explicit limits; never delete a recovery point."""

from __future__ import annotations

import time
import uuid

from .core import INSTANCE, atomic_json, lease, private_directory, read_json
from .vault import SCHEDULE_ID, Vault, require


class Schedule:
    def __init__(self, phone):
        self.phone, self.paths = phone, phone.paths
        require(INSTANCE.fullmatch(phone.identity), "Invalid phone identity.")
        # Protection policy must survive recovery of the phone's own settings.
        self.root = private_directory(self.paths.vault / "Schedules")
        self.path = self.root / (phone.identity + ".json")
        self.lock = self.paths.locks / (phone.identity + ".schedule")
        self.vault = Vault(phone)
        self.cancel = self.vault.cancel

    def read(self):
        value = read_json(self.path, None, 65536)
        if value is None:
            return {
                "format": "velox-linux-schedule",
                "version": 1,
                "phone": self.phone.identity,
                "owner": "",
                "days": 0,
                "keep": 3,
                "limit_gb": 25,
                "last": 0,
                "attempt": 0,
                "paused": False,
                "status": "",
            }
        require(
            isinstance(value, dict)
            and value.get("format") == "velox-linux-schedule"
            and type(value.get("version")) is int
            and value["version"] == 1
            and value.get("phone") == self.phone.identity
            and isinstance(value.get("owner"), str)
            and (value["owner"] == "" or SCHEDULE_ID.fullmatch(value["owner"]))
            and type(value.get("days")) is int
            and value["days"] in (0, 1, 7)
            and type(value.get("keep")) is int
            and 1 <= value["keep"] <= 20
            and type(value.get("limit_gb")) is int
            and 5 <= value["limit_gb"] <= 500
            and all(
                type(value.get(key)) is int and 0 <= value[key] <= 4102444800
                for key in ("last", "attempt")
            )
            and type(value.get("paused")) is bool
            and isinstance(value.get("status"), str)
            and len(value["status"]) <= 1200,
            "This backup schedule needs attention. Its settings are damaged.",
        )
        require(not value["days"] or value["owner"], "Invalid scheduled backup identity.")
        return value

    def save(self, *, days, keep, limit_gb):
        require(
            type(days) is int
            and days in (0, 1, 7)
            and type(keep) is int
            and 1 <= keep <= 20
            and type(limit_gb) is int
            and 5 <= limit_gb <= 500,
            "Invalid backup schedule limits.",
        )
        with lease(self.lock):
            value = self.read()
            value.update(
                days=days,
                keep=keep,
                limit_gb=limit_gb,
                paused=False,
                status="",
                attempt=0,
                owner=value["owner"] or uuid.uuid4().hex,
            )
            atomic_json(self.path, value)
        return value

    def due(self, timestamp=None):
        value = self.read()
        timestamp = int(time.time()) if timestamp is None else timestamp
        return bool(
            value["days"]
            and not value["paused"]
            and timestamp >= value["last"] + value["days"] * 86400
            and timestamp >= value["attempt"] + 3600
            and self.phone.state == "stopped"
            and not self.phone.alive()
            and self.phone.avd.is_dir()
        )

    def run(self, report=lambda _message, _fraction: None):
        with lease(self.lock):
            if not self.due():
                return None
            value = self.read()
            value["attempt"] = int(time.time())
            atomic_json(self.path, value)
            scheduled = {
                "id": value["owner"],
                "phone": self.phone.identity,
                "root": str(self.paths.root),
            }

            def permit(total):
                count, occupied = 0, 0
                for entry in self.vault.list():
                    require(
                        not entry["error"],
                        "A recovery point needs attention. Verify or move it before resuming scheduled backups.",
                    )
                    if entry["schedule"] == scheduled:
                        count += 1
                        occupied += entry["bytes"]
                require(
                    count < value["keep"] and occupied + total <= value["limit_gb"] * 1024**3,
                    "Scheduled backup limit reached. Move older scheduled recovery points out of the vault, or raise the limit.",
                )

            try:
                result = self.vault.create(report, schedule=scheduled, permit=permit)
            except Exception as error:
                value.update(paused=True, status=str(error)[:1200])
                atomic_json(self.path, value)
                raise
            value.update(last=int(time.time()), status="Scheduled backup verified.", paused=False)
            atomic_json(self.path, value)
            return result
