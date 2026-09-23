# Real-screen capture to compare with PrintWindow. Usage: snap_screen_now.ps1 [navIndex]
param([int]$nav = 2)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class SS {
    public delegate bool EnumCb(IntPtr h, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumCb cb, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L; public int T; public int R; public int B; }
}
"@
[SS]::SetProcessDPIAware() | Out-Null

$detail = [IntPtr]::Zero
$cb = [SS+EnumCb]{ param($h, $l)
    $sb = New-Object System.Text.StringBuilder 256
    [SS]::GetClassNameW($h, $sb, 256) | Out-Null
    if ($sb.ToString() -eq 'QuotaBarDetailWnd' -and $script:detail -eq [IntPtr]::Zero) { $script:detail = $h; return $false }
    return $true
}
[SS]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
if ($detail -eq [IntPtr]::Zero) { Write-Host 'no detail window'; exit 1 }

# switch page (same client-ratio coords as shot_pages.ps1)
if ($nav -ge 0) {
    $c = New-Object SS+RECT
    [SS]::GetClientRect($detail, [ref]$c) | Out-Null
    [int]$cw = $c.R - $c.L; [int]$ch = $c.B - $c.T
    [int]$x = [Math]::Round($cw * 112.0 / 1000.0)
    [int]$y = [Math]::Round($ch * (110.0 + 40.0 * $nav) / 780.0)
    [int]$lp = $y * 65536 + $x
    [SS]::PostMessageW($detail, 0x0202, [IntPtr]::Zero, [IntPtr]$lp) | Out-Null
    Start-Sleep -Milliseconds 700
}

$r = New-Object SS+RECT
[SS]::GetWindowRect($detail, [ref]$r) | Out-Null
[int]$w = $r.R - $r.L; [int]$h2 = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap($w, $h2)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size($w, $h2)))
$bmp.Save("D:\Zcode\QuotaBar\build\screen_page.png", [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Host "saved screen_page.png $w x $h2"
