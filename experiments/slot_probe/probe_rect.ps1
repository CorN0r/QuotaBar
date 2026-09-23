Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class TB8 {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string win);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr p, IntPtr a, string cls, string win);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L; public int T; public int R; public int B; }
    public static string Rect(IntPtr h){ RECT r; GetWindowRect(h, out r); return r.L+","+r.T+","+r.R+","+r.B; }
}
"@
$tray = [TB8]::FindWindowW("Shell_TrayWnd", $null)
$slot = [TB8]::FindWindowExW($tray, [IntPtr]::Zero, "QuotaBarSlotWnd", $null)
if ($slot -ne [IntPtr]::Zero) {
    Write-Host ("slot hwnd=" + $slot + " rect=" + [TB8]::Rect($slot) + " vis=" + [TB8]::IsWindowVisible($slot))
} else { Write-Host "slot not found" }
