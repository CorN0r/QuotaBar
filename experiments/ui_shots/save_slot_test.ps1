Add-Type @"
using System;using System.Runtime.InteropServices;using System.Text;
public class SV {
  public delegate bool EnumCb(IntPtr h, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumCb cb, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder sb, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L; public int T; public int R; public int B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
}
"@
Add-Type @"
using System.Runtime.InteropServices;
public static class DpiSV { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }
"@
[DpiSV]::SetProcessDPIAware() | Out-Null

$det = [IntPtr]::Zero
$hidden = [IntPtr]::Zero
$cb = [SV+EnumCb]{ param($h, $l)
  $sb = New-Object System.Text.StringBuilder 256
  [SV]::GetClassNameW($h, $sb, 256) | Out-Null
  $c = $sb.ToString()
  if ($c -eq 'QuotaBarDetailWnd' -and [SV]::IsWindowVisible($h)) { $script:det = $h; return $false }
  if ($c -eq 'QuotaBarHiddenWnd') { $script:hidden = $h }
  return $true }
[SV]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
if ($det -eq [IntPtr]::Zero) {
  [SV]::PostMessageW($hidden, 0x8005, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
  Start-Sleep -Milliseconds 1500
  [SV]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
}
if ($det -eq [IntPtr]::Zero) { Write-Host 'window not open'; exit 1 }

function ClickXY([int]$x, [int]$y) {
  [int]$lp = $y * 65536 + $x
  [SV]::PostMessageW($det, 0x0201, [IntPtr]::Zero, [IntPtr]$lp) | Out-Null
  Start-Sleep -Milliseconds 80
  [SV]::PostMessageW($det, 0x0202, [IntPtr]::Zero, [IntPtr]$lp) | Out-Null
}
function Slot() {
  $j = Get-Content "$env:LOCALAPPDATA\QuotaBar\config.json" -Raw -Encoding UTF8 | ConvertFrom-Json
  return [int]$j.display.slot_offset_x_dip
}
# nav item click by ratio of client rect (proven ratios from shot_pages.ps1)
function ClickNav([int]$i) {
  $c = New-Object SV+RECT
  [SV]::GetClientRect($det, [ref]$c) | Out-Null
  [int]$cw = $c.R - $c.L; [int]$ch = $c.B - $c.T
  [int]$x = [Math]::Round($cw * 112.0 / 1000.0)
  [int]$y = [Math]::Round($ch * (110.0 + 40.0 * $i) / 780.0)
  ClickXY $x $y
}

Write-Host ("slot baseline: " + (Slot))

# go to display page (nav item 1)
ClickNav 1
Start-Sleep -Milliseconds 500

# 1) save BEFORE touching slider: config must stay at baseline (-57)
ClickXY 1870 1567
Start-Sleep -Milliseconds 400
Write-Host ("slot after save (no adjust): " + (Slot))

# 2) drag position slider to 20% (value ~= -120): down then up
$trackL = 544; $trackR = 1096
$x = [int]($trackL + 0.20 * ($trackR - $trackL)); $y = 1100
$lp = $y * 65536 + $x
[SV]::PostMessageW($det, 0x0201, [IntPtr]::Zero, [IntPtr]$lp) | Out-Null
Start-Sleep -Milliseconds 120
[SV]::PostMessageW($det, 0x0202, [IntPtr]::Zero, [IntPtr]$lp) | Out-Null
Start-Sleep -Milliseconds 400
$s2 = Slot
Write-Host ("slot after slider release: " + $s2 + " (expect about -120)")

# 3) click save: slot must STAY at slider value (bug used to revert to -57)
ClickXY 1870 1567
Start-Sleep -Milliseconds 400
$s3 = Slot
Write-Host ("slot after save: " + $s3 + " (must equal slider value)")
if ($s3 -eq $s2) { Write-Host "PASS: save keeps live-adjusted slot" } else { Write-Host "FAIL: save reverted slot" }

# 4) restore original -57 via slider (x calibrated from -60@743 reading: +3 DIP = +4 px)
$x = 747
$lp2 = 1100 * 65536 + $x
[SV]::PostMessageW($det, 0x0201, [IntPtr]::Zero, [IntPtr]$lp2) | Out-Null
Start-Sleep -Milliseconds 120
[SV]::PostMessageW($det, 0x0202, [IntPtr]::Zero, [IntPtr]$lp2) | Out-Null
Start-Sleep -Milliseconds 400
Write-Host ("slot restored to: " + (Slot) + " (target -57)")

# close window
[SV]::PostMessageW($det, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Write-Host "done"
