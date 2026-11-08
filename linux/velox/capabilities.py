"""SDK-specific guest limits observed from Android, separate from host limits."""

from __future__ import annotations

import re

from .core import VeloxError, atomic_json, file_hash, lease, read_json
from .setup import resource_root


def cpu_count(value):
    if not isinstance(value, str) or not re.fullmatch(r"[0-9,-]{1,256}", value):
        return 0
    found = set()
    for part in value.split(","):
        if not re.fullmatch(r"[0-9]{1,2}(?:-[0-9]{1,2})?", part):
            return 0
        numbers = list(map(int, part.split("-")))
        low, high = numbers[0], numbers[-1]
        if low > high or high >= 64:
            return 0
        added = set(range(low, high + 1))
        if found.intersection(added):
            return 0
        found.update(added)
    return len(found)


def sdk_signature():
    return file_hash(resource_root() / "sdk.lock.json")


def cpu_limit(root):
    try:
        value = read_json(root / "engine-capabilities.json", {}, 4096)
        if (
            isinstance(value, dict)
            and value.get("format") == 1
            and value.get("sdk") == sdk_signature()
            and type(value.get("cores")) is int
            and 1 <= value["cores"] <= 64
        ):
            return value["cores"]
    except (OSError, VeloxError):
        pass
    return 64


def record_cpu_limit(paths, requested, observed):
    if not 0 < observed < requested <= 64:
        return
    with lease(paths.locks / "engine-capabilities.lock", blocking=True):
        atomic_json(
            paths.root / "engine-capabilities.json",
            {"format": 1, "sdk": sdk_signature(), "cores": min(cpu_limit(paths.root), observed)},
        )
