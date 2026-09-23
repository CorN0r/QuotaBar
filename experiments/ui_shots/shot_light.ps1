# 浅色主题一次性验证：翻转 AppsUseLightTheme → 启动 → 截额度/设置两页 → 恢复
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class LT1 {
    public delegate bool EnumCb(IntPtr h, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumCb cb, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L; public int T; public int R; public int B; }
}
"@
Add-Type @"
using System.Runtime.InteropServices;
public static class LT2 { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }
"@
[LT2]::SetProcessDPIAware() | Out-Null

$reg = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize"
$old = (Get-ItemProperty -Path $reg -Name AppsUseLightTheme).AppsUseLightTheme
Write-Host "old AppsUseLightTheme=$old"
Set-ItemProperty -Path $reg -Name AppsUseLightTheme -Value 1
try {
    Start-Process -FilePath "D:\Zcode\QuotaBar\build\QuotaBar.exe" | Out-Null
    $wnd = [IntPtr]::Zero
    for ($t = 0; $t -lt 60; $t++) {
        Start-Sleep -Milliseconds 500
        $cb = [LT1+EnumCb]{ param($h, $l)
            $sb = New-Object System.Text.StringBuilder 256
            [LT1]::GetClassNameW($h, $sb, 256) | Out-Null
            if ($sb.ToString() -eq "QuotaBarDetailWnd" -and [LT1]::IsWindowVisible($h)) { $script:wnd = $h; return $false }
            return $true
        }
        $script:wnd = [IntPtr]::Zero
        [LT1]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
        $wnd = $script:wnd
        if ($wnd -ne [IntPtr]::Zero) { break }
    }
    if ($wnd -eq [IntPtr]::Zero) {
        # 非首次启动只进托盘：唤醒主窗口
        $hid = [IntPtr]::Zero
        $cb2 = [LT1+EnumCb]{ param($h, $l)
            $sb = New-Object System.Text.StringBuilder 256
            [LT1]::GetClassNameW($h, $sb, 256) | Out-Null
            if ($sb.ToString() -eq "QuotaBarHiddenWnd") { $script:hid = $h; return $false }
            return $true
        }
        $script:hid = [IntPtr]::Zero
        [LT1]::EnumWindows($cb2, [IntPtr]::Zero) | Out-Null
        $hid = $script:hid
        if ($hid -ne [IntPtr]::Zero) {
            [LT1]::PostMessageW($hid, 0x8005, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
            for ($t = 0; $t -lt 40; $t++) {
                Start-Sleep -Milliseconds 500
                $cb = [LT1+EnumCb]{ param($h, $l)
                    $sb = New-Object System.Text.StringBuilder 256
                    [LT1]::GetClassNameW($h, $sb, 256) | Out-Null
                    if ($sb.ToString() -eq "QuotaBarDetailWnd" -and [LT1]::IsWindowVisible($h)) { $script:wnd = $h; return $false }
                    return $true
                }
                $script:wnd = [IntPtr]::Zero
                [LT1]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
                $wnd = $script:wnd
                if ($wnd -ne [IntPtr]::Zero) { break }
            }
        }
    }
    if ($wnd -eq [IntPtr]::Zero) { Write-Host "window not found"; exit 1 }
    Start-Sleep -Milliseconds 1500
    $r = New-Object LT1+RECT
    [LT1]::GetWindowRect($wnd, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $hgt = $r.B - $r.T
    $bmp = New-Object System.Drawing.Bitmap($w, $hgt)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $bg = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 247, 249, 252))
    $g.FillRectangle($bg, 0, 0, $w, $hgt)
    $hdc = $g.GetHdc()
    [LT1]::PrintWindow($wnd, $hdc, 2) | Out-Null
    $g.ReleaseHdc($hdc); $g.Dispose()
    $bmp.Save("D:\Zcode\QuotaBar\build\page_light.png", [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Host "saved page_light.png $w x $hgt"
    # 关闭窗口（走 WM_CLOSE → 退出）
    [LT1]::PostMessageW($wnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 800
} finally {
    Set-ItemProperty -Path $reg -Name AppsUseLightTheme -Value $old
    Write-Host "restored AppsUseLightTheme=$old"
}
