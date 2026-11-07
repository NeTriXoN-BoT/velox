# Extract only Android installation data and describe APK manifests without executing them.
param([Parameter(Mandatory=$true)][string]$Archive, [Parameter(Mandatory=$true)][string]$Destination)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Read-U16([byte[]]$Bytes, [int]$At) { [BitConverter]::ToUInt16($Bytes, $At) }
function Read-U32([byte[]]$Bytes, [int]$At) { [BitConverter]::ToUInt32($Bytes, $At) }
function Read-Length8([byte[]]$Bytes, [ref]$At) {
    $n = [int]$Bytes[$At.Value]; $At.Value++
    if (($n -band 128) -ne 0) { $n = (($n -band 127) -shl 8) -bor $Bytes[$At.Value]; $At.Value++ }
    return $n
}
function Read-Length16([byte[]]$Bytes, [ref]$At) {
    $n = Read-U16 $Bytes $At.Value; $At.Value += 2
    if (($n -band 32768) -ne 0) { $n = (($n -band 32767) -shl 16) -bor (Read-U16 $Bytes $At.Value); $At.Value += 2 }
    return $n
}
function Read-Apk([string]$File) {
    $apk = [IO.Compression.ZipFile]::OpenRead($File)
    try {
        $manifest = $apk.GetEntry('AndroidManifest.xml')
        if (-not $manifest -or $manifest.Length -gt 16MB) { throw "Invalid APK manifest: $File" }
        $stream = $manifest.Open(); $memory = New-Object IO.MemoryStream
        try { $stream.CopyTo($memory); $bytes = $memory.ToArray() } finally { $stream.Dispose(); $memory.Dispose() }
        if ($bytes.Length -lt 8 -or (Read-U16 $bytes 0) -ne 3 -or (Read-U32 $bytes 4) -ne $bytes.Length) {
            throw "Invalid binary APK manifest: $File"
        }
        $strings = @(); $package = ''; $split = ''; $minSdk = 1
        for ($at = 8; $at -lt $bytes.Length;) {
            if ($at + 8 -gt $bytes.Length) { throw 'Truncated APK manifest' }
            $kind = Read-U16 $bytes $at; $header = Read-U16 $bytes ($at + 2); $size = Read-U32 $bytes ($at + 4)
            if ($size -lt 8 -or $at + $size -gt $bytes.Length) { throw 'Invalid APK manifest chunk' }
            if ($kind -eq 1) {
                $count = Read-U32 $bytes ($at + 8); $flags = Read-U32 $bytes ($at + 16); $start = Read-U32 $bytes ($at + 20)
                if ($count -gt 100000 -or $header + 4 * $count -gt $size) { throw 'Invalid APK string pool' }
                $strings = New-Object 'string[]' $count
                for ($i = 0; $i -lt $count; $i++) {
                    $cursor = [int]($at + $start + (Read-U32 $bytes ($at + $header + 4 * $i)))
                    if (($flags -band 256) -ne 0) {
                        $null = Read-Length8 $bytes ([ref]$cursor)
                        $length = Read-Length8 $bytes ([ref]$cursor)
                        $strings[$i] = [Text.Encoding]::UTF8.GetString($bytes, $cursor, $length)
                    } else {
                        $length = Read-Length16 $bytes ([ref]$cursor)
                        $strings[$i] = [Text.Encoding]::Unicode.GetString($bytes, $cursor, 2 * $length)
                    }
                }
            } elseif ($kind -eq 258) {
                $tagId = Read-U32 $bytes ($at + 20)
                if ($tagId -ge $strings.Count) { throw 'Invalid APK tag' }
                $tag = $strings[$tagId]; $attributeStart = Read-U16 $bytes ($at + 24)
                $attributeSize = Read-U16 $bytes ($at + 26); $attributes = Read-U16 $bytes ($at + 28)
                if ($attributeSize -lt 20 -or 16 + $attributeStart + $attributeSize * $attributes -gt $size) { throw 'Invalid APK attributes' }
                for ($i = 0; $i -lt $attributes; $i++) {
                    $a = $at + 16 + $attributeStart + $i * $attributeSize
                    $nameId = Read-U32 $bytes ($a + 4); $type = $bytes[$a + 15]; $value = Read-U32 $bytes ($a + 16)
                    if ($nameId -ge $strings.Count) { throw 'Invalid APK attribute name' }
                    $name = $strings[$nameId]
                    if ($type -eq 3 -and $value -lt $strings.Count) {
                        if ($tag -eq 'manifest' -and $name -eq 'package') { $package = $strings[$value] }
                        if ($tag -eq 'manifest' -and $name -eq 'split') { $split = $strings[$value] }
                    }
                    if ($tag -eq 'uses-sdk' -and $name -eq 'minSdkVersion' -and $type -in 16,17) { $minSdk = $value }
                }
            }
            $at += $size
        }
        if ($package -notmatch '^[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)+$') { throw 'Invalid APK package name' }
        if ($split -and $split -notmatch '^[A-Za-z0-9_.-]+$') { throw 'Invalid APK split name' }
        $abis = @($apk.Entries | ForEach-Object { if ($_.FullName -match '^lib/([^/]+)/[^/]+\.so$') { $Matches[1] } } | Sort-Object -Unique)
        return [PSCustomObject]@{ Package=$package; Split=$split; MinSdk=$minSdk; Abis=($abis -join ',') }
    } finally { $apk.Dispose() }
}

