# Test helper for the Velox launcher window (class "VeloxLauncher"): save a picture of it, or
# click a point given in DIPs (client coordinates at 96 dpi). Clicks are posted messages, so the
# window is not activated and other applications is not interrupted.
#   ui.ps1 snap <file.png>      ui.ps1 click <x> <y>
param([string]$Action, [string]$A1, [string]$A2)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class GpW {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
    public struct RECT { public int L, T, R, B; }
}
"@
$h = [GpW]::FindWindow('VeloxLauncher', [NullString]::Value)
if ($h -eq [IntPtr]::Zero) { 'launcher not running'; exit 1 }
if ($Action -eq 'snap') {
    $r = New-Object GpW+RECT; [void][GpW]::GetWindowRect($h, [ref]$r)
    $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
    $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc(); [void][GpW]::PrintWindow($h, $dc, 2); $g.ReleaseHdc($dc); $g.Dispose()
    $bmp.Save($A1); "saved $($bmp.Width)x$($bmp.Height)"; $bmp.Dispose()
} elseif ($Action -eq 'click') {
    $s = [GpW]::GetDpiForWindow($h) / 96.0
    $x = [int]([double]$A1 * $s); $y = [int]([double]$A2 * $s)
    $lp = [IntPtr](($y -shl 16) -bor $x)
    [void][GpW]::PostMessage($h, 0x0200, [IntPtr]::Zero, $lp)
    [void][GpW]::PostMessage($h, 0x0201, [IntPtr]1, $lp)
    [void][GpW]::PostMessage($h, 0x0202, [IntPtr]::Zero, $lp)
    "clicked ($A1, $A2) dip"
}
