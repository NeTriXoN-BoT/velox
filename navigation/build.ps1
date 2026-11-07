param([Parameter(Mandatory=$true)][string]$LauncherApk, [string]$Sdk = $env:ANDROID_SDK_ROOT)
$ErrorActionPreference = 'Stop'
if (-not $Sdk) { throw 'Set ANDROID_SDK_ROOT or pass -Sdk.' }
$sdk = $Sdk
$bt = "$sdk\build-tools\35.0.0"
$project = Split-Path $PSScriptRoot
function Run($exe) {
    & $exe @args
    if ($LASTEXITCODE -ne 0) { throw "$exe failed ($LASTEXITCODE)" }
}
$build = Join-Path $project 'build\navigation'
New-Item -ItemType Directory -Force $build | Out-Null
Run python "$PSScriptRoot\patch-layout.py" --launcher $LauncherApk
$hash = (Get-FileHash -LiteralPath $LauncherApk -Algorithm SHA256).Hash.ToLower()
$guard = [IO.File]::ReadAllText("$PSScriptRoot\post-fs-data.sh") -replace '(?m)^EXPECTED=[a-f0-9]+$', "EXPECTED=$hash"
[IO.File]::WriteAllText("$PSScriptRoot\post-fs-data.sh", $guard, (New-Object Text.UTF8Encoding($false)))
Run "$bt\aapt2.exe" compile --dir "$PSScriptRoot\template" -o "$build\overlay-res.zip"
Run "$bt\aapt2.exe" link -o "$build\overlay-base.apk" -I "$sdk\platforms\android-35\android.jar" --manifest "$PSScriptRoot\AndroidManifest.xml" --min-sdk-version 30 --target-sdk-version 35 --allow-reserved-package-id --package-id 0x7e "$build\overlay-res.zip"
Add-Type -AssemblyName System.IO.Compression.FileSystem
$source = [IO.Compression.ZipFile]::OpenRead("$build\overlay-base.apk")
if (Test-Path -LiteralPath "$build\overlay-patched.apk") { Remove-Item -LiteralPath "$build\overlay-patched.apk" }
$output = [IO.Compression.ZipFile]::Open("$build\overlay-patched.apk",[IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($item in $source.Entries) {
        # Android 11+ rejects a compressed resources.arsc even when the APK signature is valid.
        $compression = if ($item.FullName -eq 'resources.arsc') { [IO.Compression.CompressionLevel]::NoCompression } else { [IO.Compression.CompressionLevel]::Optimal }
        $entry = $output.CreateEntry($item.FullName,$compression)
        $inputStream = if ($item.FullName -eq 'res/layout/taskbar.xml') { [IO.File]::OpenRead("$PSScriptRoot\compiled\taskbar.xml") } else { $item.Open() }
        $outputStream = $entry.Open()
        try { $inputStream.CopyTo($outputStream) } finally { $inputStream.Dispose(); $outputStream.Dispose() }
    }
} finally { $source.Dispose(); $output.Dispose() }
Run "$bt\zipalign.exe" -p -f 4 "$build\overlay-patched.apk" "$build\overlay-aligned.apk"
Run "$bt\zipalign.exe" -c -p 4 "$build\overlay-aligned.apk"
Run python -c "import sys,zipfile; z=zipfile.ZipFile(sys.argv[1]); assert z.getinfo('resources.arsc').compress_type == zipfile.ZIP_STORED" "$build\overlay-aligned.apk"
Run "$bt\apksigner.bat" sign --ks "$project\keys\helper.keystore" --ks-key-alias helper --ks-pass "file:$project\keys\helper.pass" --out "$PSScriptRoot\VeloxNavOrder.apk" "$build\overlay-aligned.apk"
