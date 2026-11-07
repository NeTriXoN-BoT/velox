# Captures the on-screen pixels of one top-level window (found by class name) at given times.
#   capwin.ps1 <class> <outPrefix> <seconds,...>
param([string]$Class, [string]$Out, [string]$Times = '1')
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class CapW { [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r); public struct RECT { public int L,T,R,B; } }
"@
$t0 = Get-Date
foreach ($at in ($Times -split ',' | ForEach-Object { [double]$_ })) {
    while (((Get-Date) - $t0).TotalSeconds -lt $at) { Start-Sleep -Milliseconds 30 }
    $h = [CapW]::FindWindow($Class, [NullString]::Value)
    if ($h -eq [IntPtr]::Zero) { "no window at $at s"; continue }
    $r = New-Object CapW+RECT; [void][CapW]::GetWindowRect($h, [ref]$r)
    $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
    $g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size); $g.Dispose()
    $bmp.Save("${Out}_$at.png"); "captured $($bmp.Width)x$($bmp.Height) at $at s"; $bmp.Dispose()
}
