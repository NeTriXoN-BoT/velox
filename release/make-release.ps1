# Builds a Velox release: a zip to share with friends, and the signed update manifest that copies
# already out there check.
#
#   powershell -ExecutionPolicy Bypass -File gaming-phone\release\make-release.ps1 [-DownloadBase https://.../]
#
# -DownloadBase is the address the new Velox.exe will be downloadable from (for GitHub releases:
# https://github.com/<owner>/<repo>/releases/download/v<version>/). Without it only the zip is made.
# The update notes are this version's "- " lines in gaming-phone\CHANGELOG.txt.
#
# Every release: raise gaming-phone\VERSION, run this, then upload dist\velox\Velox.exe to
# -DownloadBase and dist\velox\update.txt to the address in gaming-phone\update-url.txt.
param([string]$DownloadBase = '', [switch]$Installer)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$dist = Join-Path $root 'dist'
$keyFile = Join-Path $root 'keys\update-signing.key'
$version = (Get-Content "$root\VERSION" -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'VERSION must be a numeric major.minor.patch version.' }
$dist = [IO.Path]::GetFullPath($dist)
New-Item -ItemType Directory -Force $dist | Out-Null
if ((Get-Item -LiteralPath $dist).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'The release directory must not be a link.' }
if ($DownloadBase -and -not $DownloadBase.EndsWith('/')) { $DownloadBase += '/' }
if ($DownloadBase -and $DownloadBase -notmatch '^https://') { throw '-DownloadBase must be an https:// address' }
if (-not (Test-Path "$root\update-url.txt")) { Write-Warning 'gaming-phone\update-url.txt is missing: this build will not check for updates.' }

