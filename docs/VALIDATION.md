# Release validation

Velox 2.5.0 was reviewed and tested on an x64 Windows 11 host with isolated application folders, Android profiles and private test desktops. These are clean application/profile tests on the existing OS, rather than certification across all hardware or a newly installed Windows VM.

| Area | Result |
| :-- | :-- |
| Native review | All 16 C++ modules passed GCC static analysis without diagnostics. Manual review covered ownership, filesystem boundaries, networking/updating, archives, input, timers and resource handling. |
| Android/scripts | Java components rebuilt; provider permissions, IPC validation, launch/extraction scripts and packaging reviewed. Overflowing preference revisions were rejected without changing preferences. |
| UI | All 11 main pages, setup and six management views checked at 100/125/150/200% in English/German. Dark name fields, Unicode, Enter/Escape, dialog save/cancel and native search/number fields passed. |
| Closing | Pending stop completes before closing; repeated close requests cannot skip background-state restoration. |
| Templates/clones | Fresh templates retained settings/layouts with new Android data. A separate-process full clone booted with its installed app and a newly written data marker. |
| Storage | Cleanup preserved the save-disk checksum. A moved copy booted with its data marker; Undo booted the earlier state and retained both copies. Missing paths and cross-profile aliases were rejected before maintenance; working-folder sentinels stayed unchanged. |
| Background | Actual Android settings preserved live FPS, applied a limit and restored the latest live choice. Real silent Windows audio sessions confirmed owned-session mute/restoration and an unchanged foreign session. |
| Sharing | Both orientations/preset round-tripped. Malformed, oversized, traversal, nonfinite and changed-package files were rejected. Undo and initialization rollback of an interrupted three-file import passed. |
| Schedules | Real SHA-256 cold backup passed. Count/GB caps were independently checked with explicit pause/resume, no deleted recovery points and no interruption of active games. |
| Startup care | Owned-startup cancellation, non-destructive repair and the following full boot passed. |
| Resources | Recommendation matrix, 64-unit settings/launch routing, disabled options, engine-minimum RAM reservation and combined allocations passed; saved targets remained intact. |
| Instances | Final app ran two guests with separate toolbars and independent shutdown. Synthetic removal/restore, ownership guards, links and reused-port recovery passed. |
| Android experience | Real bilingual Home/Settings, orientation, preference sync, non-root HID reports and simultaneous mapped touches passed. Android system language stayed unchanged. |
| Vault | Backup/verify, corruption/SDK rejection, backing-chain checks, sealed restore, retained-app boot, exact-settings Undo and interrupted rollback passed. |
| First start | Final app cold-booted a fresh phone, retained it after closing controls and respected automatic-start disabled. Google download/checksum/extraction was validated in the earlier clean setup and is unchanged. |
| Installer | English/German wizard, per-user install, exact payload, upgrade, launch and uninstall passed while retaining settings/data. |
| Distribution | ZIP contains 15 payload files and passed 303 integrity, allowlist, resource and privacy checks. Defender reported no threats in the staged app, installer and ZIP with remediation disabled. |

The review fixed missing/shared-storage handling, interrupted imports, overflowing IPC values, minimum-RAM reservation accounting, live-FPS restoration and close/restoration races. Source/distribution scans exclude personal data, private keys, development records, SDKs, phone images and production image metadata.

Earlier real checks reached 32 online CPUs/32 GB RAM. A forced 64-CPU request on the available host exposed 32, which Velox detected and cached. A true 64-CPU/64-GB guest, physical controller models, every GPU/audio driver, every game, firmware changes and a WHP enable/reboot cycle remain unvalidated. Input/audio isolation used controlled reports/processes, rather than certifying all physical devices. Instant start is optional and off by default; its entire snapshot lifecycle is not certified. Guest allocations are targets; process/graphics overhead and performance vary.

A clean scan is evidence for the tested build and cases, rather than a guarantee of no vulnerabilities. Windows binaries remain unsigned; see the [download verification guide](VERIFY-DOWNLOADS.md).
