Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class HWH {
  public delegate bool EnumCb(IntPtr h, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumCb cb, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder sb, int n);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L; public int T; public int R; public int B; }
}
"@
Add-Type @"
using System.Runtime.InteropServices;
public static class DpiQH { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }
"@
[DpiQH]::SetProcessDPIAware() | Out-Null
$hidden = [IntPtr]::Zero
$det = [IntPtr]::Zero
$cb = [HWH+EnumCb]{ param($h, $l)
  $sb = New-Object System.Text.StringBuilder 256
  [HWH]::GetClassNameW($h, $sb, 256) | Out-Null
  $c = $sb.ToString()
  if ($c -eq 'QuotaBarHiddenWnd') { $script:hidden = $h }
  if ($c -eq 'QuotaBarDetailWnd' -and [HWH]::IsWindowVisible($h)) { $script:det = $h; return $false }
  return $true
}
[HWH]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
if ($det -eq [IntPtr]::Zero) {
  [HWH]::PostMessageW($hidden, 0x8005, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
  Start-Sleep -Milliseconds 1200
  $det = [IntPtr]::Zero
  [HWH]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
}
if ($det -eq [IntPtr]::Zero) { Write-Host 'not open'; exit 1 }
function SnapH([string]$name) {
  $r = New-Object HWH+RECT
  [HWH]::GetWindowRect($det, [ref]$r) | Out-Null
  $w = $r.R - $r.L; $h = $r.B - $r.T
  $bmp = New-Object System.Drawing.Bitmap($w, $h)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  [HWH]::PrintWindow($det, $hdc, 0) | Out-Null
  $g.ReleaseHdc($hdc); $g.Dispose()
  $bmp.Save(("D:\Zcode\QuotaBar\build\" + $name + ".png"), [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  Write-Host ("saved " + $name)
}
function MoveMouse([int]$x, [int]$y) {
  [HWH]::PostMessageW($det, 0x0200, [IntPtr]::Zero, [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))) | Out-Null
}
# 1) toggle hover (control target)
MoveMouse 1005 320
Start-Sleep -Milliseconds 250
SnapH 'h2_toggle'
# 2) icon hover, capture right away
MoveMouse 822 348
SnapH 'h2_icon'
Start-Sleep -Milliseconds 250
SnapH 'h2_icon2'
Write-Host 'done'
