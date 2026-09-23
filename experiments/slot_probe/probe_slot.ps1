$out = "slot_state.txt"
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class TB5 {
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr p, IntPtr a, string cls, string win);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string win);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L; public int T; public int R; public int B; }
    public static string Cls(IntPtr h){ var sb=new StringBuilder(256); GetClassNameW(h,sb,256); return sb.ToString(); }
    public static string Rect(IntPtr h){ RECT r; GetWindowRect(h, out r); return r.L+","+r.T+","+r.R+","+r.B+" ("+(r.R-r.L)+"x"+(r.B-r.T)+")"; }
}
"@
$sb = New-Object System.Text.StringBuilder
function Add-Line($s) { [void]$sb.AppendLine($s) }
Add-Line ("=== slot-state probe @ " + (Get-Date -Format "HH:mm:ss.fff") + " ===")
$tray = [TB5]::FindWindowW("Shell_TrayWnd", $null)
Add-Line ("Shell_TrayWnd=" + $tray)
$slot = [TB5]::FindWindowExW($tray, [IntPtr]::Zero, "QuotaBarSlotWnd", $null)
Add-Line ("direct child via FindWindowExW: " + $slot)
# exhaustive: walk EVERY window (top-level + descendants), find any QuotaBarSlotWnd
$script:found = [IntPtr]::Zero
$cbTop = [TB5+EnumProc]{ param($h, $lp)
    if ([TB5]::Cls($h) -eq "QuotaBarSlotWnd") { $script:found = $h; return $false }
    $cbC = [TB5+EnumProc]{ param($c, $lp2)
        if ([TB5]::Cls($c) -eq "QuotaBarSlotWnd") { $script:found = $c; return $false }
        return (-not ($script:found -ne [IntPtr]::Zero))
    }
    [TB5]::EnumChildWindows($h, $cbC, [IntPtr]::Zero) | Out-Null
    return ($script:found -eq [IntPtr]::Zero)
}
[TB5]::EnumWindows($cbTop, [IntPtr]::Zero) | Out-Null
if ($script:found -ne [IntPtr]::Zero) {
    $p = [TB5]::GetParent($script:found)
    $anc = [TB5]::GetAncestor($script:found, 2)  # GA_ROOT
    Add-Line ("slot window=" + $script:found + " vis=" + [TB5]::IsWindowVisible($script:found) + " rect=" + [TB5]::Rect($script:found))
    Add-Line ("  parent=" + $p + " cls=" + [TB5]::Cls($p))
    Add-Line ("  root  =" + $anc + " cls=" + [TB5]::Cls($anc))
} else {
    Add-Line "NO QuotaBarSlotWnd EXISTS anywhere"
}
[System.IO.File]::WriteAllText($out, $sb.ToString())
Write-Host "written: $out"
