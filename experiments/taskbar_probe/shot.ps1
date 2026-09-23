# shot.ps1 - capture taskbar strip region + probe window state for visual debugging
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Win32Shot {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
    public struct RECT { public int L, T, R, B; }
}
'@
[Win32Shot]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

$h = [Win32Shot]::FindWindowW("Shell_TrayWnd", $null)
if ($h -eq [IntPtr]::Zero) { Write-Host "no Shell_TrayWnd"; exit 1 }
$tr = New-Object Win32Shot+RECT
[Win32Shot]::GetWindowRect($h, [ref]$tr) | Out-Null
Write-Host ("taskbar screen rect: {0},{1} {2}x{3}" -f $tr.L, $tr.T, ($tr.R - $tr.L), ($tr.B - $tr.T))

$ph = [Win32Shot]::FindWindowW("QB_TaskbarProbeWnd", $null)
if ($ph -ne [IntPtr]::Zero) {
    $pr = New-Object Win32Shot+RECT
    [Win32Shot]::GetWindowRect($ph, [ref]$pr) | Out-Null
    $vis = [Win32Shot]::IsWindowVisible($ph)
    $par = [Win32Shot]::GetParent($ph)
    Write-Host ("probe: visible={0} parent=0x{1:X} rect={2},{3} {4}x{5}" -f $vis, $par.ToInt64(), $pr.L, $pr.T, ($pr.R - $pr.L), ($pr.B - $pr.T))
} else {
    Write-Host "probe window NOT FOUND by class QB_TaskbarProbeWnd"
}

# capture region: taskbar full width, plus 40px above it
$left = $tr.L; $top = $tr.T - 40; $w = $tr.R - $tr.L; $hh = ($tr.B - $tr.T) + 40
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap($w, $hh)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($left, $top, 0, 0, $bmp.Size)
$out = "D:\Zcode\QuotaBar\experiments\taskbar_probe\taskbar_shot.png"
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Host "saved: $out"
