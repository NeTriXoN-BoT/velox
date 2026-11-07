VELOX
Android gaming phone for your PC
====================================================================

Velox runs a real Android 15 phone (with Google Play) on Windows, tuned
for games: up to 144 Hz, up to 64 virtual CPU cores, GPU graphics, root on demand,
custom cursors, wallpapers and a live system monitor.


WHAT YOU NEED
- An up-to-date x64 version of Windows 10 or 11 on an Intel/AMD PC
- Virtualization switched on (most PCs have it on; if not, Velox tells you
  and can turn on "Windows Hypervisor Platform" for you, then restart)
- About 10 GB free disk space
- Internet for the first start (Velox downloads Android from Google, 2.2 GB)


GETTING STARTED
1. Run Velox-Setup to install for your Windows account, or unzip this folder
   somewhere you can write to, for example
   Documents\Velox or D:\Games\Velox (not inside "Program Files").
2. Open Velox.exe.
3. Read and accept the Android SDK License, then press
   "Download and set up". Every file comes straight from dl.google.com
   and is checked against Google's official checksum.
4. The phone starts automatically when setup finishes. The first start takes
   a little longer. Start phone is also available at any time.
5. Sign in to Google Play on the phone and install your games.

Velox picks the best settings for your PC on the first start. You can
change them any time (Display, Performance) or press "Use recommended".


TIPS
- Instances: New phone creates an independent Android phone using the SDK already
  installed. Run the main phone and up to eight additional phones as resources allow.
  Each new phone starts with 2 cores, 2 GB RAM and 60 FPS; adjust its own Performance
  page. Combined allocations reserve RAM for Windows and stay within the CPU budget.
- Open controls opens that phone's Velox window. Closing its controls leaves Android
  running; reopening reconnects to that same phone. Stop phone affects only that phone.
  Apps, accounts, saves, presets, controls, preferences and captures stay separate.
- The phones share the SDK files. Creating another phone does not download Android
  again. Updates are managed from the main window; close other controls windows
  before installing an update. Their Android guests can remain running.
- Instances > Remove phone removes a stopped additional phone after its controls
  have closed. Review its name, then confirm Move to Recycle Bin. The main phone,
  shared SDK and files outside that instance's managed folder are protected.
  Use the Recycle Bin button, select the folder ID shown in the confirmation,
  and choose Restore to undo removal. Storage is reclaimed when you empty that
  item from the Windows Recycle Bin yourself. Keep a free additional-phone slot
  before restoring; the installation supports eight additional phones.
  A restored phone picks a free connection port if its old port has been reused.
- Library: search and open installed apps, keep favorites and see recently played
  games. Right-click an app to mark it as a game or edit its controls.
- Game presets: use a library card's gear or right-click > Game preset to choose
  a game's display, FPS, CPU/RAM, graphics and fullscreen behavior. Global settings
  stay intact. Hardware changes show a restart review before closing running apps.
- Controls > Controllers opens Controller Studio for the active game and orientation.
  Connect an Xbox-compatible or DirectInput controller through Windows USB/Bluetooth.
  Native Android gamepad mode works with games that support gamepads, without Root.
  Touch mapping works with placed controller buttons and left/right sticks in the
  phone editor's right-click menu. Tune dead zones, sensitivity and camera Y per game.
  Input is neutralized on focus loss and disconnected devices release held controls.
- Control Studio: place and drag tap, WASD joystick, swipe and mouse-aim bindings.
  Layouts save per game and orientation. F8 toggles mouse aim; Esc releases it;
  F10 closes the editor. Enable mapped controls on Controls or the side toolbar.
- Right-click the Velox side toolbar to dock, collapse or reorder its buttons.
  The original toolbar is hidden automatically; you can show it from that menu.
- Performance offers up to 64 virtual cores and 64 GB RAM when your PC has enough
  resources. Memory choices reserve at least 3 GB or a quarter of system RAM for
  Windows. Auto remains a balanced default; larger allocations need a phone restart.
- Controls > Velox Home opens the Android home screen. Use as default Home makes
  it your launcher. Velox Settings customizes Home and syncs navigation/rotation
  with desktop Velox; it also links to the original Android settings.
- Display > Android navigation offers Back/Home/Recents (default),
  Recents/Home/Back, and Gestures. Reversed buttons need Root and an initial
  setup restart on this Android 15 image.
- The emulator's controls start in Dark theme on first use. You can change
  this later in its extended controls under Settings > General.
- Display > Automatic rotation is on by default: the phone follows each
  game's portrait or landscape orientation, even while Velox is minimized.
  Turn it off for manual control; use Rotate on Overview or the toolbar menu.
- Drop APK, APKS, XAPK or APKM files on Velox to install them. Split app
  bundles select compatible CPU and density files. Included OBB game data
  is copied to Android/obb/<package>. Standalone OBB files need their normal
  main.<version>.<package>.obb or patch.<version>.<package>.obb filename.
