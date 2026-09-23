# scan.ps1 - full virtual-screen pixel scan for probe text colors (orange/green/gray)
param([int]$Step = 4)
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class WScan {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
}
'@
[WScan]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$scr = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$sw = $scr.Width * 2; $sh = $scr.Height * 2  # physical px at 200%
"virtual: $sw x $sh (step $Step)"
$b = New-Object System.Drawing.Bitmap($sw, $sh)
$g2 = [System.Drawing.Graphics]::FromImage($b)
$g2.CopyFromScreen(0, 0, 0, 0, $b.Size)

$orange = New-Object System.Collections.ArrayList
$green = New-Object System.Collections.ArrayList
for ($y = 0; $y -lt $sh; $y += $Step) {
    for ($x = 0; $x -lt $sw; $x += $Step) {
        $c = $b.GetPixel($x, $y)
        if ($c.R -gt 220 -and $c.G -gt 130 -and $c.G -lt 210 -and $c.B -lt 140) {
            if ($orange.Count -lt 8) { [void]$orange.Add("($x,$y)") }
        }
        if ($c.G -gt 170 -and $c.R -lt 140 -and $c.B -lt 180) {
            if ($green.Count -lt 8) { [void]$green.Add("($x,$y)") }
        }
    }
}
"orange: count>=$($orange.Count) @ $($orange -join ' ')"
"green:  count>=$($green.Count) @ $($green -join ' ')"
$b.Dispose()
