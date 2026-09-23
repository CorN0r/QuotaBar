Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap("D:\slotzoom.png")
[int]$W = $bmp.Width; [int]$H = $bmp.Height
# 4x upscale of 492x80 physical -> text rows: name row top half, value row bottom half.
# value row occupies roughly y in [H/2, H]; scan it.
[int]$y0 = [int]($H * 0.52); [int]$y1 = [int]($H * 0.98)
# find bright (text) pixels; taskbar dark background. Print ASCII art of value row,
# compressed: sample every 2 px horizontally, every 2 vertically to fit console.
$sb = New-Object System.Text.StringBuilder
for ([int]$y = $y0; $y -lt $y1; $y += 2) {
    $line = ""
    for ([int]$x = 0; $x -lt $W; $x += 2) {
        $c = $bmp.GetPixel($x, $y)
        $lum = ($c.R * 3 + $c.G * 6 + $c.B) / 10
        if ($lum -gt 110) { $line += "#" } else { $line += "." }
    }
    [void]$sb.AppendLine($line)
}
[System.IO.File]::WriteAllText("D:\glyph_ascii.txt", $sb.ToString())
Write-Host ("grid " + ($W/2) + "x" + (($y1-$y0)/2) + " written")
