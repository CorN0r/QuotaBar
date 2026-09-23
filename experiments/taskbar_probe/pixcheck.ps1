# pixcheck.ps1 - capture region (physical px) and count pixels near a target color
# usage: pixcheck.ps1 -X 280 -Y 280 -W 330 -H 120 [-Target magenta|orange|green|yellow]
param([int]$X = 280, [int]$Y = 280, [int]$W = 330, [int]$H = 120, [string]$Target = "magenta")
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class WPix {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
}
'@
[WPix]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
Add-Type -AssemblyName System.Drawing
$b = New-Object System.Drawing.Bitmap($W, $H)
$g2 = [System.Drawing.Graphics]::FromImage($b)
$g2.CopyFromScreen($X, $Y, 0, 0, $b.Size)
$hit = 0
for ($y = 0; $y -lt $b.Height; $y++) {
    for ($x = 0; $x -lt $b.Width; $x++) {
        $c = $b.GetPixel($x, $y)
        switch ($Target) {
            "magenta" { if ($c.R -gt 230 -and $c.G -lt 60 -and $c.B -gt 230) { $hit++ } }
            "orange"  { if ($c.R -gt 220 -and $c.G -gt 130 -and $c.G -lt 210 -and $c.B -lt 140) { $hit++ } }
            "green"   { if ($c.G -gt 170 -and $c.R -lt 140 -and $c.B -lt 180) { $hit++ } }
            "yellow"  { if ($c.R -gt 230 -and $c.G -gt 230 -and $c.B -lt 60) { $hit++ } }
        }
    }
}
"region($X,$Y ${W}x${H}) $Target=$hit"
$b.Dispose()
