# 抓取指定窗口内容(PrintWindow,即使被遮挡也可截)
param([int]$hwnd, [string]$out, [int]$w = 1546, [int]$h = 1031)
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public class Win32P {
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, HDC hdcBlt, uint nFlags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
    public struct RECT { public int L, T, R, B; }
}
'@ -ReferencedAssemblies System.Drawing 2>$null
if (-not ('Win32P' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public class Win32P {
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
    public struct RECT { public int L; public int T; public int R; public int B; }
}
'@
}
$h = [IntPtr]$hwnd
$r = New-Object Win32P+RECT
[Win32P]::GetWindowRect($h, [ref]$r) | Out-Null
$ww = $r.R - $r.L; $wh = $r.B - $r.T
if ($ww -le 0 -or $wh -le 0) { throw "bad rect" }
$bmp = New-Object System.Drawing.Bitmap($ww, $wh)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[Win32P]::PrintWindow($h, $hdc, 2) | Out-Null   # PW_RENDERFULLCONTENT
$g.ReleaseHdc($hdc)
$g.Dispose()
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "saved $out ($ww x $wh)"
