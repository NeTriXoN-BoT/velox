"""Prepare an isolated build Python from the project's pinned archives and wheels."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import tarfile
import urllib.parse
import zipfile

from build_downloads import download


def bootstrap(work):
    root = Path(__file__).resolve().parents[1]
    lock = json.loads((root / "dependencies.lock.json").read_text())
    work = work.resolve()
    work.mkdir(mode=0o700, parents=True, exist_ok=True)
    target, cache = work / "tools-python", work / "build-inputs"
    fingerprint = hashlib.sha256((root / "dependencies.lock.json").read_bytes()).hexdigest()
    marker = target / ".velox-build-runtime"
    if target.exists():
        if (
            target.is_symlink()
            or not marker.is_file()
            or marker.is_symlink()
            or marker.read_text().strip() != fingerprint
        ):
            raise SystemExit(
                "Choose a fresh build folder; this runtime does not match the dependency lock."
            )
        print(target / "bin/python3.12")
        return
    archive = download(
        cache, urllib.parse.unquote(Path(lock["python"]["url"]).name), lock["python"]
    )
    import tempfile

    with tempfile.TemporaryDirectory(prefix=".python-", dir=work) as temporary:
        with tarfile.open(archive) as source:
            source.extractall(temporary, filter="data")
        python = Path(temporary) / "python"
        site = python / "lib/python3.12/site-packages"
        site.mkdir(parents=True, exist_ok=True)
        for spec in lock["packages"].values():
            wheel = download(cache, spec["filename"], spec)
            with zipfile.ZipFile(wheel) as files:
                for entry in files.infolist():
                    path = PurePosixPath(entry.filename)
                    if path.is_absolute() or ".." in path.parts or "\\" in entry.filename:
                        raise RuntimeError("Unsafe wheel path.")
                files.extractall(site)
        (python / marker.name).write_text(fingerprint + "\n")
        os.rename(python, target)
    print(target / "bin/python3.12")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True)
    bootstrap(parser.parse_args().work)
