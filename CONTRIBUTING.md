# Contributing

Bug reports, focused fixes, hardware compatibility results and English/German translation improvements are welcome.

For a bug, include the Velox version, Windows version or Linux distribution/display system, relevant CPU/GPU/RAM and graphics driver, the action that triggers it, expected behavior and what happened. Include only the useful portion of logs and redact personal paths, accounts, tokens, phone numbers and app data. A screenshot without personal information is helpful for layout issues.

Build instructions are in [docs/BUILD.md](docs/BUILD.md) and [the Linux build guide](docs/LINUX-BUILD.md). Keep pull requests focused and explain the resulting behavior and validation. Native Windows code uses C++20, four-space indentation and Win32/GDI+. The Linux interface uses Python/Qt with Ruff configuration in linux/pyproject.toml. Android code uses the SDK tools without Gradle.

Preserve configured user choices, existing phone data, compatibility guards and process ownership checks. Use isolated profiles for integration tests. Validate portrait/landscape, English/German, high-DPI layouts and unsupported hardware choices when changing UI or performance settings. For input changes, verify focus loss, release/cancel and disconnect behavior. For recovery changes, prove that the original phone remains intact.

Only include source, assets and documentation needed by the project. Keep signing keys, passwords, SDKs, writable phones, local configuration, logs, caches and test outputs out of commits and release packages. Preserve third-party licenses and describe changes to third-party sources.

Contributions are accepted under the project's MIT license, except where a file's existing third-party license applies. Do not submit artwork, code or other material you lack permission to distribute.
