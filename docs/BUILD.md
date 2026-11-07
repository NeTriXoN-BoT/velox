# Build Velox

The Windows launcher uses C++20, Win32 and GDI+. Android components use Java and SDK command-line tools; Gradle is not required.

## Prerequisites

- Windows x64 and PowerShell 7 (Windows PowerShell is also supported by the release scripts).
- MinGW-w64 GCC with static UCRT libraries, `g++` and `windres`. Set `MINGW_HOME` to its installation directory; the default is `%USERPROFILE%\Tools\mingw64`. The release was validated with GCC 16.2.
- A JDK on PATH (`java`, `javac`, `jar`, `keytool`). JDK 25 was used for the release; sources target Java 11 bytecode.
- Android SDK: **build-tools 35.0.0**, **platforms;android-35**. Set `ANDROID_SDK_ROOT` to that SDK directory. The SDK used by players is installed separately by Velox.
- Optional installer: **Inno Setup 6** with modern dark wizard support. Set `INNO_HOME` if it is outside `%USERPROFILE%\Tools\InnoSetup`.
- Python 3 for the navigation patcher and source checks.

## Local build

```powershell
$env:MINGW_HOME = 'D:\Tools\mingw64'
$env:ANDROID_SDK_ROOT = 'D:\Android\Sdk'
pwsh -NoProfile -File .\build.ps1 -Out .\build\app
```

The first build creates a **local Android signing key** in the ignored `keys/` folder. Subsequent builds reuse it. No official private signing keys are distributed. Keep your local key if you want to update Android companion apps in an existing development phone. Official and independently built APKs use different keys; test community builds in a separate phone/profile.

Normal builds embed the prebuilt resource-only navigation overlay. See [navigation](../navigation/README.md) to rebuild it for another compatible image. The overlay contains no executable Android code.

`build.ps1` without `-Out` installs into `%USERPROFILE%\Tools\Velox`; use an explicit output directory for development. Do not overwrite a running executable or your regular phone installation.

## Packaging

```powershell
pwsh -NoProfile -File .\release\make-release.ps1 -Installer
```

This makes a clean portable ZIP, per-file SHA-256 manifest, archive checksum and optional installer under `dist/`. The installer accepts English or German, requires no administrator rights, and preserves settings and phone data on upgrade/uninstall. Add `-Installer` only when Inno Setup is installed.

Official maintainers can also pass `-DownloadBase https://github.com/<owner>/<repo>/releases/download/v<version>/` to sign `update.txt` using their local update key. Never publish `keys/`. Forks should set their own `update-url.txt` and use `release/new-signing-key.ps1` to create a separate update identity before distributing their own update channel. The checked-in public key authenticates official Velox updates only.

## Checks

```powershell
python tools/check-source.py
g++ -std=c++20 -static -Wall -Wextra -I src tools/dev/orientation-test.cpp -o build/orientation-test.exe
.\build\orientation-test.exe
```

The portable orientation test also runs in CI. Additional Win32, emulator and Android instrumentation helpers are in `tools/dev/`; they require an isolated test profile and owned test phone. Do not run guest-changing tests against personal accounts or saves.

Before releasing, build with warnings enabled, run static analysis on every native module, exercise English/German and high-DPI layouts, test actual Android input and recovery, install/upgrade/uninstall in an isolated directory, and inspect the final archive and source tree for private material. See [contributing](../CONTRIBUTING.md).
