# Toggle the QuotaBar detail window via tray-callback message (WM_APP+2, WM_LBUTTONUP).
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class OD {
    public delegate bool EnumCb(IntPtr h, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumCb cb, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder sb, int n);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
}
"@
$cb = [OD+EnumCb]{ param($h, $l)
    $sb = New-Object System.Text.StringBuilder 256
    [OD]::GetClassNameW($h, $sb, 256) | Out-Null
    if ($sb.ToString() -eq 'QuotaBarHiddenWnd') { [OD]::PostMessageW($h, 0x8002, [IntPtr]::Zero, [IntPtr]0x0202) | Out-Null; return $false }
    return $true
}
[OD]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 900
Write-Host 'toggled'
