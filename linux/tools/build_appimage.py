"""Assemble a clean x86-64 AppImage from pinned runtimes and explicit build artifacts."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tarfile
import tempfile
import urllib.parse
import zipfile

from abi import requirements
from build_downloads import download
from build_licenses import collect as collect_licenses
from translations import export


ROOT = Path(__file__).resolve().parents[2]
LINUX = ROOT / "linux"
QT_MODULES = {"QtCore", "QtGui", "QtWidgets", "QtNetwork", "QtOpenGL", "QtOpenGLWidgets", "QtSvg"}
QT_PLUGINS = {
    "platforms",
    "platformthemes",
    "iconengines",
    "imageformats",
    "xcbglintegrations",
    "wayland-decoration-client",
    "wayland-graphics-integration-client",
    "wayland-shell-integration",
}
HOST_LIBRARIES = re.compile(
    r"^(?:ld-linux|lib(?:c|m|dl|pthread|rt|resolv|util|GL|EGL|GLX|OpenGL|GLdispatch|drm|gbm|vulkan|nvidia)[-.])"
)
# Keep desktop, compiler and graphics-provider libraries on the host, following
# AppImageCommunity/pkg2appimage's compatibility guidance. Bundling these can
# override a newer GPU driver or font stack with the build machine's versions.
HOST_NAMES = frozenset(
    (
        "libstdc++.so.6",
        "libgcc_s.so.1",
        "libexpat.so.1",
        "libfontconfig.so.1",
        "libfreetype.so.6",
        "libharfbuzz.so.0",
        "libX11.so.6",
        "libX11-xcb.so.1",
        "libxcb.so.1",
        "libwayland-client.so.0",
        "libasound.so.2",
        "libICE.so.6",
        "libSM.so.6",
        "libusb-1.0.so.0",
        "libuuid.so.1",
        "libz.so.1",
        "libpipewire-0.3.so.0",
    )
)


def run(arguments, **kwargs):
    subprocess.run(list(map(str, arguments)), check=True, **kwargs)


def digest(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def unzip(archive, target):
    with zipfile.ZipFile(archive) as source:
        if (
            len(source.infolist()) > 30000
            or sum(item.file_size for item in source.infolist()) > 1024**3
        ):
            raise RuntimeError("The pinned wheel exceeds its extraction limits.")
        for item in source.infolist():
            path = PurePosixPath(item.filename)
            if (
                path.is_absolute()
                or ".." in path.parts
                or "\\" in item.filename
                or item.flag_bits & 1
            ):
                raise RuntimeError("Unsafe wheel path.")
        source.extractall(target)


def elf(path):
    if not path.is_file() or path.is_symlink():
        return False
    with path.open("rb") as source:
        return source.read(4) == b"\x7fELF"


def normalize_permissions(appdir):
    # Windows-backed source mounts can report every file as 0777. Extraction
    # must never leave application code writable by other desktop accounts.
    for path in appdir.rglob("*"):
        if path.is_symlink():
            if not path.resolve(strict=True).is_relative_to(appdir):
                raise RuntimeError("A package link leaves its AppDir.")
        elif path.is_dir():
            path.chmod(0o755)
        elif path.is_file():
            path.chmod(0o755 if path == appdir / "AppRun" or elf(path) else 0o644)
        else:
            raise RuntimeError("A package contains an unsupported file type.")
    appdir.chmod(0o755)


def qt_trim(site):
    pyside = site / "PySide6"
    for path in list(pyside.iterdir()):
        keep = (
            path.name in ("__init__.py", "_config.py", "_git_pyside_version.py", "support", "Qt")
            or path.name.startswith("libpyside6")
            or path.name.split(".")[0] in QT_MODULES
        )
        if not keep:
            shutil.rmtree(path) if path.is_dir() else path.unlink()
    qt = pyside / "Qt"
    for path in list(qt.iterdir()):
        if path.name not in ("lib", "plugins", "translations", "licenses"):
            shutil.rmtree(path) if path.is_dir() else path.unlink()
    for path in (qt / "plugins").iterdir():
        if path.name not in QT_PLUGINS:
            shutil.rmtree(path) if path.is_dir() else path.unlink()
    # The Essentials wheel can carry a PDF codec whose QtPdf library belongs
    # to Addons. Velox opens raster wallpapers and does not use PDF rendering.
    (qt / "plugins/imageformats/libqpdf.so").unlink(missing_ok=True)
    (qt / "plugins/platformthemes/libqgtk3.so").unlink(missing_ok=True)
    for path in (qt / "plugins/platforms").iterdir():
        if path.name not in ("libqxcb.so", "libqwayland.so", "libqoffscreen.so"):
            path.unlink()
    # Keep the native desktop themes, image codecs and X11/Wayland integrations.
    # Resolve their Qt dependencies before discarding unused QML/media libraries.
    available = {path.name: path for path in (qt / "lib").iterdir() if path.is_file()}
    roots = [path for path in pyside.glob("*.so*") if elf(path)]
    roots += [path for path in (qt / "plugins").rglob("*.so") if elf(path)]
    needed, pending = set(), list(roots)
    while pending:
        path = pending.pop()
        text = subprocess.check_output(["readelf", "-d", str(path)]).decode()
        for name in re.findall(r"\(NEEDED\).*?\[(.*?)\]", text):
            if name in available and name not in needed:
                needed.add(name)
                pending.append(available[name])
    for name, path in available.items():
        if name not in needed:
            path.unlink()
    for path in (qt / "translations").glob("*"):
        if path.name not in ("qtbase_de.qm", "qt_de.qm"):
            path.unlink()


def system_libraries(appdir):
    destination = appdir / "usr/lib"
    copied = {}
    pending = [path for path in appdir.rglob("*") if elf(path)]
    inspected = set()
    while pending:
        path = pending.pop()
        if path in inspected:
            continue
        inspected.add(path)
        qt_lib = appdir / "usr/lib/python3.12/site-packages/PySide6/Qt/lib"
        result = subprocess.run(
            ["ldd", str(path)],
            capture_output=True,
            text=True,
            env=dict(os.environ, LD_LIBRARY_PATH=str(qt_lib) + ":" + str(destination)),
        )
        for line in result.stdout.splitlines():
            match = re.match(r"\s*(\S+) => (\S+) \(", line)
            if not match:
                if "=> not found" in line:
                    raise RuntimeError("Missing build dependency: " + line.strip())
                continue
            name, location = match.groups()
            source = Path(location).resolve()
            if HOST_LIBRARIES.match(name) or name in HOST_NAMES or appdir in source.parents:
                continue
            if name.startswith("libQt6"):
                raise RuntimeError("A bundled plugin requires an unavailable Qt library: " + name)
            if name not in copied:
                target = destination / name
                if target.exists():
                    if digest(target) != digest(source):
                        raise RuntimeError("Conflicting runtime library: " + name)
                    continue
                shutil.copyfile(source, target)
                copied[name] = str(source)
                pending.append(target)
    return copied


def resources(appdir, artifacts, python):
    target = appdir / "usr/share/velox"
    target.mkdir(parents=True)
    # Explicit allowlist prevents QA files, SDKs, accounts, old tools and signing
    # material from entering a release even when the build folder contains them.
    for name in ("helper.apk", "companion.apk"):
        source = artifacts / name
        if source.is_symlink() or not source.is_file() or source.stat().st_size > 16 * 1024**2:
            raise RuntimeError("Missing regular Android build artifact: " + name)
        shutil.copyfile(source, target / name)
    for name in ("VeloxNavOrder.apk", "stock-taskbar.xml", "post-fs-data.sh"):
        data = (ROOT / "navigation" / name).read_bytes()
        if name.endswith(".sh"):
            data = data.replace(b"\r\n", b"\n")
        (target / name).write_bytes(data)
    for name in ("sdk.lock.json", "android-sdk-license.txt", "patch-ramdisk.sh"):
        (target / name).write_bytes((LINUX / "assets" / name).read_bytes().replace(b"\r\n", b"\n"))
    (target / "version.txt").write_text((ROOT / "VERSION").read_text().strip() + "\n")
    export(ROOT / "src/localization.inc", target / "de.json")
    walls = target / "wallpapers"
    walls.mkdir()
    for source in sorted((ROOT / "wallpapers").glob("*.png")):
        shutil.copyfile(source, walls / source.name)
    native = target / "native"
    native.mkdir()
    for name in ("libSDL2-2.0.so.0", "libvelox-pointer.so"):
        source = artifacts / "native" / name
        if source.is_symlink() or not source.is_file():
            raise RuntimeError("Missing regular native build artifact: " + name)
        shutil.copyfile(source, native / name)
    protocol = target / "protocol"
    protocol.mkdir()
    run(
        [
            python,
            "-m",
            "grpc_tools.protoc",
            "-I",
            LINUX / "proto",
            "--python_out=" + str(protocol),
            "--grpc_python_out=" + str(protocol),
            LINUX / "proto/emulator_controller.proto",
        ]
    )
    environment = dict(
        os.environ, PYTHONPATH=str(LINUX), QT_QPA_PLATFORM="offscreen", PYTHONDONTWRITEBYTECODE="1"
    )
    run(
        [
            python,
            "-c",
            "from PySide6.QtWidgets import QApplication; from velox.theme import brand_icon; import sys; app=QApplication([]); assert brand_icon().pixmap(256,256).save(sys.argv[1])",
            target / "icon.png",
        ],
        env=environment,
    )
    inventory = {
        path.relative_to(target).as_posix(): digest(path)
        for path in sorted(target.rglob("*"))
        if path.is_file()
    }
    (target / "resources.json").write_text(json.dumps(inventory, indent=2, sort_keys=True) + "\n")
    return target


def assemble(options):
    if os.name != "posix" or os.uname().machine != "x86_64":
        raise SystemExit("Build this AppImage on x86-64 Linux.")
    lock = json.loads((LINUX / "dependencies.lock.json").read_text())
    work = options.work.resolve()
    work.mkdir(mode=0o700, parents=True, exist_ok=True)
    cache = work / "build-inputs"
    python_archive = download(
        cache, urllib.parse.unquote(Path(lock["python"]["url"]).name), lock["python"]
    )
    wheels = {
        name: download(cache, spec["filename"], spec) for name, spec in lock["packages"].items()
    }
    appdir = options.appdir.absolute()
    if appdir.exists() or options.output.exists():
        raise SystemExit(
            "Choose new AppDir and output paths; existing artifacts are never overwritten."
        )
    appdir.mkdir(mode=0o700, parents=True)
    with tempfile.TemporaryDirectory(prefix=".runtime-", dir=work) as temporary:
        with tarfile.open(python_archive) as archive:
            archive.extractall(temporary, filter="data")
        shutil.move(str(Path(temporary) / "python"), str(appdir / "usr"))
    site = appdir / "usr/lib/python3.12/site-packages"
    site.mkdir(parents=True, exist_ok=True)
    for path in list(site.iterdir()):
        shutil.rmtree(path) if path.is_dir() else path.unlink()
    for name, wheel in wheels.items():
        if not lock["packages"][name].get("build_only", False):
            unzip(wheel, site)
    qt_trim(site)
    shutil.copytree(
        LINUX / "velox", site / "velox", ignore=shutil.ignore_patterns("__pycache__", "*.pyc")
    )
    for name in (
        "idlelib",
        "test",
        "ensurepip",
        "tkinter",
        "turtledemo",
        "curses",
        "dbm",
        "config-3.12-x86_64-linux-gnu",
    ):
        path = appdir / "usr/lib/python3.12" / name
        if path.exists():
            shutil.rmtree(path)
    for path in (appdir / "usr/bin").iterdir():
        if path.name not in ("python3", "python3.12"):
            path.unlink()
    for path in (
        appdir / "usr/include",
        appdir / "usr/share/man",
        appdir / "usr/share/terminfo",
        appdir / "usr/lib/pkgconfig",
    ):
        if path.exists():
            shutil.rmtree(path)
    # Tcl/Tk is part of the standalone Python download; no shipped module uses it.
    for pattern in ("tcl*", "tk*", "itcl*", "thread*", "libtcl*", "libtk*"):
        for path in (appdir / "usr/lib").glob(pattern):
            shutil.rmtree(path) if path.is_dir() else path.unlink()
    for pattern in ("_crypt.*", "_curses*", "readline.*", "_tkinter.*", "_dbm.*", "_gdbm.*"):
        for path in (appdir / "usr/lib/python3.12/lib-dynload").glob(pattern):
            path.unlink()
    assets = resources(appdir, options.resources.resolve(), options.python)
    libraries = system_libraries(appdir)
    licenses = appdir / "usr/share/licenses/velox"
    licenses.mkdir(parents=True)
    collect_licenses(cache, licenses / "upstream-source-notices", options.sources_output)
    shutil.copytree("/usr/share/common-licenses", licenses / "common-licenses")
    shutil.copytree(LINUX / "assets/licenses", licenses / "native-notices")
    shutil.copyfile(LINUX / "assets/SOURCE-BUILD.md", licenses / "SOURCE-BUILD.md")
    for source in (
        ROOT / "LICENSE.txt",
        ROOT / "release/THIRD-PARTY-NOTICES.txt",
        LINUX / "assets/LICENSE-Magisk.txt",
    ):
        shutil.copyfile(source, licenses / source.name)
    system_packages = []
    for name, location in libraries.items():
        result = subprocess.run(["dpkg-query", "-S", location], capture_output=True, text=True)
        if result.returncode != 0:
            alias = (
                location.removeprefix("/usr") if location.startswith("/usr/") else "/usr" + location
            )
            result = subprocess.run(
                ["dpkg-query", "-S", alias],
                capture_output=True,
                text=True,
            )
        if result.returncode != 0:
            raise RuntimeError("Identify the license for bundled library " + name)
        package = result.stdout.split(": ", 1)[0].split(":", 1)[0]
        details = subprocess.check_output(
            ["dpkg-query", "-W", "-f=${Version}|${source:Package}|${source:Version}", package],
            text=True,
        ).split("|")
        system_packages.append(
            dict(
                library=name,
                package=package,
                version=details[0],
                source=details[1],
                source_version=details[2],
                sha256=digest(appdir / "usr/lib" / name),
            )
        )
        copyright = Path("/usr/share/doc") / package / "copyright"
        if not copyright.exists():
            raise RuntimeError("Missing library license: " + package)
        shutil.copyfile(copyright, licenses / (package + "-copyright.txt"))
    # Dynamic Qt libraries remain replaceable after --appimage-extract. Preserve
    # every wheel's original license directory and provide corresponding-source links.
    (licenses / "LINUX-NOTICES.txt").write_text(
        "Velox uses dynamically linked Qt/PySide6 under LGPL-3.0, CPython, grpcio, protobuf, cryptography, CFFI, SDL2 and Wayland components. Original license texts are retained beside each Python distribution and in this folder.\nThe matching Velox release includes a corresponding-source archive containing the exact Qt/PySide, bundled Debian library, native controls and AppImage runtime sources, patches and build instructions. Its source-dependencies.lock.json records archive origins and checksums.\nQt/PySide source and relinking information: https://code.qt.io/cgit/pyside/pyside-setup.git/ and https://code.qt.io/cgit/qt/qtbase.git/ ; use the exact versions in dependencies.lock.json.\nSDL source: "
        + lock["sdl2"]["url"]
        + "\nMagisk patcher: https://github.com/topjohnwu/Magisk/tree/v31.1 ; modified source is included in share/velox/patch-ramdisk.sh.\nExtract with --appimage-extract to replace LGPL libraries with compatible modified builds. No application signature check prevents replacing these libraries. Google SDK files are downloaded separately after license acceptance.\n"
    )
    shutil.copyfile(assets / "icon.png", appdir / "velox.png")
    metainfo = appdir / "usr/share/metainfo"
    metainfo.mkdir(parents=True)
    shutil.copyfile(
        LINUX / "assets/app.velox.Velox.metainfo.xml", metainfo / "app.velox.Velox.metainfo.xml"
    )
    os.symlink("velox.png", appdir / ".DirIcon")
    (appdir / "app.velox.Velox.desktop").write_text(
        "[Desktop Entry]\nType=Application\nName=Velox\nComment=Your games. Your phone. Your space.\nComment[de]=Deine Spiele. Dein Handy. Dein Platz.\nExec=AppRun\nIcon=velox\nTerminal=false\nCategories=Game;Emulator;\nStartupWMClass=Velox\n"
    )
    (appdir / "AppRun").write_text(
        '#!/bin/sh\nset -eu\nAPPDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\nexport APPDIR\nunset PYTHONPATH PYTHONHOME LD_PRELOAD QT_PLUGIN_PATH QT_QPA_PLATFORM_PLUGIN_PATH\nexport VELOX_RESOURCES="$APPDIR/usr/share/velox"\nexport PYTHONDONTWRITEBYTECODE=1 PYTHONNOUSERSITE=1\nexport LD_LIBRARY_PATH="$APPDIR/usr/lib/python3.12/site-packages/PySide6/Qt/lib:$APPDIR/usr/lib"\nexec "$APPDIR/usr/bin/python3.12" -I -B -m velox "$@"\n'
    )
    os.chmod(appdir / "AppRun", 0o755)
    # Inventory is relative and contains no build-machine paths or account data.
    abi = {}
    for path in sorted(appdir.rglob("*")):
        if elf(path):
            abi[path.relative_to(appdir).as_posix()] = requirements(path)
    manifest = {
        "format": "velox-linux-build",
        "version": (ROOT / "VERSION").read_text().strip(),
        "architecture": "x86_64",
        "dependencies": lock,
        "abi": abi,
        "bundled_system_libraries": sorted(libraries),
        "system_packages": sorted(system_packages, key=lambda item: item["library"]),
    }
    (assets / "build-info.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    normalize_permissions(appdir)
    run(
        [
            appdir / "usr/bin/python3.12",
            "-I",
            "-B",
            "-c",
            "from PySide6.QtWidgets import QApplication; import grpc,cryptography; from velox.gui import MainWindow; app=QApplication([]); print('Runtime imports passed')",
        ],
        env=dict(
            os.environ,
            LD_LIBRARY_PATH=str(site / "PySide6/Qt/lib") + ":" + str(appdir / "usr/lib"),
            QT_QPA_PLATFORM="offscreen",
            VELOX_RESOURCES=str(assets),
        ),
    )
    tool = download(cache, "appimagetool-x86_64.AppImage", lock["appimagetool"])
    runtime = download(cache, "appimage-runtime-20251108-x86_64", lock["appimage_runtime"])
    tool.chmod(0o755)
    options.output.parent.mkdir(parents=True, exist_ok=True)
    run(
        [
            tool,
            "--appimage-extract-and-run",
            "--no-appstream",
            "--runtime-file",
            runtime,
            appdir,
            options.output.absolute(),
        ],
        env=dict(
            os.environ, ARCH="x86_64", SOURCE_DATE_EPOCH=options.epoch, APPIMAGE_EXTRACT_AND_RUN="1"
        ),
    )
    print(
        json.dumps(
            {
                "artifact": str(options.output),
                "sha256": digest(options.output),
                "bytes": options.output.stat().st_size,
            }
        ),
        flush=True,
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument(
        "--resources", type=Path, required=True, help="Trusted built APKs and native/ libraries"
    )
    parser.add_argument(
        "--python",
        type=Path,
        required=True,
        help="Build Python with pinned grpcio-tools and PySide6",
    )
    parser.add_argument("--appdir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--sources-output",
        type=Path,
        help="Optional corresponding-source tar archive for the release",
    )
    parser.add_argument("--epoch", default="1794096000")
    assemble(parser.parse_args())
