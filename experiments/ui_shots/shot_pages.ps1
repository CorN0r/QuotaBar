# 适配 2026-09 两页版 UI 的截图：启动 exe → 抓额度详情/显示设置 → 关闭
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

function Find-Detail {
    $script:found = [IntPtr]::Zero
    $cb = [PG3+EnumCb]{ param($h, $l)
        $sb = New-Object System.Text.StringBuilder 256
        [PG3]::GetClassNameW($h, $sb, 256) | Out-Null
        if ($sb.ToString() -eq "QuotaBarDetailWnd" -and [PG3]::IsWindowVisible($h)) {
            $script:found = $h
            return $false
        }
        return $true
    }
    [PG3]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    return $script:found
}

function Find-Hidden {
    $script:hidden = [IntPtr]::Zero
    $cb = [PG3+EnumCb]{ param($h, $l)
        $sb = New-Object System.Text.StringBuilder 256
        [PG3]::GetClassNameW($h, $sb, 256) | Out-Null
        if ($sb.ToString() -eq "QuotaBarHiddenWnd") { $script:hidden = $h; return $false }
        return $true
    }
    [PG3]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    return $script:hidden
}

function ClickNav([int]$i) {
    ClickXY 112 (98 + 40 * $i)
}

# 任意 DIP 坐标点击（按窗口 DPI 换算为客户区像素）
function ClickXY([int]$dipX, [int]$dipY) {
    $c = New-Object PG3+RECT
    [PG3]::GetClientRect($script:found, [ref]$c) | Out-Null
    [int]$cw = $c.R - $c.L; [int]$ch = $c.B - $c.T
    $g = [System.Drawing.Graphics]::FromHwnd($script:found)
    $scale = $g.DpiX / 96.0
    $g.Dispose()
    [int]$x = [Math]::Round($dipX * $scale)
    [int]$y = [Math]::Round($dipY * $scale)
    if ($x -ge $cw) { $x = $cw - 1 }
    if ($y -ge $ch) { $y = $ch - 1 }
    [int]$lp = $y * 65536 + $x
    [PG3]::PostMessageW($script:found, 0x0200, [IntPtr]::Zero, [IntPtr]$lp) | Out-Null
    [PG3]::PostMessageW($script:found, 0x0201, [IntPtr]1, [IntPtr]$lp) | Out-Null
    [PG3]::PostMessageW($script:found, 0x0202, [IntPtr]0, [IntPtr]$lp) | Out-Null
}

function Snap([string]$name) {
    Start-Sleep -Milliseconds 600
    $r = New-Object PG3+RECT
    [PG3]::GetWindowRect($script:found, [ref]$r) | Out-Null
    [int]$w = $r.R - $r.L; [int]$hgt = $r.B - $r.T
    $bmp = New-Object System.Drawing.Bitmap($w, $hgt)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $bg = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 16, 20, 26))
    $g.FillRectangle($bg, 0, 0, $w, $hgt)
    $hdc = $g.GetHdc()
    [PG3]::PrintWindow($script:found, $hdc, 2) | Out-Null
    $g.ReleaseHdc($hdc)
    $g.Dispose()
    $bmp.Save("D:\Zcode\QuotaBar\build\page_$name.png", [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Host ("saved page_$name.png " + $w + "x" + $hgt)
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
    # 非首次启动只进托盘：给隐藏窗口发托盘消息唤出主窗口
    $hid = Find-Hidden
    if ($hid -eq [IntPtr]::Zero) { Write-Host "hidden window not found"; exit 1 }
    [PG3]::PostMessageW($hid, 0x8005, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    for ($t = 0; $t -lt 40; $t++) {
        Start-Sleep -Milliseconds 250
        $wnd = Find-Detail
        if ($wnd -ne [IntPtr]::Zero) { break }
    }
}
if ($wnd -eq [IntPtr]::Zero) { Write-Host "detail window not found"; exit 1 }

$script:found = $wnd
Start-Sleep -Milliseconds 800
ClickNav 0
Snap "quota"
# 展开第一张卡的「设置 ▾」（卡片带内按钮，内容区估算坐标）
ClickXY 550 292
Snap "quota_expanded"
# 「添加卡片」面板
ClickXY 927 96
Snap "quota_add"
ClickNav 1
Snap "display"

# 窄窗：960×760 DIP → 侧栏收成图标栏（rail nav）
[PG3]::SetWindowPos($script:found, [IntPtr]::Zero, 0, 0, 1926, 1566, 0x0004) | Out-Null
Start-Sleep -Milliseconds 800
Snap "display_rail"
ClickXY 28 124
Snap "quota_rail"

[PG3]::PostMessageW($script:found, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 500
Write-Host "done"
