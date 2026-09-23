# probe_taskbar.ps1 - dump Shell_TrayWnd child tree + slot window rect.
# Usage: powershell -File probe_taskbar.ps1 <out.txt>
param([string]$OutFile = "dump.txt")

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class TB {
    public delegate bool EnumProc(IntPtr h, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string win);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L; public int T; public int R; public int B; }
    public static string Cls(IntPtr h){ var sb=new StringBuilder(256); GetClassNameW(h,sb,256); return sb.ToString(); }
    public static string Txt(IntPtr h){ var sb=new StringBuilder(256); GetWindowTextW(h,sb,256); return sb.ToString(); }
    public static string Rect(IntPtr h){ RECT r; GetWindowRect(h, out r); return r.L+","+r.T+","+r.R+","+r.B+" ("+(r.R-r.L)+"x"+(r.B-r.T)+")"; }
}
"@

$sb = New-Object System.Text.StringBuilder
function Add-Line($s) { [void]$sb.AppendLine($s) }

Add-Line ("=== probe @ " + (Get-Date -Format "HH:mm:ss.fff") + " ===")

# 1. top-level windows of interest (Start menu open?)
$cbTop = [TB+EnumProc]{ param($h, $lp)
    $cls = [TB]::Cls($h)
    if ($cls -eq "Windows.UI.Core.CoreWindow" -or $cls -eq "Shell_TrayWnd" -or $cls -like "*XamlExplorer*" -or $cls -like "*StartMenu*") {
        Add-Line ("TOP  " + $cls + " | '" + [TB]::Txt($h) + "' | vis=" + [TB]::IsWindowVisible($h) + " | " + [TB]::Rect($h))
    }
    return $true
}
[TB]::EnumWindows($cbTop, [IntPtr]::Zero) | Out-Null

# 2. Shell_TrayWnd children (full descendant walk)
$tray = [TB]::FindWindowW("Shell_TrayWnd", $null)
if ($tray -eq [IntPtr]::Zero) { Add-Line "Shell_TrayWnd NOT FOUND"; }
else {
    Add-Line ("TRAY Shell_TrayWnd rect=" + [TB]::Rect($tray))
    $cbChild = [TB+EnumProc]{ param($h, $lp)
        Add-Line ("  child " + [TB]::Cls($h) + " | vis=" + [TB]::IsWindowVisible($h) + " | " + [TB]::Rect($h))
        return $true
    }
    [TB]::EnumChildWindows($tray, $cbChild, [IntPtr]::Zero) | Out-Null
}

# 3. QuotaBar slot window rect (the embed bar itself)
$slot = [IntPtr]::Zero
$cbSlot = [TB+EnumProc]{ param($h, $lp)
    if ([TB]::Cls($h) -eq "QuotaBarSlotWnd") { Set-Variable -Name found -Value $h -Scope 1 }
    return $true
}
# simpler: FindWindowEx-free approach via EnumWindows with script scope var
$script:slotHwnd = [IntPtr]::Zero
$cbSlot2 = [TB+EnumProc]{ param($h, $lp)
    if ([TB]::Cls($h) -eq "QuotaBarSlotWnd") { $script:slotHwnd = $h; return $false }
    return $true
}
[TB]::EnumWindows($cbSlot2, [IntPtr]::Zero) | Out-Null
if ($script:slotHwnd -ne [IntPtr]::Zero) {
    Add-Line ("SLOT QuotaBarSlotWnd rect=" + [TB]::Rect($script:slotHwnd) + " vis=" + [TB]::IsWindowVisible($script:slotHwnd))
} else {
    Add-Line "SLOT QuotaBarSlotWnd NOT FOUND"
}

[System.IO.File]::WriteAllText($OutFile, $sb.ToString())
Write-Host "written: $OutFile"
