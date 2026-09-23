$out = "restored.txt"
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Mouse {
    [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public MOUSEINPUT mi; }
    [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx; public int dy; public uint mouseData; public uint dwFlags; public uint time; public UIntPtr dwExtraInfo; }
    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
    public static void Click(int x, int y) {
        var inp = new INPUT[3];
        inp[0].type = 0; inp[0].mi.dx = x; inp[0].mi.dy = y; inp[0].mi.dwFlags = 0x8000 | 0x0001; // ABSOLUTE|MOVE
        inp[1].type = 0; inp[1].mi = inp[0].mi; inp[1].mi.dwFlags = 0x0002; // LEFTDOWN
        inp[2].type = 0; inp[2].mi = inp[0].mi; inp[2].mi.dwFlags = 0x0004; // LEFTUP
        SendInput(3, inp, Marshal.SizeOf(typeof(INPUT)));
    }
}
"@
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class TB7 {
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string win);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr p, IntPtr a, string cls, string win);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L; public int T; public int R; public int B; }
    public static string Cls(IntPtr h){ var sb=new StringBuilder(256); GetClassNameW(h,sb,256); return sb.ToString(); }
    public static string Rect(IntPtr h){ RECT r; GetWindowRect(h, out r); return r.L+","+r.T+","+r.R+","+r.B+" ("+(r.R-r.L)+"x"+(r.B-r.T)+")"; }
}
"@
# click desktop wallpaper (physical 244,1219) to dismiss Start
[Mouse]::Click(5125, 38414)
Start-Sleep -Milliseconds 2000
$sb = New-Object System.Text.StringBuilder
function Add-Line($s) { [void]$sb.AppendLine($s) }
Add-Line ("=== after dismissal @ " + (Get-Date -Format "HH:mm:ss.fff") + " ===")
$tray = [TB7]::FindWindowW("Shell_TrayWnd", $null)
$cb = [TB7+EnumProc]{ param($h, $lp)
    Add-Line ("  child " + [TB7]::Cls($h) + " vis=" + [TB7]::IsWindowVisible($h) + " " + [TB7]::Rect($h))
    return $true
}
[TB7]::EnumChildWindows($tray, $cb, [IntPtr]::Zero) | Out-Null
$slot = [TB7]::FindWindowExW($tray, [IntPtr]::Zero, "QuotaBarSlotWnd", $null)
Add-Line ("slot hwnd=" + $slot + " rect=" + [TB7]::Rect($slot))
[System.IO.File]::WriteAllText($out, $sb.ToString())
Write-Host "written: $out"
