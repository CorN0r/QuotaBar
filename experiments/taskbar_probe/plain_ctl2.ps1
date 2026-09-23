# plain_ctl2.ps1 - plain window control test, synchronous pumping, no event closures
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class WP2 {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
}
'@
[WP2]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
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
$l = New-Object System.Windows.Forms.Label
$l.Text = "PLAIN GDI CONTROL"
$l.Dock = 'Fill'
$l.TextAlign = 'MiddleCenter'
$f.Controls.Add($l)

$f.Show()
$deadline = [DateTime]::Now.AddMilliseconds(1200)
while ([DateTime]::Now -lt $deadline) {
    [System.Windows.Forms.Application]::DoEvents()
    Start-Sleep -Milliseconds 50
}
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
"region(280,280 330x120): yellow=$yellow (form should be at 300,300 299x82)"
$b.Dispose()
$f.Close()
