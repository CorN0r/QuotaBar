$out = "chain_state.txt"
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class TB6 {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string win);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr p, IntPtr a, string cls, string win);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L; public int T; public int R; public int B; }
    public static string Cls(IntPtr h){ var sb=new StringBuilder(256); GetClassNameW(h,sb,256); return sb.ToString(); }
    public static string Rect(IntPtr h){ RECT r; GetWindowRect(h, out r); return r.L+","+r.T+","+r.R+","+r.B+" ("+(r.R-r.L)+"x"+(r.B-r.T)+")"; }
}
"@
$sb = New-Object System.Text.StringBuilder
function Add-Line($s) { [void]$sb.AppendLine($s) }
Add-Line ("=== direct-children chain @ " + (Get-Date -Format "HH:mm:ss.fff") + " ===")
$tray = [TB6]::FindWindowW("Shell_TrayWnd", $null)
Add-Line ("Shell_TrayWnd=" + $tray)
$after = [IntPtr]::Zero
$n = 0
do {
    $h = [TB6]::FindWindowExW($tray, $after, $null, $null)
    if ($h -eq [IntPtr]::Zero) { break }
    $n++
    Add-Line ("  [" + $n + "] hwnd=" + $h + " " + [TB6]::Cls($h) + " vis=" + [TB6]::IsWindowVisible($h) + " " + [TB6]::Rect($h))
    $after = $h
} while ($n -lt 40)
[System.IO.File]::WriteAllText($out, $sb.ToString())
Write-Host "written: $out"
