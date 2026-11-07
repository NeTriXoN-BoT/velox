# Architecture

Velox is a native Windows launcher around Google's Android Emulator. It does not replace the virtualization engine or claim to implement Android itself.

| Area | Source |
| :-- | :-- |
| Desktop UI and settings flow | `src/main.cpp`, `src/localization.*` |
| Settings, host detection and owned phone lifecycle | `src/phone.*` |
| First-run verified downloads and AVD setup | `src/setup.*`, `src/net.*` |
| Toolbar, input editor and mapped controls | `src/playtools.*` |
| XInput, DirectInput and Android gamepad reports | `src/controllers.*`, `src/studio.*` |
| Game-specific session preferences | `src/presets.*` |
| Catalog and Android preference synchronization | `src/experience.*`, `companion/` |
| Backup, staged restore and Undo | `src/vault.*` |
| Independent phones and combined resource budget | `src/instances.*` |
| Boot screens, wallpapers, logo, cursors and frame | `src/art.*`, `src/overlay.*` |
| Local system monitoring | `src/sysmon.*` |
| Signed application updates | `src/update.*` |
| Wallpaper provider | `helper/` |
| Optional native navigation overlay | `navigation/` |

## Boundaries

- Launchers and guests are identified by executable path, instance metadata, process ID and creation time. A matching window title or emulator port alone does not establish ownership.
- Additional phones share read-only SDK components while keeping writable AVDs and preferences separate. Admission is serialized and accounts for running and starting guests.
- Input hooks are scoped to the active phone. Loss of focus, disconnection and EOF neutralize gamepad reports and release mapped touches.
- Android bridge providers require `android.permission.DUMP`. Privileged shell input uses the already-authorized ADB shell through a private stdin pipe, with no new network listener. The embedded APK is checked against the installed APK before use.
- Restore verifies checksums, SDK identity, paths and disk backing references. It stages another phone and journals the settings switch so the previous phone remains recoverable.
- Downloads are HTTPS, pinned and checked. The updater requires an ECDSA P-256 signature and matching payload size/hash. The private signing key is never shipped.
- Large work runs away from the UI thread; logs, diagnostic reads and caches are bounded. Language lookup uses a sorted immutable catalog.

See [companion details](../companion/README.md) for Android Home, Settings, controller and input protocols. See [navigation](../navigation/README.md) for the image-specific reversed-button overlay.

## Removing an additional phone

Instances requires the target to be stopped with no controls process. It acquires
the allocation gate and the target launcher mutex, then rechecks ownership. Only
a validated additional-instance directory is passed to Windows file operations;
the main profile, shared SDK, other phones' storage and linked paths are protected.
The operation explicitly requests the Recycle Bin and rejects a permanent-delete
callback. No direct permanent-delete fallback is used. Restoring the folder in
Windows brings it back to the catalog. On startup, duplicate restored ports are
reassigned under the allocation gate. Excess restored profiles remain visible for
cleanup, while the normal eight-additional-phone startup limit still applies.
