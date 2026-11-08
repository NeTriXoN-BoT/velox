"""Share the reviewed English/German catalog with the Linux frontend."""

import ast
import json
import re
import sys
from pathlib import Path


def export(source: Path, target: Path):
    catalog = {}
    pattern = re.compile(r'\{L("(?:\\.|[^"\\])*")\s*,\s*L("(?:\\.|[^"\\])*")\s*\}')
    for english, german in pattern.findall(source.read_text(encoding="utf-8")):
        catalog[ast.literal_eval(english)] = ast.literal_eval(german)
    if len(catalog) < 700:
        raise ValueError("The reviewed translation catalog is incomplete")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(
        json.dumps(catalog, ensure_ascii=False, sort_keys=True, indent=2) + "\n", encoding="utf-8"
    )


if __name__ == "__main__":
    export(Path(sys.argv[1]), Path(sys.argv[2]))
