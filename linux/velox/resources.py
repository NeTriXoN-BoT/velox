"""Integrity-checked application resources from the installed Velox bundle."""

from __future__ import annotations

import re

from .core import VeloxError, file_hash, read_json, regular
from .setup import resource_root


def checked(name, maximum=32 * 1024**2):
    if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,79}", name):
        raise VeloxError("Invalid application resource name.")
    root = resource_root()
    value = read_json(root / "resources.json", {})
    expected = value.get(name) if isinstance(value, dict) else None
    path = root / name
    info = regular(path, maximum=maximum)
    if (
        info.st_nlink != 1
        or not isinstance(expected, str)
        or not re.fullmatch(r"[a-f0-9]{64}", expected)
        or file_hash(path) != expected
    ):
        raise VeloxError("An application resource did not pass its integrity check.")
    return path
