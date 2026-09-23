Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class HW7 {
  public delegate bool EnumCb(IntPtr h, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumCb cb, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder sb, int n);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L; public int T; public int R; public int B; }
}
"@
$hidden = [IntPtr]::Zero
$det = [IntPtr]::Zero
$cb = [HW7+EnumCb]{ param($h, $l)
  $sb = New-Object System.Text.StringBuilder 256
  [HW7]::GetClassNameW($h, $sb, 256) | Out-Null
  $c = $sb.ToString()
  if ($c -eq 'QuotaBarHiddenWnd') { $script:hidden = $h }
  if ($c -eq 'QuotaBarDetailWnd' -and [HW7]::IsWindowVisible($h)) { $script:det = $h; return $false }
  return $true
}
[HW7]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
if ($det -eq [IntPtr]::Zero) {
  [HW7]::PostMessageW($hidden, 0x8005, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
  Start-Sleep -Milliseconds 1200
  $det = [IntPtr]::Zero
  [HW7]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
}
if ($det -eq [IntPtr]::Zero) { Write-Host 'detail not open'; exit 1 }
$dpi = [HW7]::GetDpiForWindow($det)
Write-Host "window dpi=$dpi"
function Click([int]$cx, [int]$cy) {
  [HW7]::PostMessageW($det, 0x0202, [IntPtr]::Zero, [IntPtr](($cy -shl 16) -bor ($cx -band 0xFFFF))) | Out-Null
}
function Snap([string]$name) {
  Start-Sleep -Milliseconds 400
  $r = New-Object HW7+RECT
  [HW7]::GetWindowRect($det, [ref]$r) | Out-Null
  $w = $r.R - $r.L; $h = $r.B - $r.T
  $bmp = New-Object System.Drawing.Bitmap($w, $h)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  [HW7]::PrintWindow($det, $hdc, 0) | Out-Null
  $g.ReleaseHdc($hdc); $g.Dispose()
  $bmp.Save(("D:\Zcode\QuotaBar\build\" + $name + ".png"), [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  Write-Host ("saved " + $name + " " + $w + "x" + $h)
}
# current rect = reference (whatever monitor/dpi the app chose); keep position
$wr0 = New-Object HW7+RECT
[HW7]::GetWindowRect($det, [ref]$wr0) | Out-Null
$W0 = $wr0.R - $wr0.L; $H0 = $wr0.B - $wr0.T
# quota page: nav item 0 at DIP (224/2=112? no) - ratio of design 2026x1631 mapped to current
$navx = [int]($W0 * 112.0 / 2026.0); $navy = [int]($H0 * 110.0 / 1631.0)
Click $navx $navy
Start-Sleep -Milliseconds 500
# narrow (same position, half width)
[HW7]::SetWindowPos($det, [IntPtr]::Zero, $wr0.L, $wr0.T, [int]($W0/2), $H0, 0x0004) | Out-Null
Start-Sleep -Milliseconds 600
Snap 'r_narrow'
# back to default
[HW7]::SetWindowPos($det, [IntPtr]::Zero, $wr0.L, $wr0.T, $W0, $H0, 0x0004) | Out-Null
Start-Sleep -Milliseconds 600
# GLM card toggle (right card header) - map design coords to current size
$tx = [int]($W0 * 1772.0 / 2026.0); $ty = [int]($H0 * 795.0 / 1631.0)
Click $tx $ty
Snap 'r_glm_off'
# hover Kimi note icon
$hx = [int]($W0 * 536.0 / 2026.0); $hy = [int]($H0 * 540.0 / 1631.0)
[HW7]::PostMessageW($det, 0x0200, [IntPtr]::Zero, [IntPtr](($hy -shl 16) -bor ($hx -band 0xFFFF))) | Out-Null
Snap 'r_hover'
# restore toggle
Click $tx $ty
Snap 'r_restored'
Write-Host 'done'
