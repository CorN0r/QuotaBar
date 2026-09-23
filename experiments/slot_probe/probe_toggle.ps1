$out = "tray_restored.txt"
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class KB3 {
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    public static void TapWin() {
        keybd_event(0x5B, 0, 0, UIntPtr.Zero);
        keybd_event(0x5B, 0, 2, UIntPtr.Zero);
    }
}
"@
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class TB4 {
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string win);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L; public int T; public int R; public int B; }
    public static string Cls(IntPtr h){ var sb=new StringBuilder(256); GetClassNameW(h,sb,256); return sb.ToString(); }
    public static string Rect(IntPtr h){ RECT r; GetWindowRect(h, out r); return r.L+","+r.T+","+r.R+","+r.B+" ("+(r.R-r.L)+"x"+(r.B-r.T)+")"; }
}
"@
[TB4]::EnumWindows({ param($h, $lp)
    if ([TB4]::Cls($h) -eq "Shell_TrayWnd") { [KB3]::TapWin(); return $false }
    return $true
}, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 2000
$sb = New-Object System.Text.StringBuilder
function Add-Line($s) { [void]$sb.AppendLine($s) }
Add-Line ("=== probe(WIN TOGGLE, after) @ " + (Get-Date -Format "HH:mm:ss.fff") + " ===")
$tray = [TB4]::FindWindowW("Shell_TrayWnd", $null)
$cbChild = [TB4+EnumProc]{ param($h, $lp)
    Add-Line ("  child " + [TB4]::Cls($h) + " | vis=" + [TB4]::IsWindowVisible($h) + " | " + [TB4]::Rect($h))
    return $true
}
[TB4]::EnumChildWindows($tray, $cbChild, [IntPtr]::Zero) | Out-Null
[System.IO.File]::WriteAllText($out, $sb.ToString())
Write-Host "written: $out"
