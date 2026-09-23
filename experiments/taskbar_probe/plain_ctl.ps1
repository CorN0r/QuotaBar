# plain_ctl.ps1 - plain (non-layered) GDI window visibility control test
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class WP {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
}
'@
[WP]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$f = New-Object System.Windows.Forms.Form
$f.Text = "plain control"
$f.StartPosition = 'Manual'
$f.Location = New-Object System.Drawing.Point(300, 300)
$f.Size = New-Object System.Drawing.Size(299, 82)
$f.TopMost = $true
$f.ShowInTaskbar = $false
$f.BackColor = [System.Drawing.Color]::Yellow

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 2500
$checked = $false
$f.Add_Shown({ $timer.Start() })
$timer.Add_Tick({
    if (-not $checked) {
        $checked = $true
        $b = New-Object System.Drawing.Bitmap(330, 120)
        $g2 = [System.Drawing.Graphics]::FromImage($b)
        $g2.CopyFromScreen(280, 280, 0, 0, $b.Size)
        $yellow = 0
        for ($y = 0; $y -lt $b.Height; $y++) {
            for ($x = 0; $x -lt $b.Width; $x++) {
                $c = $b.GetPixel($x, $y)
                if ($c.R -gt 230 -and $c.G -gt 230 -and $c.B -lt 60) { $yellow++ }
            }
        }
        "region(280,280 330x120): yellow=$yellow (form at 300,300 299x82)"
        $b.Dispose()
    } else {
        $f.Close()
    }
})
$f.ShowDialog() | Out-Null
"form closed"
