# Verify a Velox download before opening it

[Deutsch](VERIFY-DOWNLOADS.de.md) · [Official downloads](https://github.com/NeTriXoN-BoT/velox/releases/latest)

## Why Windows may warn you

The Windows installer and `Velox.exe` currently have **no Authenticode publisher signature**. Windows may display **Unknown publisher** or a SmartScreen **Windows protected your PC** message. This is expected for these unsigned releases, but the warning alone does not establish whether a file is safe. Velox's signed application-update feed is separate from Windows publisher verification.

## 1. Check the source and release version

Use the release page at **https://github.com/NeTriXoN-BoT/velox/releases**. Check the account and repository name before downloading.

For version **2.5.0**, choose `Velox-Setup-2.5.0.exe` or `Velox-2.5.0.zip`. Download **SHA256SUMS.txt from that same release**. Open that text file in Notepad; each line contains a SHA-256 value followed by its filename. The matching `.sha256` sidecar is an alternative for the installer or ZIP.

For another version, use that version's filenames and checksum file. GitHub's automatic **Source code** archives are for building the project, not the portable app download.

## 2. Calculate the downloaded file's SHA-256

Open the download folder in File Explorer. Type `powershell` in its address bar and press Enter to open PowerShell in that folder. Administrator rights are not required. If the file is elsewhere, use its actual path inside the quotes.

For the **installer**, run:

```powershell
Get-FileHash -LiteralPath '.\Velox-Setup-2.5.0.exe' -Algorithm SHA256 | Format-List Algorithm,Hash
```

For the **portable ZIP**, run:

```powershell
Get-FileHash -LiteralPath '.\Velox-2.5.0.zip' -Algorithm SHA256 | Format-List Algorithm,Hash
```

Run only the command for your downloaded file. These commands read the file; they do not run the installer or app. `Format-List` displays the complete hash rather than a shortened table column.

Compare **all 64 characters** under `Hash` with the line for that exact filename in the release's `SHA256SUMS.txt`. Upper/lowercase differences do not matter.

- **Match:** the downloaded file matches the published checksum.
- **Mismatch or missing entry:** stop. Do not run the installer or app. Download again from the official release; if the mismatch persists, [report it](https://github.com/NeTriXoN-BoT/velox/issues).
- **File not found:** check its folder and filename. A renamed download, such as one ending in `(1)`, needs that actual filename in the command.

A checksum verifies agreement with the published file. It is not a malware scan, an independently verified publisher identity, or proof that software is safe. Obtain both file and checksum from the official project; an untrusted mirror providing both is insufficient.

For a portable install, check the ZIP **before extracting it**, then extract into its own folder. The ZIP also contains a separate `SHA256SUMS.txt` for the extracted payload. You can additionally check the extracted app with `Get-FileHash -LiteralPath '.\Velox.exe' -Algorithm SHA256 | Format-List Algorithm,Hash` from that folder and compare against its `Velox.exe` entry.

## 3. Decide whether to proceed with the Windows warning

After verifying the source and hash, open the installer, or open the extracted `Velox.exe` for a portable install. If you trust the project and SmartScreen shows an unrecognized-app warning, select **More info → Run anyway**, **if Windows offers those controls**. An **Unknown publisher** label remains expected; checking a hash does not change it.

Do not disable Microsoft Defender, SmartScreen or Smart App Control to install Velox. If Windows provides no option to continue, a security or administrator policy may be blocking the unsigned file. Respect that restriction; on a managed PC, contact your administrator. A specific antivirus threat detection is different from an unrecognized-publisher warning: stop and report the detection for investigation rather than adding an exclusion.

Microsoft references: [SmartScreen reputation and unsigned files](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/smartscreen-reputation) · [PowerShell Get-FileHash](https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.utility/get-filehash).
