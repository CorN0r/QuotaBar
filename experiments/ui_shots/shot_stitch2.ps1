# Snapshot the reworked main window (stitch_minimalist restyle): quota + display pages.
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class PG9 {
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
public static class DpiQ9 { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }
"@
[DpiQ9]::SetProcessDPIAware() | Out-Null

function Find-Detail {
    $script:found = [IntPtr]::Zero
    $cb = [PG9+EnumCb]{ param($h, $l)
        $sb = New-Object System.Text.StringBuilder 256
        [PG9]::GetClassNameW($h, $sb, 256) | Out-Null
        if ($sb.ToString() -eq "QuotaBarDetailWnd" -and [PG9]::IsWindowVisible($h)) {
            $script:found = $h
            return $false
        }
        return $true
    }
    [PG9]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    return $script:found
}

function Find-Hidden {
    $script:hidden = [IntPtr]::Zero
    $cb = [PG9+EnumCb]{ param($h, $l)
        $sb = New-Object System.Text.StringBuilder 256
        [PG9]::GetClassNameW($h, $sb, 256) | Out-Null
        if ($sb.ToString() -eq "QuotaBarHiddenWnd") { $script:hidden = $h; return $false }
        return $true
    }
    [PG9]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    return $script:hidden
}

# Click at DIP coordinates (nav item centers: x=112, y = 80 + 46*i with 44px titlebar)
function ClickXY([int]$dipX, [int]$dipY) {
    $c = New-Object PG9+RECT
    [PG9]::GetClientRect($script:found, [ref]$c) | Out-Null
    [int]$cw = $c.R - $c.L; [int]$ch = $c.B - $c.T
    $g = [System.Drawing.Graphics]::FromHwnd($script:found)
    $scale = $g.DpiX / 96.0
    $g.Dispose()
    [int]$x = [Math]::Round($dipX * $scale)
    [int]$y = [Math]::Round($dipY * $scale)
    if ($x -ge $cw) { $x = $cw - 1 }
    if ($y -ge $ch) { $y = $ch - 1 }
    [int]$lp = $y * 65536 + $x
    [PG9]::PostMessageW($script:found, 0x0200, [IntPtr]::Zero, [IntPtr]$lp) | Out-Null
    [PG9]::PostMessageW($script:found, 0x0201, [IntPtr]1, [IntPtr]$lp) | Out-Null
    [PG9]::PostMessageW($script:found, 0x0202, [IntPtr]0, [IntPtr]$lp) | Out-Null
}

function Snap([string]$name) {
    Start-Sleep -Milliseconds 700
    $r = New-Object PG9+RECT
    [PG9]::GetWindowRect($script:found, [ref]$r) | Out-Null
    [int]$w = $r.R - $r.L; [int]$hgt = $r.B - $r.T
    $bmp = New-Object System.Drawing.Bitmap($w, $hgt)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $bg = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 11, 15, 23))
    $g.FillRectangle($bg, 0, 0, $w, $hgt)
    $hdc = $g.GetHdc()
    [PG9]::PrintWindow($script:found, $hdc, 0) | Out-Null
    $g.ReleaseHdc($hdc)
    $g.Dispose()
    $bmp.Save("D:\Zcode\QuotaBar\build\stitch2_$name.png", [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Host ("saved stitch2_$name.png " + $w + "x" + $hgt)
}

$exe = "D:\Zcode\QuotaBar\build\QuotaBar.exe"
Start-Process -FilePath $exe | Out-Null
$wnd = [IntPtr]::Zero
for ($t = 0; $t -lt 40; $t++) {
    Start-Sleep -Milliseconds 250
    $wnd = Find-Detail
    if ($wnd -ne [IntPtr]::Zero) { break }
}
if ($wnd -eq [IntPtr]::Zero) {
    # No first-run window: poke the hidden window (WM_APP+5, tray toggle)
    $hid = Find-Hidden
    if ($hid -eq [IntPtr]::Zero) { Write-Host "hidden window not found"; exit 1 }
    [PG9]::PostMessageW($hid, 0x8005, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    for ($t = 0; $t -lt 40; $t++) {
        Start-Sleep -Milliseconds 250
        $wnd = Find-Detail
        if ($wnd -ne [IntPtr]::Zero) { break }
    }
}
if ($wnd -eq [IntPtr]::Zero) { Write-Host "detail window not found"; exit 1 }

$script:found = $wnd
Start-Sleep -Milliseconds 900
Snap "quota"
ClickXY 112 126
Snap "display"
# expand verification with synchronous PrintWindow
ClickXY 112 80
Start-Sleep -Milliseconds 500
ClickXY 540 461
Snap "expanded"
Write-Host "done"
