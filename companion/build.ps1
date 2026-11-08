param([string]$Sdk=$env:ANDROID_SDK_ROOT, [string]$HelperKeystore='', [string]$HelperPasswordFile='')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
$externalKey=[bool]$HelperKeystore
if ([bool]$HelperKeystore -ne [bool]$HelperPasswordFile) { throw 'Supply both helper signing paths.' }
if (-not $HelperKeystore) { $HelperKeystore="$root\keys\helper.keystore"; $HelperPasswordFile="$root\keys\helper.pass" }
$bt=Join-Path $Sdk 'build-tools\35.0.0'
$out=Join-Path $root 'build\companion'
New-Item -ItemType Directory -Force "$out\classes","$out\dex" | Out-Null
function Run($exe){& $exe @args; if($LASTEXITCODE -ne 0){throw "$exe failed ($LASTEXITCODE)"}}
$inputData=New-Object IO.MemoryStream
try{
    Get-ChildItem -LiteralPath $PSScriptRoot -Recurse -File | Sort-Object FullName | ForEach-Object {
        $bytes=[IO.File]::ReadAllBytes($_.FullName);$inputData.Write($bytes,0,$bytes.Length)
    }
    $sha=[Security.Cryptography.SHA256]::Create()
    try{$hash=([BitConverter]::ToString($sha.ComputeHash($inputData.ToArray()))).Replace('-','').ToLower().Substring(0,24)}finally{$sha.Dispose()}
}finally{$inputData.Dispose()}
$utf8=New-Object Text.UTF8Encoding($false)
[IO.File]::WriteAllText("$out\Build.java","package app.velox.home; final class Build { static final String ID = `"$hash`"; }",$utf8)
[IO.File]::WriteAllText("$root\build\companion_id.h","#pragma once`n#define VELOX_COMPANION_ID L`"$hash`"`n",$utf8)
$apk="$root\build\companion.apk"
if(-not $externalKey -and (Test-Path -LiteralPath $apk) -and (Test-Path -LiteralPath "$out\source-id") -and ([IO.File]::ReadAllText("$out\source-id") -eq $hash)){return}
if(-not $Sdk){throw 'Set ANDROID_SDK_ROOT to build Velox Home.'}
$jar=Join-Path $Sdk 'platforms\android-35\android.jar'
$sources=@(Get-ChildItem "$PSScriptRoot\src" -Recurse -Filter '*.java' | ForEach-Object FullName)+@("$out\Build.java")
Run javac @('--release','11','-encoding','UTF-8','-classpath',$jar,'-d',"$out\classes") @sources
Run jar cf "$out\classes.jar" -C "$out\classes" .
Run "$bt\d8.bat" --release --min-api 30 --lib $jar --output "$out\dex" "$out\classes.jar"
Run "$bt\aapt2.exe" compile --dir "$PSScriptRoot\res" -o "$out\res.zip"
Run "$bt\aapt2.exe" link -o "$out\base.apk" -I $jar --manifest "$PSScriptRoot\AndroidManifest.xml" --min-sdk-version 30 --target-sdk-version 35 --version-name $hash "$out\res.zip"
Run jar uf "$out\base.apk" -C "$out\dex" classes.dex
Run "$bt\zipalign.exe" -p -f 4 "$out\base.apk" "$out\aligned.apk"
Run "$bt\apksigner.bat" sign --ks $HelperKeystore --ks-key-alias helper --ks-pass "file:$HelperPasswordFile" --out $apk "$out\aligned.apk"
[IO.File]::WriteAllText("$out\source-id",$hash,$utf8)
Write-Host 'Built Velox Home, Settings and desktop bridge.'
