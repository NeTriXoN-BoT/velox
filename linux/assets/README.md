# Linux runtime resources

`sdk.lock.json` pins the Google archives used by automatic Android setup. The SDK
license is shown before those files are downloaded. SDK images and personal phone
data are not part of the application package.

`patch-ramdisk.sh` adapts Magisk v31.1's AVD patcher for Velox's pinned Android 15
x86_64 image. It runs inside the selected phone, extracts only named tools from the
verified Magisk APK, accepts an original ramdisk and produces a separate patched
copy. It contains no downloader. The shared SDK image is kept intact.

The patcher is GPL-3.0-only; see `LICENSE-Magisk.txt`. Changes and their date are
marked in the script. Its upstream source is
[Magisk's versioned AVD patcher](https://github.com/topjohnwu/Magisk/blob/v31.1/scripts/avd_patch.sh).
The optional Magisk APK is fetched directly from its pinned official release;
[corresponding Magisk source](https://github.com/topjohnwu/Magisk/tree/v31.1) is available
under its own license. Root remains an explicit choice for each phone.

Companion APKs, navigation resources, translations, native input libraries and the
wallpaper gallery are assembled during the application build. The runtime checks
their SHA-256 inventory before installing or executing optional tools.
