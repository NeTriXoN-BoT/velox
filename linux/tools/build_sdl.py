"""Build the pinned controller library without changing the host installation."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import tarfile
import tempfile

from build_downloads import download


def build(work: Path, prefix: Path, jobs: int):
    if os.name != "posix":
        raise SystemExit("Build the Linux controller library on Linux.")
    lock = json.loads(
        (Path(__file__).resolve().parents[1] / "dependencies.lock.json").read_text(encoding="utf-8")
    )["sdl2"]
    work, prefix = work.resolve(), prefix.resolve()
    archive = download(work, "SDL2-" + lock["version"] + ".tar.gz", lock)
    prefix.mkdir(mode=0o700, parents=True, exist_ok=True)
    source = work / ("SDL2-" + lock["version"])
    marker = source / ".velox-source-sha256"
    if source.exists():
        if (
            source.is_symlink()
            or marker.is_symlink()
            or not marker.is_file()
            or marker.read_text().strip() != lock["sha256"]
        ):
            raise RuntimeError(
                "Use a fresh build folder; this source tree has no completed integrity record."
            )
    else:
        with tempfile.TemporaryDirectory(prefix=".source-", dir=work) as temporary:
            staging = Path(temporary)
            with tarfile.open(archive) as files:
                total, names = 0, set()
                for entry in files:
                    name = PurePosixPath(entry.name)
                    total += entry.size
                    if (
                        not name.parts
                        or name.is_absolute()
                        or ".." in name.parts
                        or name.parts[0] != source.name
                        or entry.name in names
                        or len(names) >= 10000
                        or entry.size > 32 * 1024**2
                        or total > 256 * 1024**2
                    ):
                        raise RuntimeError("Unsupported controller source archive member.")
                    names.add(entry.name)
                    if entry.issym():
                        link = PurePosixPath(entry.linkname)
                        target = (staging / Path(*name.parts).parent / entry.linkname).resolve()
                        if link.is_absolute() or not target.is_relative_to(staging / source.name):
                            raise RuntimeError("Controller source link leaves its source folder.")
                    elif not (entry.isfile() or entry.isdir()):
                        raise RuntimeError("Unsupported controller source archive member.")
                files.extractall(staging, filter="data")
            extracted = staging / source.name
            (extracted / marker.name).write_text(lock["sha256"] + "\n", encoding="ascii")
            os.replace(extracted, source)
    output = work / "objects"
    output.mkdir(exist_ok=True)
    environment = dict(os.environ)
    for key in ("LD_LIBRARY_PATH", "LD_PRELOAD", "PYTHONHOME", "PYTHONPATH", "QT_PLUGIN_PATH"):
        environment.pop(key, None)
    environment["CFLAGS"] = "-O2 -fPIC -ffile-prefix-map=" + str(work) + "=/usr/src/velox/sdl2"
    environment["LDFLAGS"] = "-Wl,-z,relro,-z,now"
    subprocess.run(
        [
            str(source / "configure"),
            "--prefix=" + str(prefix),
            "--disable-static",
            "--enable-shared",
            "--disable-video",
            "--disable-audio",
            "--disable-render",
            "--disable-haptic",
            "--disable-sensor",
            "--disable-power",
            "--disable-filesystem",
            "--disable-rpath",
        ],
        cwd=output,
        env=environment,
        check=True,
    )
    subprocess.run(
        ["make", "-j" + str(max(1, min(8, jobs)))], cwd=output, env=environment, check=True
    )
    subprocess.run(["make", "install"], cwd=output, env=environment, check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=2)
    options = parser.parse_args()
    build(options.work, options.prefix, options.jobs)
