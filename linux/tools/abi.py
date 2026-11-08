"""Report imported ABI versions without executing the packaged binaries."""

from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path


def requirements(file: Path) -> dict:
    with file.open("rb") as reader:
        if reader.read(4) != b"\x7fELF":
            return {}
    output = subprocess.run(
        ["readelf", "-W", "--dyn-syms", str(file)], capture_output=True, check=True
    ).stdout.decode(errors="replace")
    result = {}
    for line in output.splitlines():
        if " UND " not in line:
            continue
        for family, number in re.findall(r"@(GLIBC|GLIBCXX|CXXABI)_([0-9.]+)", line):
            value = tuple(map(int, number.split(".")))
            if value > result.get(family, (0,)):
                result[family] = value
    return {key: ".".join(map(str, value)) for key, value in result.items()}


if __name__ == "__main__":
    for argument in sys.argv[1:]:
        file = Path(argument)
        print(json.dumps({"file": str(file), "requires": requirements(file)}))
