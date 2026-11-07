# Release validation

Velox 2.4.0 was checked on an x64 Windows 11 host using isolated application folders, Android profiles and private test desktops. This is a clean application/profile test on the existing OS, not a newly installed Windows VM or a certification across different hardware.

| Area | Result |
| :-- | :-- |
| Native static analysis | All 16 C++ modules completed GCC analysis with zero diagnostics. |
| UI | All 11 desktop pages and setup checked in English/German at 100%, 125%, 150% and 200%; dialog save/close, native text editing and unsupported options exercised. |
| Hardware choices | Recommendation matrix, combined allocations, fallback graphics, 64-unit settings/presets and exact launch parameters checked. |
| Android capacity | Real guests validated through 32 online CPUs/32 GB RAM. A forced 64-CPU request on the available host exposed 32; Velox detected, cached and applied that actual limit. |
| Android experience | Real Home/Settings instrumentation, portrait/landscape, search/favorites, live language changes and two-way settings synchronization passed. Android system language remained unchanged. |
| Input | Real non-root Android HID registration, button down/up, axes, triggers, neutralization and detach; simultaneous mapped touch contacts and releases passed. |
| Independent phones | Two actual guests with separate apps/data, independent stop/restart, retained data, reopen/adoption and combined resource guards passed. Matching foreign windows/processes were rejected. |
| Instance removal | A synthetic profile was sent to the real Windows Recycle Bin and restored with exact bytes retained. Main/current IDs, open launcher mutexes, a live owned runtime without controls, embedded SDK paths and nested links were rejected. |
| Restored instances | Reused connection ports were reassigned under the allocation gate. Main settings, shared SDK and external marker files remained intact. |
| Recovery | Complete cold backup, corruption rejection, SDK mismatch rejection, disk backing checks, immutable staged restore, boot with retained app, exact settings Undo and interrupted rollback passed. |
| Startup | Final release automatically booted a new phone; closing controls retained the guest; scoped stop and disabled automatic startup passed. |
| Setup and packaging | First-run Google downloads/setup checked in isolated empty SDK/profile locations. Final installer install/reinstall/launch/uninstall passed while retaining test settings and phone data. |
| Distribution | Portable ZIP passed 303 integrity, allowlist, resource and privacy checks. Public source built with fresh local Android signing keys. Source and release privacy scans passed. |

The available tests do not establish performance on every system or compatibility with every game. A 64-online-CPU or 64-GB guest and physical Xbox/PlayStation/generic-controller models have not been validated on suitable hardware. The real guest input path was exercised with generated controller reports. Firmware virtualization changes and a Windows Hypervisor Platform enable/reboot cycle were not exercised in this final run. Instant start is optional and off by default; its entire snapshot lifecycle has not been validated end to end.
