# 生成 assets/quota_bar.ico(16/24/32/48/64/128/256;256 为 PNG 帧,其余 BMP+Alpha)。
# 造型:深色圆角底片 + 三色弧环(蓝→紫 270° 弧 + 青色端点圆),呼应圆环呈现模式。
# 用法: powershell -NoProfile -ExecutionPolicy Bypass -File tools\gen_icon.ps1
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$out = Join-Path $root 'assets\quota_bar.ico'

function New-RoundPath([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc($x, $y, $r, $r, 180, 90)
    $path.AddArc($x + $w - $r, $y, $r, $r, 270, 90)
    $path.AddArc($x + $w - $r, $y + $h - $r, $r, $r, 0, 90)
    $path.AddArc($x, $y + $h - $r, $r, $r, 90, 90)
    $path.CloseFigure()
    return $path
}

function New-IconBitmap([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)

    # 底片:深色圆角方块,纵向微渐变
    $tile = New-RoundPath 0 0 $size $size ([Math]::Max(2, [int]($size * 0.22)))
    $trect = New-Object System.Drawing.Rectangle(0, 0, $size, $size)
    $tbrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush($trect,
        [System.Drawing.Color]::FromArgb(255, 40, 44, 54),
        [System.Drawing.Color]::FromArgb(255, 20, 22, 27),
        [System.Drawing.Drawing2D.LinearGradientMode]::Vertical)
    $g.FillPath($tbrush, $tile)
    # 内描边(高光)
    $border = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(28, 255, 255, 255), [Math]::Max(1, [int]($size * 0.02)))
    $g.DrawPath($border, $tile)

    # 弧环:轨道 + 蓝→紫 270° 弧(圆帽)+ 青色端点圆
    $cx = [float]($size * 0.50); $cy = [float]($size * 0.54)
    $r = [float]($size * 0.28)
    $stroke = [Math]::Max(1.5, [float]($size * 0.11))
    $rd = $r * 2
    $rrc = New-Object System.Drawing.RectangleF(([float]($cx - $r)), ([float]($cy - $r)), $rd, $rd)
    $track = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 56, 61, 74), $stroke)
    $track.StartCap = 'Round'; $track.EndCap = 'Round'
    $g.DrawArc($track, $rrc, 0, 360)

    function New-ArcPen([int]$rgb, [float]$w) {
        $p = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, [int](($rgb -shr 16) -band 255), [int](($rgb -shr 8) -band 255), [int]($rgb -band 255)), $w)
        $p.StartCap = 'Round'; $p.EndCap = 'Round'
        return $p
    }
    $blue = New-ArcPen 0x5B8CFF $stroke
    $violet = New-ArcPen 0x8A6CFF $stroke
    $g.DrawArc($blue, $rrc, -90, 135)   # 顶 → 右下
    $g.DrawArc($violet, $rrc, 45, 135)  # 右下 → 左侧

    # 端点圆(青):弧结束于 180°(正左)
    $dotR = [Math]::Max(1.5, [float]($size * 0.075))
    $dotBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 58, 208, 184))
    $g.FillEllipse($dotBrush, $cx - $r - $dotR, $cy - $dotR, $dotR * 2, $dotR * 2)

    $g.Dispose()
    return $bmp
}

# 将 Bitmap 编码为 ICO 单帧（BITMAPINFOHEADER + XOR BGRA 自底向上 + AND 掩码）
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
    $maskRow = [Math]::Ceiling($w/8.0); $maskRow = [Math]::Ceiling($maskRow/4.0)*4
    $bw.Write((New-Object byte[] ($maskRow*$h)))
    $bw.Flush()
    return ,$ms.ToArray()
}

function Encode-PngFrame([System.Drawing.Bitmap]$bmp) {
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    return ,$ms.ToArray()
}

$sizes = @(16, 24, 32, 48, 64, 128, 256)
$frames = @()
foreach ($s in $sizes) {
    $b = New-IconBitmap $s
    if ($s -ge 256) { $frames += ,(Encode-PngFrame $b) }
    else { $frames += ,(Encode-IcoFrame $b) }
    $b.Dispose()
}
$fs = [System.IO.File]::Create($out)
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$frames.Count)
$offset = 6 + 16*$frames.Count
for ($i = 0; $i -lt $frames.Count; $i++) {
    $s = $sizes[$i]
    $sb = $(if ($s -ge 256) {0} else {$s})
    $bw.Write([byte]$sb); $bw.Write([byte]$sb); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32)
    $bw.Write([uint32]$frames[$i].Length); $bw.Write([uint32]$offset)
    $offset += $frames[$i].Length
}
foreach ($f in $frames) { $bw.Write($f) }
$bw.Flush(); $bw.Dispose()
Write-Host "icon written: $out ($((Get-Item $out).Length) bytes)"
