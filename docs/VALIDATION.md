# Release validation

Velox 2.6.0 was reviewed and tested on an x64 Windows 11 host using isolated application folders, Android profiles and private test desktops. These are clean application/profile tests on the existing OS, rather than certification across all hardware or a newly installed Windows VM.

| Area | Result |
| :-- | :-- |
| Native review | All 16 native modules passed GCC static analysis without diagnostics. Review covered changed preferences, DPI/chrome calculations, window/process ownership, callbacks, input preservation, resource lifetimes, background work and distribution boundaries. |
| Scaling model | 148,104 cases cover small desktops, ultrawide/4K/8K dimensions, 100–400% Windows DPI, manual overrides, usable desktop limits and both phone orientations. |
| Desktop UI | All 11 pages, setup, management views and both scaling entry points checked at 75/90/100/110/125/150/175/200/250/300% in English/German. Clickable controls remain within the layout without overlaps. |
| Native controls | Actual window reflow, effective percentage, input retention, off-screen recovery, dialog DPI notifications and repeated scale changes passed. GDI handles remained bounded; dark search/number/name fields, Unicode, Enter/Escape and confirmation cancellation passed. |
| Real phone | Final app passed Auto, 25% and oversized phone settings, fit within the desktop, retained manual resizing, fitted portrait/landscape without oscillation and preserved Android resolution/density. |
| Preferences | Independent window settings round-tripped; malformed values fell back to Auto. Window percentages do not change Android snapshot signatures or require a guest restart. |
| Regression | Hardware recommendations/64-unit launch routing, controller filtering and mapped contacts, per-game presets, backup path validation, restore metadata/SDK checks, Recycle Bin removal/restore, shared-storage protection, scheduled backup budgets and engine-minimum RAM accounting passed. |
| Fresh phone | Final app automatically cold-booted a fresh independent Android phone, retained it when controls closed, stopped only that phone and respected disabled automatic startup. Google download/checksum/extraction was validated in the earlier clean setup and is unchanged. |
| Installer | English/German wizard, per-user install, exact payload, upgrade, launch and uninstall passed, retaining settings and phone folders. |
| Source build | A fresh copy of the public tracked files rebuilt with fresh local Android signing material, without private keys, cached builds or writable phones. |
| Distribution | ZIP contains 15 payload files and passed 303 integrity, allowlist, resource and privacy checks. Public source and release scans exclude personal paths, signing secrets, credentials, development records, SDKs, AVDs and production image metadata. Defender reported no threats in the staged executable, installer and ZIP with remediation disabled. |

Automatic fitting only targets a verified owned phone window, waits for geometry to settle and does not run during a native move/size operation. The review removed idle scaling allocations, repeated toolbar resize calls and duplicate editor resize calls. Dialogs follow their own display in Auto mode. Manual choices are retained when their current effective size must be smaller.

The preceding 2.5.0 validation also covered real cold clones, storage moves/Undo, audio-session isolation, live background FPS restoration, layout import/Undo/rollback, scheduled cold backups, startup cancellation/repair, Android Home/Settings, input and backup/restore. Those broader results remain historical evidence for the unchanged features, rather than a claim that every scenario or hardware combination was repeated for scaling.

Physical mixed-DPI multi-monitor transitions were not available for direct validation; the work-area/DPI model and actual notification/reflow paths were checked separately. Real checks have reached 32 online CPUs/32 GB RAM. A true 64-CPU/64-GB guest, all controller models, GPU/audio drivers, games, firmware changes and a WHP enable/reboot cycle remain unvalidated. Input/controller checks use controlled reports and processes. Instant start is optional and off by default; its entire snapshot lifecycle is not certified.

A clean scan is evidence for the tested build and cases, rather than a guarantee of no vulnerabilities. Windows binaries remain unsigned; see the [download verification guide](VERIFY-DOWNLOADS.md). See the [scaling guide](DISPLAY-SCALING.md) for percentage semantics and limits.
