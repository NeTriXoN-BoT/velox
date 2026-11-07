param([string]$Stage='')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
$version=(Get-Content -LiteralPath "$root\VERSION" -Raw).Trim()
if(-not $Stage){$Stage="$root\dist\Velox-$version"}
$Stage=[IO.Path]::GetFullPath($Stage)
$inno=if($env:INNO_HOME){$env:INNO_HOME}else{"$env:USERPROFILE\Tools\InnoSetup"}
if(-not (Test-Path -LiteralPath "$inno\ISCC.exe")){throw 'Inno Setup 6 is required to build the optional installer.'}
if(-not (Test-Path -LiteralPath "$Stage\SHA256SUMS.txt")){throw 'Build and verify the clean portable release first.'}
foreach($line in [IO.File]::ReadAllLines("$Stage\SHA256SUMS.txt")){
 $expected,$relative=$line -split ' \*',2
 $path=[IO.Path]::GetFullPath((Join-Path $Stage $relative))
 if(-not $path.StartsWith($Stage+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'Invalid release path'}
 if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLower() -ne $expected){throw "Release checksum mismatch: $relative"}
}
$expectedFiles=@('SHA256SUMS.txt')+@([IO.File]::ReadAllLines("$Stage\SHA256SUMS.txt")|ForEach-Object{($_ -split ' \*',2)[1]})
$items=@(Get-ChildItem -LiteralPath $Stage -Recurse -Force)
if((Get-Item -LiteralPath $Stage).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'The release stage must not be a link.'}
foreach($item in $items){if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'A release item is a link.'}}
$actualFiles=@($items|Where-Object{-not $_.PSIsContainer}|ForEach-Object{$_.FullName.Substring($Stage.Length+1).Replace('\','/')})
if($actualFiles.Count -ne $expectedFiles.Count -or @($actualFiles|Where-Object{$_ -notin $expectedFiles}).Count){throw 'Installer source contains unlisted files. Rebuild the clean release.'}
$art="$root\build\installer-art"
New-Item -ItemType Directory -Force $art | Out-Null
& javac -encoding UTF-8 -d $art "$root\tools\VeloxIcon.java" "$root\installer\InstallerArt.java"
if($LASTEXITCODE -ne 0){throw 'Installer artwork build failed.'}
& java -cp $art InstallerArt $art
if($LASTEXITCODE -ne 0){throw 'Installer artwork render failed.'}
& "$inno\ISCC.exe" /Qp "/DAppVersion=$version" "/DStageDir=$Stage" "$root\installer\Velox.iss"
if($LASTEXITCODE -ne 0){throw 'Installer compilation failed.'}
$installer="$root\dist\Velox-Setup-$version.exe"
$hash=(Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLower()
[IO.File]::WriteAllText("$installer.sha256","$hash *Velox-Setup-$version.exe`n",(New-Object Text.UTF8Encoding($false)))
Write-Output "Installer ready: $installer"
