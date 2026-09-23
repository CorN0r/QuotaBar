Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class SM1 {
    public delegate bool EnumCb(IntPtr h, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumCb cb, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L; public int T; public int R; public int B; }
}
"@
$hid = [IntPtr]::Zero
$cb = [SM1+EnumCb]{ param($h, $l)
    $sb = New-Object System.Text.StringBuilder 256
    [SM1]::GetClassNameW($h, $sb, 256) | Out-Null
    if ($sb.ToString() -eq "QuotaBarHiddenWnd") { $script:hid = $h; return $false }
    return $true
}
[SM1]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
if ($script:hid -eq [IntPtr]::Zero) { Write-Host "hidden not found"; exit 1 }
[SM1]::PostMessageW($script:hid, 0x8005, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 1500
$det = [IntPtr]::Zero
$cb2 = [SM1+EnumCb]{ param($h, $l)
    $sb = New-Object System.Text.StringBuilder 256
    [SM1]::GetClassNameW($h, $sb, 256) | Out-Null
    if ($sb.ToString() -eq "QuotaBarDetailWnd" -and [SM1]::IsWindowVisible($h)) { $script:det = $h; return $false }
    return $true
}
[SM1]::EnumWindows($cb2, [IntPtr]::Zero) | Out-Null
if ($script:det -eq [IntPtr]::Zero) { Write-Host "detail not found"; exit 1 }
$r = New-Object SM1+RECT
[SM1]::GetWindowRect($script:det, [ref]$r) | Out-Null
Write-Host ("detail rect: " + $r.L + "," + $r.T + " " + ($r.R-$r.L) + "x" + ($r.B-$r.T))
