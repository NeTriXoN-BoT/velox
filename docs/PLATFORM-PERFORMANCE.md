# Platform performance

Velox uses the native capabilities of each operating system. Windows uses Windows Hypervisor Platform (WHPX); Linux uses KVM. Both accelerate Android's CPU execution in hardware. Graphics rendering uses the host driver through the Android Emulator. This does not require exclusive GPU passthrough.

The Linux AppImage targets modern x86-64 glibc desktops with KVM and supports X11 and Wayland. See the [Linux guide](LINUX.md) for runtime requirements and tested coverage. Performance and sensor availability depend on the host driver and compositor.

| Capability | Windows | Linux |
| --- | --- | --- |
| CPU acceleration | WHPX | KVM, checked through the device API |
| Graphics selection | SDK Automatic, explicit GPU or software | SDK Automatic, explicit GPU or software |
| Desktop display | Native emulator window | OpenGL view with shared-memory frames; raster and RPC fallbacks |
| Input | Focus-scoped Windows hooks; XInput and DirectInput controllers | Local keyboard input, SDL2 controllers, X11 capture and Wayland relative-pointer protocols |
| Background audio | Per-phone Windows audio session | Per-phone PulseAudio or PipeWire-Pulse stream |
| Optional focus boost | Above Normal priority, restored when focus leaves | Installed GameMode service through its PID-fd API |
| Resource limits | Available host threads, memory headroom and observed guest capacity | CPU affinity, cgroup limits, memory headroom and observed guest capacity |

## Choosing settings

Use **Recommended** for a plan suited to the detected hardware and desktop. CPU and memory remain configurable up to 64, subject to the system's capacity. More virtual CPUs or RAM do not automatically improve a game's performance. Leave room for the desktop, graphics driver and other running phones.

Automatic allocations reserve less memory on systems where graphics shares system RAM. Linux recalculates its startup allocation using current memory pressure and other active phones. An observed guest CPU limit disables unsupported choices for that SDK.

**Automatic graphics** lets the SDK select a compatible renderer. Velox reports the active Android renderer separately from the host graphics card. If Automatic falls back to software rendering, Velox temporarily uses 30 Hz to reduce CPU demand. The preferred foreground rate remains saved for the next hardware-rendered boot. An explicit software choice retains its configured rate.

Use the background frame limit and background mute options when running several phones. The Android display uses a safe 30 Hz minimum; lower per-game frame limits use Android's game pacing mechanism. Temporary game interventions are restored when controls close. A game's explicit frame-rate opt-out remains authoritative.

Focus boost is optional. Windows changes only a normal-priority Velox phone process, respects a later manual priority change and records its own changes for recovery after interrupted controls. Linux uses the installed GameMode service without editing its configuration. Existing registrations from other tools are left alone. Interrupted Linux requests have a private recovery record tied to the exact phone process and D-Bus service; a later controls session retries cleanup.

Full shutdown restores temporary limits before asking Android's framework to power off, allowing services to save pending preferences. Instant start instead asks the SDK to save the running VM. A restart waits for Android's storage lock to be released.

The System page samples host usage off the UI thread. Its sensor view includes VRAM usage, temperatures, clocks, power and fans when a supported source supplies them. Windows uses native performance counters and the installed NVIDIA driver's NVML interface; an already running Libre Hardware Monitor or Open Hardware Monitor can supply additional CPU and GPU sensors through WMI. Linux reads kernel telemetry and the installed graphics driver's read-only interfaces. Sensor support varies by hardware and permissions. Missing or stale values show as unavailable, and Velox installs no monitoring driver.

Windows phone taskbar properties point back to Velox and the selected phone. The SDK executable and internal emulator window title stay intact. The custom dock follows native geometry events; launcher UI paints reuse their surface and consume cached engine observations while discovery runs in a worker.

## Compatibility and measurement

Keep the graphics driver current and enable CPU virtualization. KVM permissions are configured for the regular desktop account; Velox must not run with sudo. Linux relative mouse capture requires compositor support for pointer constraints and relative-pointer protocols. Right-button drag remains available when capture is unavailable.

Performance depends on the game, hardware, driver and virtualization configuration. Neither operating system is inherently fastest for every game. Windows and Linux hardware paths need their own measurements; a software-rendered Linux test is not a hardware GPU benchmark. Physical controller models and distribution-specific driver combinations also need testing before claiming compatibility.

See Google's [Android Emulator acceleration documentation](https://developer.android.com/studio/run/emulator-acceleration) and the [GameMode project](https://github.com/FeralInteractive/gamemode) for the underlying platform requirements.
