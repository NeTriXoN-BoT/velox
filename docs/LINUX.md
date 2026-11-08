# Velox on Linux

[Deutsch](LINUX.de.md)

Velox's AppImage contains the launcher, its Python/Qt runtime, controls and original
wallpapers. Android is downloaded directly from Google after you accept the SDK
license. A separate Android SDK or Python installation is not required.

## Requirements

- An x86-64 desktop with glibc 2.34 or newer and the GCC 11 C++ runtime or newer.
- A working X11 or Wayland desktop, graphics driver, fonts and OpenGL support.
- CPU virtualization and permission for your account to open `/dev/kvm`.
- At least 6 GB of RAM; 8 GB or more is recommended. Keep at least 10 GB free.
- FUSE with `fusermount3` or `fusermount` for normal AppImage mounting.

The package is intended for modern glibc distributions, including Debian, Ubuntu,
Mint, Fedora, Arch, openSUSE and compatible enterprise desktops. It is not an ARM,
Alpine/musl or NixOS-native build. Distribution packaging and driver differences
can still affect compatibility.

The packaged UI has been checked on Debian 12 and AlmaLinux 9, including English
and German. Android setup and KVM boots, recovery, cloning and controls have been
tested on Debian 12. Native pointer capture has been tested on isolated X11 and
Wayland desktops. These checks do not cover every distribution, physical graphics
card or controller model.

## First start

Download the AppImage and `SHA256SUMS.txt` from the same GitHub release. Verify the
file before making it executable:

```sh
sha256sum --check --ignore-missing SHA256SUMS.txt
chmod +x Velox-linux-x86_64.AppImage
./Velox-linux-x86_64.AppImage
```

Start Velox as your normal account. Follow setup's virtualization, KVM and storage
checks, read Google's license and let setup finish. Downloads are pinned and
verified; interrupted Android downloads can resume. Velox releases update those
tested SDK pins together with the application.

KVM permissions are managed by your distribution. On desktops that use a `kvm`
group, adding your account normally requires signing out and back in. Use your
distribution's documented procedure; Velox does not run the desktop as root or
make `/dev/kvm` world-writable.

If FUSE is unavailable, extract and run the package in a folder you own:

```sh
mkdir -p "$HOME/Applications/Velox"
cd "$HOME/Applications/Velox"
/path/to/Velox-linux-x86_64.AppImage --appimage-extract
./squashfs-root/AppRun
```

Extraction runs the same application. In-place AppImage updating and its menu
shortcut require opening the AppImage itself. Keep extracted copies separate
from the Android data folder.

## Data and updates

Data defaults to `$XDG_DATA_HOME/velox`, or `~/.local/share/velox`. Use
`--data-dir /path/to/folder` to select another private data folder. AppImages
contain no accounts, phone data or SDK images. Closing the launcher or hiding the
phone view keeps running phones available for the next launcher session.

The Updates page checks Velox's signed Linux feed and verifies the AppImage
checksum before replacing it. Updates are an explicit choice. The previous image
is retained beside the AppImage in a private `.velox-update-*` folder. After a
successful restart you can remove that retained version when you no longer need
it. Existing phones and backups stay in their data folder.

Use **Add to application menu** on the Updates page for an optional per-account
shortcut. Keep the AppImage at the same location afterwards. Moving it requires
adding the menu shortcut again from its new location.

## Building

See [Linux build instructions](LINUX-BUILD.md). The release also provides a
corresponding-source archive for the included Qt/PySide, system libraries and AppImage runtime. Licenses and
version inventories are available inside the extracted AppImage under
`usr/share/licenses/velox` and `usr/share/velox/build-info.json`.
