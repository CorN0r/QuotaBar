# 列出并前置指定进程的可见窗口(输出 hwnd 列表)
param([int]$procId, [string]$bringTitle = "")
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class WinEnum2 {
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern int GetWindowTextW(IntPtr hWnd, [MarshalAs(UnmanagedType.LPWStr)] StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
}
'@
$script:hits = @()
$cb = [WinEnum2+EnumProc]{
    param($h, $lp)
    $pid2 = 0
    [WinEnum2]::GetWindowThreadProcessId($h, [ref]$pid2) | Out-Null
    if ($pid2 -eq $procId -and [WinEnum2]::IsWindowVisible($h)) {
        $sb = New-Object System.Text.StringBuilder 256
        [WinEnum2]::GetWindowTextW($h, $sb, 256) | Out-Null
        $title = $sb.ToString()
        Write-Host ("hwnd={0} title={1}" -f $h, $title)
        if ($bringTitle -eq "" -or $title.Contains($bringTitle)) {
            $script:hits += , @($h, $title)
        }
    }
    return $true
}
[WinEnum2]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
foreach ($hit in $script:hits) {
    [WinEnum2]::ShowWindow([IntPtr]$hit[0], 9) | Out-Null
    [WinEnum2]::SetForegroundWindow([IntPtr]$hit[0]) | Out-Null
}