- Other files (ROMs, ZIPs, saves, images, music and documents) go to the
  phone's Download folder for Android apps to open. Developer AAB files
  need to be exported as APK or APKS before installation.
- Look: pick a cursor for the phone, one of five boot screens, and a wallpaper. Add your own
  pictures with Add pictures (or put them in a wallpapers folder next to
  Velox.exe); Velox can add its logo to them (Badge, Signature, Poster).
- Phone > Root: optional real root with Magisk for testing. It downloads
  Magisk from its official GitHub release and only patches a copy of the
  phone's boot image. Banking apps and some games refuse rooted phones.
- System: live CPU, GPU and memory usage of your PC and the phone.
- Screenshots and recordings are saved to Pictures\Velox. Recordings run for up
  to one hour. Closing Velox leaves the phone and recording running; reopen
  Velox and press Stop recording to finish the clip.
- Look includes four original Velox-chan wallpapers with everyday outfits: After
  Hours, Blue Hour, Stargazer and Windowlight. Use the arrows to see all designs.


BACKUP AND RECOVERY
- Recovery > Create backup makes a complete, verified copy while the phone is off.
  Choose a local vault folder with enough space. A recovery point is a .vbackup
  FOLDER: keep or copy that whole folder, including manifest.vlx and all contents.
- Backups include apps, accounts, app data and saves, phone preferences, touch and
  controller layouts, game presets and the patched boot image when Root is enabled.
  They are private and NOT encrypted. Store them as carefully as the phone itself.
- Browse backup selects a .vbackup folder. Verify files checks every payload hash.
  Restoring requires the matching Android system image in the selected SDK.
- Restore as new phone prepares and checks a separate phone inside the vault.
  Activation switches phone preferences, controls and presets. The previous phone
  remains on disk; Undo last restore returns to its settings and data.
- Snapshots and caches are rebuilt. Restored phones start with a cold boot.
  Google sign-in, network services or individual games may require reauthentication.
- If recovery is interrupted, Velox rolls the desktop switch back on the next start.
  Diagnostics provides a bounded local log for review; it is never sent automatically.
- Recovery belongs to the selected phone; other running instances keep their data.
  To copy a phone, stop it and create a backup, then open another phone's controls
  and restore that backup as a new phone. This also copies its apps and accounts.
- Removing Velox does not remove backups or restored phones stored in another folder.


UPDATES
The Updates page shows your version and changelog. Automatic updates need
a configured publisher update service; downloaded updates must carry the
publisher's signature. When no service is configured, get a newer release
ZIP and replace Velox.exe while keeping your data folder.


PRIVACY AND SAFETY
- Velox sends no telemetry. The emulator's own usage statistics and crash
  uploads are switched off.
- The phone is only reachable from this PC (adb and the emulator console
  listen on this PC only).
- Velox never needs admin rights, except the one optional step that turns
  on Windows Hypervisor Platform (Windows shows its normal prompt).


REMOVING VELOX
For an installed copy, use Windows Settings > Apps > Velox > Uninstall.
Your phone data and settings are retained. For a portable copy, close Velox
and the phone, then delete its folder only when you no longer need its data.
Velox keeps its settings
in the "data" folder inside it (or in %APPDATA%\Velox if this folder can't be
written).

Choose English or Deutsch at System (or during setup). Velox Settings on the
phone offers the same choice and synchronizes it with desktop Velox. This
changes Velox only, not Android system language or app names.

Performance detects the current desktop resolution and refresh rate. Use
recommended considers CPU threads, dedicated/shared graphics, available RAM
and other running Velox phones, while reserving memory for Windows. These
are safe starting points, not a game-specific benchmark. Screen resolution
is also available from the phone toolbar (More controls on short screens).

Unavailable CPU/RAM choices are disabled. CPU limits use the detected logical
threads; the summary shows physical cores and logical threads separately.
RAM limits reserve Windows headroom. Imported higher allocations resolve to
the nearest supported lower choice. Auto remains conservative (up to 6 CPUs
and 8 GB); 64 GB requires a host with enough memory left for Windows.

New installations select English/German from setup or the Windows UI language,
apply safe hardware recommendations and start the main phone automatically
after Android setup. Phone > Start phone with Velox changes this behavior.
Existing configurations keep their previous startup behavior. Open controls
for another instance does not start that instance automatically.

Velox checks the CPUs actually exposed by Android. If the engine/image limits
the request, unsupported choices are disabled and the allocation is corrected.
This capability cache is bound to the SDK kernel, engine version and host.

Google packages are downloaded automatically after license acceptance on the
first start, not redistributed inside this package. Velox uses verified tested
versions. Signed Velox application updates are delivered through GitHub releases.
Installed SDK files are not silently replaced; future SDK changes are tested
and delivered through Velox releases.

Project, source, downloads and issue tracker:
https://github.com/NeTriXoN-BoT/velox
