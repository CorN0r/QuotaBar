# 裁剪放大截图区域：crop_zoom.ps1 <src.png> <out.png> <x> <y> <w> <h> [scale]
param([string]$src, [string]$out, [int]$x, [int]$y, [int]$w, [int]$h, [int]$scale = 2)
Add-Type -AssemblyName System.Drawing
$img = [System.Drawing.Image]::FromFile($src)
$bmp = New-Object System.Drawing.Bitmap([int]($w * $scale), [int]($h * $scale))
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$dstRect = New-Object System.Drawing.Rectangle(0, 0, [int]($w * $scale), [int]($h * $scale))
$srcRect = New-Object System.Drawing.Rectangle($x, $y, $w, $h)
$g.DrawImage($img, $dstRect, $srcRect, [System.Drawing.GraphicsUnit]::Pixel)
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose()
$bmp.Dispose()
$img.Dispose()
Write-Host "saved $out"
