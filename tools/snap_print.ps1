# PrintWindow snap of the QuotaBar detail window, with optional client-coordinate clicks.
# Usage: snap_print.ps1 <out.png> [x1,y1 [x2,y2 ...]]   (physical client px, sent as WM_LBUTTONUP)
param([string]$out = "build\snap.png", [string[]]$clicks = @())
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class SP {
    public delegate bool EnumCb(IntPtr h, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumCb cb, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L; public int T; public int R; public int B; }
}
"@
[SP]::SetProcessDPIAware() | Out-Null
$detail = [IntPtr]::Zero
$cb = [SP+EnumCb]{ param($h, $l)
    $sb = New-Object System.Text.StringBuilder 256
    [SP]::GetClassNameW($h, $sb, 256) | Out-Null
    if ($sb.ToString() -eq 'QuotaBarDetailWnd') { $script:detail = $h; return $false }
    return $true
}
[SP]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
if ($detail -eq [IntPtr]::Zero) { Write-Host 'no detail window'; exit 1 }

foreach ($c in $clicks) {
    $parts = $c.Split(',')
    [int]$x = [int]$parts[0]; [int]$y = [int]$parts[1]
    [int]$lp = $y * 65536 + $x
    [SP]::PostMessageW($detail, 0x0202, [IntPtr]::Zero, [IntPtr]$lp) | Out-Null
    Start-Sleep -Milliseconds 600
}

$c = New-Object SP+RECT
[SP]::GetClientRect($detail, [ref]$c) | Out-Null
[int]$w = $c.R - $c.L; [int]$h2 = $c.B - $c.T
$bmp = New-Object System.Drawing.Bitmap($w, $h2)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$bg = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 16, 20, 26))
$g.FillRectangle($bg, 0, 0, $w, $h2)
$hdc = $g.GetHdc()
[SP]::PrintWindow($detail, $hdc, 0) | Out-Null
$g.ReleaseHdc($hdc)
$g.Dispose()
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "saved $out $w x $h2"
