# Signs the platform-specific feed consumed by linux/velox/updates.py.
param(
    [Parameter(Mandatory=$true)][string]$AppImage,
    [Parameter(Mandatory=$true)][string]$DownloadBase,
    [Parameter(Mandatory=$true)][string]$KeyFile,
    [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
$version=(Get-Content -LiteralPath "$root\VERSION" -Raw).Trim()
if($version -notmatch '^\d+\.\d+\.\d+$'){throw 'Invalid release version.'}
$image=Get-Item -LiteralPath $AppImage
if($image.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Use a regular AppImage file.'}
if($image.Length -le 0 -or $image.Length -gt 768MB){throw 'Unsupported AppImage size.'}
$uri=[uri]$DownloadBase
if(-not $uri.IsAbsoluteUri -or $uri.Scheme -ne 'https' -or $uri.UserInfo -or $uri.Query -or $uri.Fragment){throw 'Use a plain HTTPS release directory.'}
if(-not $DownloadBase.EndsWith('/')){$DownloadBase+='/' }
$entries=@();$inside=$false;$kind='New'
foreach($line in Get-Content -LiteralPath "$root\CHANGELOG.txt" -Encoding UTF8){
    if($line -match '^(\d+\.\d+\.\d+)\s*(\|.*)?$'){$inside=$Matches[1] -eq $version;continue}
    if($line -match '^\[(New|Improved|Fixed)\]$'){$kind=$Matches[1];continue}
    if($inside -and $line -match '^\s*-\s+(.+)$'){$entries+="${kind}: "+$Matches[1].Trim()}
}
$notes=$entries -join '\n'
$sha=(Get-FileHash -LiteralPath $image.FullName -Algorithm SHA256).Hash.ToLower()
$url=$DownloadBase+[uri]::EscapeDataString($image.Name)
$body="velox-linux-update 1`nversion=$version`nurl=$url`nsize=$($image.Length)`nsha256=$sha`nnotes=$notes`n"
$key=[Security.Cryptography.CngKey]::Import([IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $KeyFile)),[Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob)
try{
    $ecdsa=New-Object Security.Cryptography.ECDsaCng($key)
    try{
        $ecdsa.HashAlgorithm=[Security.Cryptography.CngAlgorithm]::Sha256
        $signature=($ecdsa.SignData([Text.Encoding]::UTF8.GetBytes($body)) | ForEach-Object {'{0:x2}' -f $_}) -join ''
    }finally{$ecdsa.Dispose()}
}finally{$key.Dispose()}
[IO.File]::WriteAllText([IO.Path]::GetFullPath($Output),$body+"signature=$signature`n",(New-Object Text.UTF8Encoding($false)))
Write-Host 'Signed Linux update manifest.'
