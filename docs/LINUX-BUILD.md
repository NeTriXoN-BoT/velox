# Building the Linux AppImage

Build on x86-64 Linux with Python 3.12+, a C/C++ compiler, Make, pkg-config,
Wayland client development headers and `wayland-scanner`, `readelf`, `strip`,
`ldd`, and the usual desktop libraries. The current packaging script collects
system-library license notices using `dpkg-query`; use a Debian 12 build host.
The package keeps graphics, font and compiler runtime libraries on the target
desktop. Its bundled binaries must remain compatible with glibc 2.34.

Android compilation also needs JDK 17+ and Android SDK platform 35 with build-tools
35.0.0. These are development dependencies; users do not need them to run Velox.

## Build dependencies

Start in the repository root and choose a separate, private build folder:

```sh
WORK="$HOME/velox-build"
python3 linux/tools/bootstrap.py --work "$WORK"
PY="$WORK/tools-python/bin/python3.12"
```

The bootstrap checks every download against `linux/dependencies.lock.json`.
It does not install Python or packages into the host system. Runtime wheels are
installed separately in the AppImage; build-only tools do not enter the release.

## Android apps

Keep a signing keystore and its password file outside the repository and release
folders. A local development key can be made with `keytool`; official updates
must keep the existing private helper certificate. Use a new Velox data folder
when testing apps signed by a different key.

```sh
"$PY" linux/tools/build_android.py \
  --sdk "$ANDROID_SDK_ROOT" --java-home "$JAVA_HOME" \
  --keystore /private/helper.keystore --password-file /private/helper.pass \
  --alias helper --out "$WORK/resources"
```

This compiles the public Java sources with `javac`, D8 and AAPT2, aligns both APKs
and verifies their signatures. No Gradle is used. Password contents are not passed
on the command line or copied to the output folder.

## Native controls

```sh
"$PY" linux/tools/build_sdl.py --work "$WORK/sdl-build" --prefix "$WORK/sdl" --jobs 2
install -Dm755 "$WORK/sdl/lib/libSDL2-2.0.so.0" "$WORK/resources/native/libSDL2-2.0.so.0"
"$PY" linux/tools/build_pointer.py --work "$WORK/pointer-build" \
  --output "$WORK/resources/native/libvelox-pointer.so"
```

SDL and the Wayland protocol definitions are pinned and verified. Controller
support does not require a system-wide input-injection service. Mouse capture
uses X11's native mechanism or Wayland's relative-pointer and pointer-constraint
protocols, scoped to the focused phone view.

## Package

```sh
"$PY" linux/tools/build_appimage.py --work "$WORK" \
  --python "$PY" --resources "$WORK/resources" \
  --appdir "$WORK/Velox.AppDir" --output "$WORK/Velox-linux-x86_64.AppImage" \
  --sources-output "$WORK/Velox-linux-corresponding-source.tar"
```

Choose fresh AppDir and output paths. The builder refuses to overwrite an existing
package. It assembles an explicit resource allowlist, regenerates the protocol and
translation files, checks embedded-runtime imports and records relative ABI and
dependency inventories. The AppImage tool and its runtime are both pinned.

The separate corresponding-source archive contains the exact upstream Qt/PySide,
bundled Debian library, native controls and AppImage runtime sources, including
the LGPL libfuse source and upstream runtime patches. Its lock file records the
archive origins and checksums; `SOURCE-BUILD.md` explains rebuilding and relinking.
Include this archive with a binary release. Source license notices, original package licenses and native
component notices remain inside the AppImage. Extracting the image allows users
to replace dynamically linked LGPL libraries with compatible modified versions.

## Validate and publish

Run source checks, lint and formatting, test the actual packaged runtime on both
X11 and Wayland, and inspect all ELF ABI requirements. Exercise clean setup, KVM,
input, recovery, updates and English/German layouts using isolated data folders.
Check the final image for keys, SDK images, personal phone data, caches and test
artifacts before publishing.

Official Linux updates use the existing ECDSA release key and a separate Linux
manifest. A fork must replace both its update-feed URL and its public verification
key before distributing independent updates. Keep every private signing key out
of Git and release archives. See [the release policy](BUILD.md).

Official maintainers sign the Linux feed on Windows with the existing external
ECDSA key after validating the finished AppImage:

```powershell
pwsh -File release/sign-linux-update.ps1 -AppImage dist/Velox-linux-x86_64.AppImage -DownloadBase https://github.com/OWNER/REPO/releases/download/vVERSION/ -KeyFile D:/private/update-signing.key -Output dist/update-linux.txt
```
