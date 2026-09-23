# probe_open.ps1 - open Start menu via synthetic Win key, then dump taskbar tree.
$out = "tray_open.txt"
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class KB {
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    public static void TapWin() {
        keybd_event(0x5B, 0, 0, UIntPtr.Zero);          // VK_LWIN down
        keybd_event(0x5B, 0, 2, UIntPtr.Zero);          // VK_LWIN up (KEYEVENTF_KEYUP)
    }
}
"@

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class TB2 {
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string win);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int max);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L; public int T; public int R; public int B; }
    public static string Cls(IntPtr h){ var sb=new StringBuilder(256); GetClassNameW(h,sb,256); return sb.ToString(); }
    public static string Txt(IntPtr h){ var sb=new StringBuilder(256); GetWindowTextW(h,sb,256); return sb.ToString(); }
    public static string Rect(IntPtr h){ RECT r; GetWindowRect(h, out r); return r.L+","+r.T+","+r.R+","+r.B+" ("+(r.R-r.L)+"x"+(r.B-r.T)+")"; }
}
"@

[TB2]::EnumWindows({ param($h, $lp)
    if ([TB2]::Cls($h) -eq "Shell_TrayWnd") { [KB]::TapWin(); return $false }
    return $true
}, [IntPtr]::Zero) | Out-Null

Start-Sleep -Milliseconds 1800   # wait for Start menu open animation

$sb = New-Object System.Text.StringBuilder
function Add-Line($s) { [void]$sb.AppendLine($s) }
Add-Line ("=== probe(START OPEN) @ " + (Get-Date -Format "HH:mm:ss.fff") + " ===")

# dump all visible CoreWindows (Start menu / search host show up here; ASCII-only filter)
$cbTop = [TB2+EnumProc]{ param($h, $lp)
    $cls = [TB2]::Cls($h)
    if ($cls -eq "Windows.UI.Core.CoreWindow" -and [TB2]::IsWindowVisible($h)) {
        Add-Line ("TOP  " + $cls + " | vis=True | " + [TB2]::Rect($h))
    }
    return $true
}
[TB2]::EnumWindows($cbTop, [IntPtr]::Zero) | Out-Null

$tray = [TB2]::FindWindowW("Shell_TrayWnd", $null)
if ($tray -eq [IntPtr]::Zero) { Add-Line "Shell_TrayWnd NOT FOUND"; }
else {
    Add-Line ("TRAY rect=" + [TB2]::Rect($tray))
    $cbChild = [TB2+EnumProc]{ param($h, $lp)
        Add-Line ("  child " + [TB2]::Cls($h) + " | vis=" + [TB2]::IsWindowVisible($h) + " | " + [TB2]::Rect($h))
        return $true
    }
    [TB2]::EnumChildWindows($tray, $cbChild, [IntPtr]::Zero) | Out-Null
}

[System.IO.File]::WriteAllText($out, $sb.ToString())
Write-Host "written: $out"
