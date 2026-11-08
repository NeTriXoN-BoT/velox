"""Build Velox's Wayland pointer lock adapter with the pinned protocol definitions."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tarfile

from build_downloads import download


def build(work: Path, output: Path):
    root = Path(__file__).resolve().parents[1]
    spec = json.loads((root / "dependencies.lock.json").read_text(encoding="utf-8"))[
        "wayland_protocols"
    ]
    work = work.resolve()
    archive = download(work, "wayland-protocols-" + spec["version"] + ".tar.xz", spec)
    generated = work / "generated"
    generated.mkdir(exist_ok=True)
    compiler_sources = []
    with tarfile.open(archive) as source:
        for name in ("pointer-constraints", "relative-pointer"):
            member = source.getmember(
                "wayland-protocols-"
                + spec["version"]
                + "/unstable/"
                + name
                + "/"
                + name
                + "-unstable-v1.xml"
            )
            if not member.isfile() or member.size > 65536:
                raise RuntimeError("Unsupported pointer protocol archive entry.")
            xml = generated / (name + ".xml")
            with source.extractfile(member) as content:
                xml.write_bytes(content.read(65537))
            subprocess.run(
                ["wayland-scanner", "client-header", str(xml), str(generated / (name + ".h"))],
                check=True,
            )
            code = generated / (name + ".c")
            subprocess.run(["wayland-scanner", "private-code", str(xml), str(code)], check=True)
            compiler_sources.append(str(code))
    output.parent.mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ)
    for key in ("LD_LIBRARY_PATH", "LD_PRELOAD"):
        environment.pop(key, None)
    flags = shlex.split(
        subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", "wayland-client"], env=environment
        ).decode()
    )
    command = [
        "cc",
        "-std=c11",
        "-O2",
        "-fPIC",
        "-shared",
        "-fvisibility=hidden",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I",
        str(generated),
        str(root / "native/pointer.c"),
        *compiler_sources,
        "-Wl,-z,relro,-z,now",
        "-o",
        str(output),
        *flags,
        "-lm",
    ]
    subprocess.run(command, env=environment, check=True)
    subprocess.run(["strip", "--strip-unneeded", str(output)], check=True)
    licenses = work / "licenses"
    licenses.mkdir(exist_ok=True)
    for name in ("pointer-constraints", "relative-pointer"):
        shutil.copy2(generated / (name + ".xml"), licenses / (name + ".xml"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    build(options.work, options.output)
