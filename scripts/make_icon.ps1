# Generates src/AiCompanion.App/Assets/Orua.ico
# Design (mirrors Assets/Orua.svg): dark navy (#0f172a) rounded square with a
# white four-point sparkle with curved concave edges (quadratic -> cubic).
# Pure GDI+ drawing, no external deps. Multi-size PNG-packed ICO (Vista+).
param(
  [string]$PreviewPath = ''
)

Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'

$sizes = @(16, 24, 32, 48, 64, 128, 256)
$outDir = Join-Path $PSScriptRoot '..\src\AiCompanion.App\Assets'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$top = [System.Drawing.Color]::FromArgb(255, 30, 41, 59)      # #1e293b
$bottom = [System.Drawing.Color]::FromArgb(255, 15, 23, 42)   # #0f172a

$pngs = New-Object System.Collections.ArrayList

foreach ($s in $sizes) {
  $k = $s / 512.0   # SVG is authored at 512x512

  $bmp = New-Object System.Drawing.Bitmap($s, $s, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
  $g.Clear([System.Drawing.Color]::Transparent)

  # rounded square background: inset 32, corner radius 104 (SVG geometry)
  $inset = [float](32 * $k)
  $rad = [float](104 * $k)
  $d = 2 * $rad
  $path = New-Object System.Drawing.Drawing2D.GraphicsPath
  $path.AddArc($inset, $inset, $d, $d, 180, 90)
  $path.AddArc($s - $inset - $d, $inset, $d, $d, 270, 90)
  $path.AddArc($s - $inset - $d, $s - $inset - $d, $d, $d, 0, 90)
  $path.AddArc($inset, $s - $inset - $d, $d, $d, 90, 90)
  $path.CloseFigure()

  $rect = New-Object System.Drawing.Rectangle(0, 0, $s, $s)
  $bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, $top, $bottom, 90.0)
  $g.FillPath($bg, $path)

  # four-point sparkle with curved edges: 4 quadratic segments from the SVG,
  # converted to cubic Beziers. SVG points (512-space):
  #   M 256 106 Q 291 221 406 256 Q 291 291 256 406 Q 221 291 106 256 Q 221 221 256 106 Z
  $sp = New-Object System.Drawing.Drawing2D.GraphicsPath
  $q = @(
    @(256, 106, 291, 221, 406, 256),
    @(406, 256, 291, 291, 256, 406),
    @(256, 406, 221, 291, 106, 256),
    @(106, 256, 221, 221, 256, 106)
  )
  foreach ($seg in $q) {
    $p0x = [float]($seg[0] * $k); $p0y = [float]($seg[1] * $k)
    $cx  = [float]($seg[2] * $k); $cy  = [float]($seg[3] * $k)
    $p2x = [float]($seg[4] * $k); $p2y = [float]($seg[5] * $k)
    # quadratic -> cubic: C1 = P0 + 2/3(C - P0), C2 = P2 + 2/3(C - P2)
    $c1x = $p0x + 2.0 / 3.0 * ($cx - $p0x)
    $c1y = $p0y + 2.0 / 3.0 * ($cy - $p0y)
    $c2x = $p2x + 2.0 / 3.0 * ($cx - $p2x)
    $c2y = $p2y + 2.0 / 3.0 * ($cy - $p2y)
    $sp.AddBezier($p0x, $p0y, $c1x, $c1y, $c2x, $c2y, $p2x, $p2y)
  }
  $sp.CloseFigure()

  $wb = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)
  $g.FillPath($wb, $sp)

  if ($PreviewPath -and $s -eq 256) {
    $bmp.Save($PreviewPath, [System.Drawing.Imaging.ImageFormat]::Png)
  }

  $g.Dispose()
  $null = $pngs.Add($bmp)
}

# pack PNGs into an ICO container (256 encoded as 0 per ICO spec)
$icoPath = Join-Path $outDir 'Orua.ico'
$fs = [System.IO.File]::Create($icoPath)
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([uint16]0)
$bw.Write([uint16]1)
$bw.Write([uint16]$pngs.Count)

$offset = 6 + 16 * $pngs.Count
$blobs = @()
foreach ($bmp in $pngs) {
  $s = $bmp.Width
  $ms = New-Object System.IO.MemoryStream
  $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
  $bytes = $ms.ToArray()
  $ms.Dispose()
  $blobs += ,$bytes
  $w = if ($s -eq 256) { 0 } else { $s }
  $bw.Write([byte]$w)
  $bw.Write([byte]$w)
  $bw.Write([byte]0)
  $bw.Write([byte]0)
  $bw.Write([uint16]1)
  $bw.Write([uint16]32)
  $bw.Write([uint32]$bytes.Length)
  $bw.Write([uint32]$offset)
  $offset += $bytes.Length
}
foreach ($bytes in $blobs) { $bw.Write($bytes) }
$bw.Close()

foreach ($bmp in $pngs) { $bmp.Dispose() }

Write-Output "OK: $icoPath ($((Get-Item $icoPath).Length) bytes, $($pngs.Count) sizes)"
