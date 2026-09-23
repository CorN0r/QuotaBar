param([string]$Master = '', [string]$OutputRoot = '')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (!$Master) { $Master = Join-Path $root 'assets/branding/quotabar-master.png' }
if (!$OutputRoot) { $OutputRoot = Join-Path $root 'assets' }
$pngDir = Join-Path $OutputRoot 'branding/png'
New-Item -ItemType Directory -Force $pngDir | Out-Null
$source = [System.Drawing.Bitmap]::FromFile($Master)
if ($source.Width -ne $source.Height) { throw 'Master must be square.' }
function New-IconBitmap([int]$size) {
    $bmp = [System.Drawing.Bitmap]::new($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CompositingMode = 'SourceCopy'
    $g.CompositingQuality = 'HighQuality'
    $g.InterpolationMode = 'HighQualityBicubic'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)
    $attrs = [System.Drawing.Imaging.ImageAttributes]::new()
    $attrs.SetWrapMode([System.Drawing.Drawing2D.WrapMode]::TileFlipXY)
    $g.DrawImage($source, [System.Drawing.Rectangle]::new(0,0,$size,$size), 0,0,$source.Width,$source.Height,[System.Drawing.GraphicsUnit]::Pixel,$attrs)
    $attrs.Dispose(); $g.Dispose()
    return $bmp
}

function Encode-IcoFrame([System.Drawing.Bitmap]$bmp) {
    $w = $bmp.Width; $h = $bmp.Height
    $bd = $bmp.LockBits((New-Object System.Drawing.Rectangle(0,0,$w,$h)),
        [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $xor = New-Object byte[] ($w*$h*4)
    [System.Runtime.InteropServices.Marshal]::Copy($bd.Scan0, $xor, 0, $xor.Length)
    $stride = $bd.Stride
    $bmp.UnlockBits($bd)
    # DIB 行序自底向上且行内 BGRX
    $flip = New-Object byte[] ($w*$h*4)
    for ($y = 0; $y -lt $h; $y++) {
        [Array]::Copy($xor, ($h-1-$y)*$stride, $flip, $y*$w*4, $w*4)
    }
    $ms = New-Object System.IO.MemoryStream
    $bw = New-Object System.IO.BinaryWriter($ms)
    # BITMAPINFOHEADER
    $bw.Write([uint32]40); $bw.Write([int32]$w); $bw.Write([int32]($h*2))
    $bw.Write([uint16]1); $bw.Write([uint16]32); $bw.Write([uint32]0)
    $bw.Write([uint32]($w*$h*4)); $bw.Write([int32]0); $bw.Write([int32]0); $bw.Write([uint32]0); $bw.Write([uint32]0)
    $bw.Write($flip)
    # AND 掩码：每行 $w 位，按 4 字节对齐，全 0（使用 Alpha）
    $maskRow = [int][Math]::Ceiling($w/8.0); $maskRow = [int]([Math]::Ceiling($maskRow/4.0)*4)
    $bw.Write((New-Object byte[] ($maskRow*$h)))
    $bw.Flush()
    return ,$ms.ToArray()
}

function Encode-PngFrame([System.Drawing.Bitmap]$bmp) {
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    return ,$ms.ToArray()
}



$sizes = @(16,20,24,28,30,32,36,40,48,60,64,72,80,96,128,256)
$frames = @()
foreach ($s in $sizes) {
    $bmp = New-IconBitmap $s
    $bmp.Save((Join-Path $pngDir "quotabar-$s.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    if ($s -eq 256) { $frames += ,(Encode-PngFrame $bmp) }
    else { $frames += ,(Encode-IcoFrame $bmp) }
    $bmp.Dispose()
}
foreach ($s in @(512,1024)) {
    $bmp = New-IconBitmap $s
    $bmp.Save((Join-Path $pngDir "quotabar-$s.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}
$out = Join-Path $OutputRoot 'quota_bar.ico'
$bw = [System.IO.BinaryWriter]::new([System.IO.File]::Create($out))
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$frames.Count)
$offset = 6 + 16*$frames.Count
for ($i=0; $i -lt $frames.Count; $i++) {
    $s = $sizes[$i]; $sb = $(if ($s -eq 256) {0} else {$s})
    $bw.Write([byte]$sb); $bw.Write([byte]$sb); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32)
    $bw.Write([uint32]$frames[$i].Length); $bw.Write([uint32]$offset)
    $offset += $frames[$i].Length
}
foreach ($frame in $frames) { $bw.Write($frame) }
$bw.Dispose(); $source.Dispose()
Write-Host "Exported ICO (16 frames) and 18 transparent PNG sizes to $OutputRoot"

