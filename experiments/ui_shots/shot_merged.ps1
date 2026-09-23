Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class PG3 {
    public delegate bool EnumCb(IntPtr h, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumCb cb, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L; public int T; public int R; public int B; }
}
"@
Add-Type @"
using System.Runtime.InteropServices;
public static class DpiQ3 { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }
"@
[DpiQ3]::SetProcessDPIAware() | Out-Null
$script:found = [IntPtr]::Zero
$script:hidden = [IntPtr]::Zero
$cb = [PG3+EnumCb]{ param($h, $l)
    $sb = New-Object System.Text.StringBuilder 256
    [PG3]::GetClassNameW($h, $sb, 256) | Out-Null
    $cls = $sb.ToString()
    if ($cls -eq "QuotaBarDetailWnd" -and [PG3]::IsWindowVisible($h)) {
        $script:found = $h
        return $false
    }
    if ($cls -eq "QuotaBarHiddenWnd") { $script:hidden = $h }
    return $true
}
[PG3]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
if ($script:found -eq [IntPtr]::Zero) {
    if ($script:hidden -eq [IntPtr]::Zero) { Write-Host "hidden window not found"; exit 1 }
    [PG3]::PostMessageW($script:hidden, 0x8005, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 1500
    [PG3]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
}
if ($script:found -eq [IntPtr]::Zero) { Write-Host "detail window not found"; exit 1 }

function Snap([string]$name) {
    Start-Sleep -Milliseconds 600
    $r = New-Object PG3+RECT
    [PG3]::GetWindowRect($script:found, [ref]$r) | Out-Null
    [int]$w = $r.R - $r.L; [int]$hgt = $r.B - $r.T
    $bmp = New-Object System.Drawing.Bitmap($w, $hgt)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $bg = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 30, 31, 36))
    $g.FillRectangle($bg, 0, 0, $w, $hgt)
    $hdc = $g.GetHdc()
    [PG3]::PrintWindow($script:found, $hdc, 0) | Out-Null
    $g.ReleaseHdc($hdc)
    $g.Dispose()
    $bmp.Save("D:\Zcode\QuotaBar\build\merged_$name.png", [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Host ("saved merged_$name.png " + $w + "x" + $hgt)
}

# click nav item i (client ratio coords: x=112/1000, y=(110+40*i)/780)
function ClickNav([int]$i) {
    $c = New-Object PG3+RECT
    [PG3]::GetClientRect($script:found, [ref]$c) | Out-Null
    [int]$cw = $c.R - $c.L; [int]$ch = $c.B - $c.T
    [int]$x = [Math]::Round($cw * 112.0 / 1000.0)
    [int]$y = [Math]::Round($ch * (110.0 + 40.0 * $i) / 780.0)
    [int]$lp = $y * 65536 + $x
    [PG3]::PostMessageW($script:found, 0x0202, [IntPtr]::Zero, [IntPtr]$lp) | Out-Null
}

# rail (collapsed 56 DIP icon bar) nav click, logical client 780x760 basis
function ClickNavRail([int]$i) {
    $c = New-Object PG3+RECT
    [PG3]::GetClientRect($script:found, [ref]$c) | Out-Null
    [int]$cw = $c.R - $c.L; [int]$ch = $c.B - $c.T
    [int]$x = [Math]::Round($cw * 28.0 / 780.0)
    [int]$y = [Math]::Round($ch * (84.0 + 40.0 * $i) / 760.0)
    [int]$lp = $y * 65536 + $x
    [PG3]::PostMessageW($script:found, 0x0202, [IntPtr]::Zero, [IntPtr]$lp) | Out-Null
}

ClickNav 0
Snap "quota"
ClickNav 1
Snap "display"
ClickNav 2
Snap "accounts"

# narrow (half-snap) check: shrink window, back to display page via rail icon
[PG3]::SetWindowPos($script:found, [IntPtr]::Zero, 0, 0, 1013, 1016, 0x0004) | Out-Null
Start-Sleep -Milliseconds 400
ClickNavRail 1
Snap "display_narrow2"
[PG3]::SetWindowPos($script:found, [IntPtr]::Zero, 0, 0, 2026, 1631, 0x0004) | Out-Null
Start-Sleep -Milliseconds 400
ClickNav 0
Snap "restored"

# docked mode: back to display page, then click mode card 0 (absolute client px on 2026x1631)
ClickNav 1
Start-Sleep -Milliseconds 400
[PG3]::PostMessageW($script:found, 0x0202, [IntPtr]::Zero, [IntPtr]((362 -shl 16) -bor 730)) | Out-Null
Start-Sleep -Milliseconds 500
Snap "display_docked"
# back to embed (draft), then close without saving
[PG3]::PostMessageW($script:found, 0x0202, [IntPtr]::Zero, [IntPtr]((434 -shl 16) -bor 1150)) | Out-Null
Start-Sleep -Milliseconds 300

[PG3]::PostMessageW($script:found, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Write-Host "done"
