# probe_close.ps1 - close Start menu via Esc, then dump taskbar tree.
$out = "tray_reclosed.txt"
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class KB2 {
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    public static void TapEsc() {
        keybd_event(0x1B, 0, 0, UIntPtr.Zero);          // VK_ESCAPE down
        keybd_event(0x1B, 0, 2, UIntPtr.Zero);          // VK_ESCAPE up
    }
}
"@
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class TB3 {
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

[TB3]::EnumWindows({ param($h, $lp)
    if ([TB3]::Cls($h) -eq "Shell_TrayWnd") { [KB2]::TapEsc(); return $false }
    return $true
}, [IntPtr]::Zero) | Out-Null

Start-Sleep -Milliseconds 1500

$sb = New-Object System.Text.StringBuilder
function Add-Line($s) { [void]$sb.AppendLine($s) }
Add-Line ("=== probe(AFTER ESC) @ " + (Get-Date -Format "HH:mm:ss.fff") + " ===")
$tray = [TB3]::FindWindowW("Shell_TrayWnd", $null)
if ($tray -eq [IntPtr]::Zero) { Add-Line "Shell_TrayWnd NOT FOUND"; }
else {
    Add-Line ("TRAY rect=" + [TB3]::Rect($tray))
    $cbChild = [TB3+EnumProc]{ param($h, $lp)
        Add-Line ("  child " + [TB3]::Cls($h) + " | vis=" + [TB3]::IsWindowVisible($h) + " | " + [TB3]::Rect($h))
        return $true
    }
    [TB3]::EnumChildWindows($tray, $cbChild, [IntPtr]::Zero) | Out-Null
}
[System.IO.File]::WriteAllText($out, $sb.ToString())
Write-Host "written: $out"
