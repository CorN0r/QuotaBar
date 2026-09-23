param([int]$targetHwnd = 0)
# 找 QuotaBar 进程的详情窗并 PrintWindow 抓两帧(验证横幅渲染稳定)
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class CapW {
    public delegate bool P(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(P c, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetWindowTextW(IntPtr h, [MarshalAs(UnmanagedType.LPWStr)] StringBuilder s, int m);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
    public struct RECT { public int L; public int T; public int R; public int B; }
    public static IntPtr Found = IntPtr.Zero;
    public static uint Target;
}
'@
if ($targetHwnd -ne 0) { [CapW]::Found = [IntPtr]$targetHwnd }
Write-Host ("detail hwnd=" + [CapW]::Found)
if ([CapW]::Found -eq [IntPtr]::Zero) { exit 1 }
$r = New-Object CapW+RECT
[CapW]::GetWindowRect([CapW]::Found, [ref]$r) | Out-Null
$ww = $r.R - $r.L; $wh = $r.B - $r.T
foreach ($frame in @('f1', 'f2')) {
    $bmp = New-Object System.Drawing.Bitmap($ww, $wh)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [CapW]::PrintWindow([CapW]::Found, $hdc, 2) | Out-Null
    $g.ReleaseHdc($hdc)
    $g.Dispose()
    $bmp.Save("D:\Zcode\QuotaBar\experiments\glm_probe\detail_$frame.png", [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Start-Sleep -Milliseconds 500
}
Write-Host "captured $ww x $wh"
