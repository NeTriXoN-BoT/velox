"""Collect versioned license notices and retain corresponding upstream sources."""

from __future__ import annotations

import argparse
import json
from pathlib import Path, PurePosixPath
import shutil
import tarfile

from build_downloads import download


def collect(cache, destination, source_bundle=None):
    root = Path(__file__).resolve().parents[1]
    lock = json.loads((root / "source-dependencies.lock.json").read_text())
    destination.mkdir(parents=True, exist_ok=True)
    archives, count = [], 0
    for name, spec in lock.items():
        archive = download(cache, spec.get("filename", spec["url"].rsplit("/", 1)[-1]), spec)
        archives.append(archive)
        if not tarfile.is_tarfile(archive):
            if archive.suffix not in (".dsc", ".asc") and not archive.name.endswith(
                (".diff.gz", ".diff.xz", ".diff.bz2")
            ):
                raise RuntimeError("Unrecognized corresponding-source archive: " + archive.name)
            continue
        with tarfile.open(archive) as source:
            for entry in source:
                relative = PurePosixPath(entry.name)
                names = [part.lower() for part in relative.parts]
                legal = "licenses" in names or any(
                    word in relative.name.lower()
                    for word in ("license", "copying", "copyright", "notice")
                )
                if not entry.isfile() or not legal:
                    continue
                if relative.is_absolute() or ".." in relative.parts or "\\" in entry.name:
                    raise RuntimeError(
                        "Unsafe source license path in " + archive.name + ": " + entry.name
                    )
                if entry.size > 8 * 1024**2:
                    raise RuntimeError("Oversized source license file.")
                target = destination / name / Path(*relative.parts[1:])
                target.parent.mkdir(parents=True, exist_ok=True)
                with source.extractfile(entry) as reader, target.open("wb") as output:
                    shutil.copyfileobj(reader, output)
                count += 1
    if count < 50:
        raise RuntimeError("The corresponding-source license inventory is incomplete.")
    (destination / "sources.json").write_text(json.dumps(lock, indent=2, sort_keys=True) + "\n")
    if source_bundle:
        if source_bundle.exists():
            raise RuntimeError("Choose a new corresponding-source output path.")
        with tarfile.open(source_bundle, "w") as target:
            for archive in archives:
                target.add(archive, arcname=archive.name, recursive=False)
            target.add(
                root / "source-dependencies.lock.json",
                arcname="source-dependencies.lock.json",
                recursive=False,
            )
            target.add(root / "assets/SOURCE-BUILD.md", arcname="SOURCE-BUILD.md", recursive=False)
    print(f"Collected {count} versioned source license notices", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sources", type=Path)
    options = parser.parse_args()
    collect(options.cache, options.output, options.sources)
