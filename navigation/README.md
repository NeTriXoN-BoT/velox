# Native navigation order

The optional `VeloxNavOrder.apk` is a resource-only overlay for the pinned Android 15
Google Play image's Pixel Launcher. It changes only the native navigation button
container to RTL, producing Recents / Home / Back. Home-screen content and app
content keep their normal direction. It contains no executable Android code.

Pixel Launcher draws the unified phone navigation bar with a hard-coded button
order; the old `sysui_nav_bar` setting and SystemUI string overrides do not change it.

Root is required. On first selection, Velox installs its own small Magisk module in
`/data/adb/modules/velox-navigation` and offers Restart to apply. Later switches
enable/disable only this overlay and recreate the launcher process; active games
are retained. Gestures and the default Back / Home / Recents layout remain available.

The stock binary layout is compared before first setup. A boot guard also checks
the pinned launcher APK checksum and avoids mounting the overlay with an updated
or different launcher. This protects resource IDs and layouts from mismatched versions.
Do not remove these compatibility checks.

## Rebuilding

Normal Velox builds embed the signed APK already in this directory. To update the
overlay for a different launcher, obtain that image's launcher APK and run:

```
powershell -ExecutionPolicy Bypass -File navigation\build.ps1 -LauncherApk <launcher.apk> -Sdk <Android SDK>
```

Requires the existing SDK build-tools 35.0.0, Java, Python (standard library only)
and the local helper signing key. No keys are included in the output. The builder
updates the stock XML, patched XML, guard checksum and signed APK. Validate both
button orders, gestures and rotation before shipping a new launcher version.

Android's binary XML requires attributes sorted by framework resource ID. The
patcher preserves this order and attribute shortcuts while repurposing the single
horizontal-orientation attribute (the default is already horizontal).

The layout derives from the Android Open Source Project's Launcher3 taskbar layout,
under Apache 2.0; see LICENSE.txt. The change reverses the navigation container only.
Source: https://android.googlesource.com/platform/packages/apps/Launcher3/