# Build into a clean staging folder.
$stage = [IO.Path]::GetFullPath((Join-Path $dist "Velox-$version"))
if ([IO.Path]::GetDirectoryName($stage) -ne $dist) { throw 'Release staging path escapes dist.' }
if (Test-Path -LiteralPath $stage) {
    if ((Get-Item -LiteralPath $stage).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'The release stage must not be a link.' }
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Force $stage, "$stage\licenses\rootAVD" | Out-Null
& "$root\build.ps1" -Out $stage
Copy-Item "$PSScriptRoot\README.txt", "$PSScriptRoot\THIRD-PARTY-NOTICES.txt" $stage
Copy-Item "$root\CHANGELOG.txt" "$stage\CHANGELOG.txt"
Copy-Item "$root\LICENSE.txt" "$stage\LICENSE.txt"

# Distribution copies contain image pixels, not private production metadata.
# Original artwork and provenance records stay in the source workspace. Copy
# every retained PNG chunk byte-for-byte; no decoding or pixel re-encoding.
foreach ($image in Get-ChildItem -LiteralPath "$stage\wallpapers" -Filter '*.png' -File) {
    $bytes = [IO.File]::ReadAllBytes($image.FullName)
    if ($bytes.Length -lt 20 -or [BitConverter]::ToString($bytes,0,8) -ne '89-50-4E-47-0D-0A-1A-0A') { throw 'Invalid wallpaper PNG.' }
    $clean = New-Object IO.MemoryStream
    try {
        $clean.Write($bytes,0,8)
        $offset=8
        while ($offset -lt $bytes.Length) {
            if ($bytes.Length-$offset -lt 12) { throw 'Truncated PNG chunk.' }
            $length=[int64]$bytes[$offset]*16777216 + [int64]$bytes[$offset+1]*65536 + [int64]$bytes[$offset+2]*256 + $bytes[$offset+3]
            if ($length -gt $bytes.Length-$offset-12) { throw 'Invalid PNG chunk length.' }
            $kind=[Text.Encoding]::ASCII.GetString($bytes,$offset+4,4)
            $count=[int]($length+12)
            if ($kind -notin @('caBX','tEXt','zTXt','iTXt','eXIf','tIME')) { $clean.Write($bytes,$offset,$count) }
            $offset+=$count
        }
        [IO.File]::WriteAllBytes($image.FullName,$clean.ToArray())
    } finally { $clean.Dispose() }
}

Copy-Item "$root\third_party\rootAVD\*" "$stage\licenses\rootAVD\"
New-Item -ItemType Directory -Force "$stage\licenses\navigation" | Out-Null
Copy-Item "$root\navigation\LICENSE.txt", "$root\navigation\README.md" "$stage\licenses\navigation\"
# Never ship keys or personal data.
foreach ($bad in 'update-signing.key', 'helper.keystore', 'helper.pass', 'settings.ini') {
    if (Get-ChildItem $stage -Recurse -Filter $bad) { throw "Refusing to package $bad" }
}

# An allowlist keeps new caches, phone data and developer outputs out of shared builds.
$allowed = '^(Velox\.exe|README\.txt|LICENSE\.txt|THIRD-PARTY-NOTICES\.txt|CHANGELOG\.txt|wallpapers/[^/]+\.(png|jpg|jpeg)|licenses/rootAVD/(LICENSE|rootAVD\.sh|SOURCE\.txt)|licenses/navigation/(LICENSE\.txt|README\.md))$'
$files = @(Get-ChildItem -LiteralPath $stage -Recurse -File)
$checksums = @()
foreach ($file in $files) {
    $relative = $file.FullName.Substring($stage.Length + 1).Replace('\','/')
    if ($relative -notmatch $allowed -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Unexpected release file: $relative" }
    $checksums += (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLower() + ' *' + $relative
}
[IO.File]::WriteAllLines("$stage\SHA256SUMS.txt", $checksums, (New-Object Text.UTF8Encoding($false)))

$zip = Join-Path $dist "Velox-$version.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip -CompressionLevel Optimal
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($zip)
try {
    $entries = @($archive.Entries | Where-Object { $_.Name })
    if ($entries.Count -ne $files.Count + 1) { throw 'ZIP file count does not match the clean stage.' }
    foreach ($entry in $entries) {
        $name = $entry.FullName.Replace('\','/')
        if (($name -ne 'SHA256SUMS.txt' -and $name -notmatch $allowed) -or $name -match '(^/|:|(^|/)\.\.(/|$))') { throw "Unexpected ZIP entry: $name" }
    }
} finally { $archive.Dispose() }
$zipHash=(Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLower()
[IO.File]::WriteAllText("$zip.sha256", "$zipHash *Velox-$version.zip`n", (New-Object Text.UTF8Encoding($false)))

if ($DownloadBase) {
    if (-not (Test-Path $keyFile)) { throw 'No signing key: run gaming-phone\release\new-signing-key.ps1 first.' }
    $exe = Join-Path $stage 'Velox.exe'
    $size = (Get-Item $exe).Length
    $sha = (Get-FileHash $exe -Algorithm SHA256).Hash.ToLower()
    $entries = @()
    $inSection = $false
    $kind = 'New'
    foreach ($line in Get-Content "$root\CHANGELOG.txt" -Encoding UTF8) {
        if ($line -match '^\s*#') { continue }
        if ($line -match '^(\d+\.\d+\.\d+)\s*(\|.*)?$') { $inSection = $Matches[1] -eq $version; $kind = 'New'; continue }
        if ($line -match '^\[(New|Improved|Fixed)\]\s*$') { $kind = $Matches[1]; continue }
        if ($inSection -and $line -match '^\s*-\s+(.+)$') { $entries += "${kind}: " + $Matches[1].Trim() }
    }
    $notes = $entries -join '\n'
    # The signature covers every byte before the "signature=" line (see src\update.hpp).
    $body = "velox-update 1`nversion=$version`nurl=$DownloadBase" + "Velox.exe`nsize=$size`nsha256=$sha`nnotes=$notes`n"
    $key = [System.Security.Cryptography.CngKey]::Import([IO.File]::ReadAllBytes($keyFile), [System.Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob)
    $ecdsa = New-Object System.Security.Cryptography.ECDsaCng($key)
    $ecdsa.HashAlgorithm = [System.Security.Cryptography.CngAlgorithm]::Sha256
    $signature = ($ecdsa.SignData([Text.Encoding]::UTF8.GetBytes($body)) | ForEach-Object { '{0:x2}' -f $_ }) -join ''
    $ecdsa.Dispose()
    $key.Dispose()
    [IO.File]::WriteAllText((Join-Path $dist 'update.txt'), $body + "signature=$signature`n", (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "Update manifest: $dist\update.txt  (upload Velox.exe to $DownloadBase)"
}
Write-Host "Release $version ready: $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
if ($Installer) { & "$PSScriptRoot\make-installer.ps1" -Stage $stage }