try {
    $root = [IO.Path]::GetFullPath($Destination).TrimEnd('\') + '\'
    [IO.Directory]::CreateDirectory($root) | Out-Null
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    $selected = @(); [long]$total = 0
    try {
        if ($zip.Entries.Count -gt 10000) { throw 'The archive contains too many files' }
        foreach ($entry in $zip.Entries) {
            $relative = $entry.FullName.Replace('\','/')
            if ($relative -notmatch '\.(apk|obb)$') { continue }
            if ($relative -match '(^/|:|[\x00-\x1f]|(^|/)\.\.?(/|$))' -or
                (($entry.ExternalAttributes -shr 16) -band 61440) -eq 40960) { throw 'The archive contains an unsafe path' }
            foreach ($part in $relative.Split('/')) {
                if ($part -match '^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\.|$)' -or $part.EndsWith('.') -or $part.EndsWith(' ')) {
                    throw 'The archive contains an invalid Windows path'
                }
            }
            $target = [IO.Path]::GetFullPath((Join-Path $root $relative))
            if (-not $target.StartsWith($root,[StringComparison]::OrdinalIgnoreCase)) { throw 'Archive path escapes its folder' }
            $total += $entry.Length
            if ($total -gt 16GB) { throw 'The unpacked installation exceeds 16 GB' }
            $selected += [PSCustomObject]@{ Entry=$entry; Path=$target; Relative=$relative }
        }
        $drive = New-Object IO.DriveInfo ([IO.Path]::GetPathRoot($root))
        if ($drive.AvailableFreeSpace -lt $total + 512MB) { throw 'Not enough free space to unpack this app' }
        foreach ($item in $selected) {
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($item.Path)) | Out-Null
            $inputStream = $item.Entry.Open()
            $outputStream = New-Object IO.FileStream ($item.Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
            try { $inputStream.CopyTo($outputStream) } finally { $inputStream.Dispose(); $outputStream.Dispose() }
        }
    } finally { $zip.Dispose() }
    $rows = @()
    foreach ($item in $selected) {
        if ([IO.Path]::GetExtension($item.Path) -ieq '.apk') {
            $meta = Read-Apk $item.Path
            $rows += "APK`t$($item.Relative)`t$($meta.Package)`t$($meta.Split)`t$($meta.MinSdk)`t$($meta.Abis)"
        } else { $rows += "OBB`t$($item.Relative)" }
    }
    [IO.File]::WriteAllLines((Join-Path $root 'packages.tsv'), [string[]]$rows, (New-Object Text.UTF8Encoding($false)))
    Write-Output 'velox-import-ok'
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
