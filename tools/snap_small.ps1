# Resize detail window to a given logical size and PrintWindow-snap a page.
# Usage: snap_small.ps1 <out.png> [pageNavIndex] [widthDip] [heightDip]
param([string]$out = "build\small.png", [int]$nav = 2, [int]$wdip = 880, [int]$hdip = 640)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class SZ {
    public delegate bool EnumCb(IntPtr h, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumCb cb, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L; public int T; public int R; public int B; }
}
"@
[SZ]::SetProcessDPIAware() | Out-Null
$detail = [IntPtr]::Zero
$cb = [SZ+EnumCb]{ param($h, $l)
    $sb = New-Object System.Text.StringBuilder 256
    [SZ]::GetClassNameW($h, $sb, 256) | Out-Null
    if ($sb.ToString() -eq 'QuotaBarDetailWnd') { $script:detail = $h; return $false }
    return $true
}
[SZ]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
if ($detail -eq [IntPtr]::Zero) { Write-Host 'no detail window'; exit 1 }

$dpi = [SZ]::GetDpiForWindow($detail)
if ($dpi -eq 0) { $dpi = 96 }
[int]$tw = [int]($wdip * $dpi / 96)
[int]$th = [int]($hdip * $dpi / 96)
[SZ]::SetWindowPos($detail, [IntPtr]::Zero, 0, 0, $tw, $th, 0x0014) | Out-Null  # NOZORDER|NOACTIVATE
Start-Sleep -Milliseconds 700
if ($nav -ge 0) {
    # nav items sit at fixed logical coords: x center 112, y center (110+40i)
    [int]$x = [int](112 * $dpi / 96)
    [int]$y = [int]((110.0 + 40.0 * $nav) * $dpi / 96)
    [int]$lp = $y * 65536 + $x
    [SZ]::PostMessageW($detail, 0x0202, [IntPtr]::Zero, [IntPtr]$lp) | Out-Null
    Start-Sleep -Milliseconds 600
}
$c = New-Object SZ+RECT
[SZ]::GetClientRect($detail, [ref]$c) | Out-Null
[int]$cw = $c.R - $c.L; [int]$chh = $c.B - $c.T
$bmp = New-Object System.Drawing.Bitmap($cw, $chh)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$bg = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 16, 20, 26))
$g.FillRectangle($bg, 0, 0, $cw, $chh)
$hdc = $g.GetHdc()
[SZ]::PrintWindow($detail, $hdc, 0) | Out-Null
$g.ReleaseHdc($hdc)
$g.Dispose()
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "saved $out ${cw}x${chh}"
