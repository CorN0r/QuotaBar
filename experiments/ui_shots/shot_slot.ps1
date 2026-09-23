Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class TB9 {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string win);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr p, IntPtr a, string cls, string win);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L; public int T; public int R; public int B; }
}
"@
Add-Type @"
using System.Runtime.InteropServices;
public static class Dpi { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }
"@
[Dpi]::SetProcessDPIAware() | Out-Null
$tray = [TB9]::FindWindowW("Shell_TrayWnd", $null)
$slot = [TB9]::FindWindowExW($tray, [IntPtr]::Zero, "QuotaBarSlotWnd", $null)
$r = New-Object TB9+RECT
[TB9]::GetWindowRect($slot, [ref]$r) | Out-Null
[int]$w = $r.R - $r.L; [int]$h = $r.B - $r.T
[int]$px = [Math]::Max(0, $r.L - 8); [int]$py = [Math]::Max(0, $r.T - 8)
[int]$pw = $w + 16; [int]$ph = $h + 16
$bmp = New-Object System.Drawing.Bitmap($pw, $ph)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($px, $py, 0, 0, (New-Object System.Drawing.Size($pw, $ph)))
$g.Dispose()
[int]$bw = $pw * 4; [int]$bh = $ph * 4
$big = New-Object System.Drawing.Bitmap($bw, $bh)
$g2 = [System.Drawing.Graphics]::FromImage($big)
$g2.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$g2.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
$g2.DrawImage($bmp, 0, 0, $bw, $bh)
$g2.Dispose()
$big.Save("D:\slotzoom.png", [System.Drawing.Imaging.ImageFormat]::Png)
Write-Host ("saved " + $pw + "x" + $ph + " -> 4x, rect " + $r.L + "," + $r.T + "," + $r.R + "," + $r.B)
