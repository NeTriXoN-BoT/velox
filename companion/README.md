# Velox Home and gaming controls

Velox's Android companion is built from native Java/Android views without Gradle or
external UI libraries. It provides Home, Settings, app catalog/icons and the desktop
control connection. It has no network permission, telemetry or accessibility service.

## Using the experience

- **Library** shows real launchable apps. Search, star favorites, or use Recent.
  Right-click an app to mark it as a game when Android hasn't classified it.
  Opening an app while the phone is stopped starts the phone first.
- **Controls** opens Control Studio on the phone. Click to place a tap and press its
  key; drag bindings to reposition them. Right-click to add a WASD joystick, swipe
  or mouse aim. Swipe destinations and mouse-button bindings are in that menu.
- Enable mapping on Controls or the toolbar. **F8** toggles mouse aim capture;
  **Esc** releases capture and **F10** closes the editor. Controls save automatically
  per Android package and portrait/landscape layout. At most 14 bindings are allowed.
- Mapped keys are intercepted only while our phone is focused. Focus loss, profile
  changes, disabling mapping, shutdown or a disconnected input stream cancel touches.
- The side toolbar can be collapsed with its V button. Right-click to change its dock,
  reorder a button, or show the original Android Emulator controls. More actions stay
  available through its menu on small phone windows.
- On Controls, **Velox Home** previews Home and **Use as default Home** selects it.
  This is explicit: installing the companion preserves the existing default Home choice.
  Android's default Home settings remain available in Velox Settings to change it back.
- Home supports search, Games, Favorites and Recent. Hold an app to favorite it,
  classify it as a game or open its Android app information.
- Velox Settings changes Home appearance locally. Navigation and automatic rotation
  synchronize with desktop Velox while connected. Reverse button order still needs
  Root and its first setup restart on the pinned image.

## Architecture

- **Multiple running phones:** Instances creates independent AVDs, launchers and
  profile folders under `data/instances/<id>/`, sharing the selected SDK. The primary
  phone keeps its existing settings and AVD. Each process has its own launcher mutex,
  connection port, controls and captures. Process birth times and recorded ownership
  prevent a matching foreign window from being adopted. A serialized allocation check
  includes live and starting guests and rejects excessive combined CPU/RAM requests.
  Closing controls leaves the guest alive; reopening adopts the matching phone only.
  The main window manages updates after other controls windows have closed.

- **Game presets:** the card gear or library context menu opens Velox Game Presets.
  Display, FPS (30–144), CPU, RAM and graphics form a per-game session; fullscreen
  applies after launch. Hardware changes require a restart review. Global choices
  are not overwritten. Files live in `data/presets/<package>.ini`.
- **Controller Studio:** Controls > Controllers selects a Windows controller and
  native Android gamepad or touch mapping. Native mode sends standard HID gamepad
  reports through the authorized Android shell; no Root, driver install or network
  listener is required. The companion confirms Android's framework has registered
  the device before reports start. EOF removes it. Touch mode supports both sticks
  and controller-button bindings alongside keyboard/mouse contacts. Each package
  and orientation saves its own mode, device, layout, deadzone and sensitivity.
- Native controllers expose one gamepad to Android. Rumble, host motion sensors and
  multiplayer gamepad routing are not implemented. Hardware layouts vary; select a
  layout and check the live preview. XInput is preferred by Automatic selection.
- Touch sticks target on-screen joystick regions. Keyboard/mouse aim also supports
  drag-to-look games. Controller mappings and preferences are private to each phone.
- **Recovery:** `src/vault.cpp` stores cold, checksummed `.vbackup` folders, stages
  a separate phone against an exact SDK-image fingerprint and journals the desktop
  settings/profile switch for Undo and crash recovery. It excludes snapshots,
  caches, logs and process metadata. It keeps both original and recovered phone data.
  Recovery regenerates `version_num.cache` from the fingerprinted `build.prop`:
  the emulator recreates writable overlays when that image identity is missing.
  Large checksums are cancellable; shared vault names include a process suffix.

- `src/experience.cpp`: signed companion deployment, cached app catalog/icons, launch
  and favorites, and synchronization with revision checks against concurrent edits.
- `src/playtools.cpp`: Windows toolbar, editor, profiles, focus-scoped input hooks and
  persistent input pipe. Coordinates are normalized to the phone display, not Windows
  desktop pixels. Joystick distances preserve their aspect ratio.
- `BridgeProvider`: app metadata and preferences behind `android.permission.DUMP`.
  Other Android apps can't call this provider. Desktop launches use the authorized
  shell because Android 15 blocks background activity starts from providers.
- `InputBridge`: runs through `app_process` as the authorized adb shell. It injects
  multi-pointer MotionEvents over a private stdin pipe, rather than starting adb for
  each tap. It opens no network listener. EOF cancels all contacts. The ordinary Home
  app itself does not have input-injection permission.
- Profiles are in `data/controls/<package>-portrait.ini` and `-landscape.ini`.
  Toolbar/synchronization preferences are in `data/experience.ini`; cached app metadata
  is in `data/library.tsv` and `data/app-icons`. Existing launcher settings stay separate.

## Build and validation

Normal `gaming-phone/build.ps1` builds and embeds the companion APK (resource 207).
The existing SDK 35 tools, JDK and helper signing key are required. Source hashing
keeps companion deployment tied to this build. Before any provider or privileged
shell use, the installed APK SHA-256 must match the embedded APK exactly; a build
identifier or package name alone is insufficient. Mismatches use a signed package
update and incompatible signatures fail closed. No signing keys are embedded or shipped.

Validate the real app catalog, icons, launch, favorites and manual game labels; native
Home/Settings in portrait and landscape; both native button orders and gestures with
Velox Home selected; multi-touch down/move/up/cancel; and editor movement/focus safety.
Use an owned hidden test phone and the posted-message/offline UI helpers. Do not move
the owner's mouse or take foreground focus to test mouse capture.

Native app metadata is supplied by Android and may use the user's language. Velox's
own interface offers English and German. Some games may restrict emulators or mapped input; this
feature does not bypass their restrictions.

Velox offers English and German in desktop System and in Velox Settings. The per-phone language uses the same revision-checked bridge as orientation and navigation. Switching languages refreshes visible controls without changing Android system locale or third-party app names. Desktop resolution in the performance summary is the current mode of the screen containing Velox; recommendations leave Windows headroom and subtract allocations for other active Velox phones. The toolbar Screen resolution action opens Display.
