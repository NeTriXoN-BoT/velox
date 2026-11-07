param([Parameter(Mandatory=$true)][string]$Out,[string]$Sdk=$env:ANDROID_SDK_ROOT,[switch]$HomeUi)
$ErrorActionPreference='Stop'
if(-not $Sdk){throw 'Set ANDROID_SDK_ROOT.'}
$project=Split-Path (Split-Path $PSScriptRoot)
$bt=Join-Path $Sdk 'build-tools\35.0.0'
$jar=Join-Path $Sdk 'platforms\android-35\android.jar'
$Out=[IO.Path]::GetFullPath($Out)
New-Item -ItemType Directory -Force "$Out\classes","$Out\dex" | Out-Null
function Run($exe){& $exe @args;if($LASTEXITCODE -ne 0){throw "$exe failed ($LASTEXITCODE)"}}
$source=if($HomeUi){"$PSScriptRoot\UiTests.java"}else{"$PSScriptRoot\input-probe\Probe.java"}
$manifest=if($HomeUi){"$PSScriptRoot\home-ui.xml"}else{"$PSScriptRoot\input-probe\AndroidManifest.xml"}
Run javac --release 11 -encoding UTF-8 -classpath $jar -d "$Out\classes" $source
Run jar cf "$Out\classes.jar" -C "$Out\classes" .
Run "$bt\d8.bat" --release --min-api 30 --lib $jar --output "$Out\dex" "$Out\classes.jar"
Run "$bt\aapt2.exe" link -o "$Out\base.apk" -I $jar --manifest $manifest --min-sdk-version 30 --target-sdk-version 35
Run jar uf "$Out\base.apk" -C "$Out\dex" classes.dex
Run "$bt\zipalign.exe" -p -f 4 "$Out\base.apk" "$Out\aligned.apk"
Run "$bt\apksigner.bat" sign --ks "$project\keys\helper.keystore" --ks-key-alias helper --ks-pass "file:$project\keys\helper.pass" --out "$Out\test.apk" "$Out\aligned.apk"
Write-Host "Built developer test only: $Out\test.apk"
