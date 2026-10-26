#Requires -Version 7.0
# Vector-style player mark, rasterized with Windows' built-in drawing APIs.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$assets = Join-Path $PSScriptRoot '..\assets'
@'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">
  <rect x="2" y="2" width="60" height="60" rx="14" fill="#252e32"/>
  <rect x="2" y="2" width="60" height="60" rx="14" fill="none" stroke="#536766" stroke-width="1.5"/>
  <path d="M11 24v16M19 16v32M27 23v18" stroke="#83d4be" stroke-width="4" stroke-linecap="round"/>
  <path d="M35 17L55 32L35 47Z" fill="#f5bd63"/>
</svg>
'@ | Set-Content "$assets\player.svg" -Encoding utf8
$images = @()
foreach ($size in @(16,24,32,48,64,128,256)) {
    $large = [Drawing.Bitmap]::new($size*4,$size*4)
    $g = [Drawing.Graphics]::FromImage($large)
    $g.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.ScaleTransform($size*4/64.0,$size*4/64.0)
    $shape = [Drawing.Drawing2D.GraphicsPath]::new()
    $shape.AddArc(2,2,28,28,180,90); $shape.AddArc(34,2,28,28,270,90)
    $shape.AddArc(34,34,28,28,0,90); $shape.AddArc(2,34,28,28,90,90); $shape.CloseFigure()
    $background = [Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml('#252e32'))
    $outline = [Drawing.Pen]::new([Drawing.ColorTranslator]::FromHtml('#536766'),1.5)
    $g.FillPath($background,$shape); $g.DrawPath($outline,$shape)
    $bars = [Drawing.Pen]::new([Drawing.ColorTranslator]::FromHtml('#83d4be'),4)
    $bars.StartCap = $bars.EndCap = [Drawing.Drawing2D.LineCap]::Round
    $g.DrawLine($bars,11,24,11,40); $g.DrawLine($bars,19,16,19,48); $g.DrawLine($bars,27,23,27,41)
    $play = [Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml('#f5bd63'))
    $g.FillPolygon($play,[Drawing.PointF[]]@([Drawing.PointF]::new(35,17),[Drawing.PointF]::new(55,32),[Drawing.PointF]::new(35,47)))
    $bitmap = [Drawing.Bitmap]::new($size,$size)
    $small = [Drawing.Graphics]::FromImage($bitmap)
    $small.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $small.DrawImage($large,0,0,$size,$size)
    $stream = [IO.MemoryStream]::new()
    $bitmap.Save($stream,[Drawing.Imaging.ImageFormat]::Png)
    $images += ,$stream.ToArray()
    foreach ($item in @($stream,$small,$bitmap,$play,$bars,$outline,$background,$shape,$g,$large)) { $item.Dispose() }
}
$output = [IO.File]::Create((Join-Path $assets 'player.ico'))
$writer = [IO.BinaryWriter]::new($output)
try {
    $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$images.Count)
    $offset = 6 + 16*$images.Count
    $sizes = @(16,24,32,48,64,128,256)
    for ($i=0; $i -lt $images.Count; $i++) {
        $dimension = if ($sizes[$i] -eq 256) { 0 } else { $sizes[$i] }
        $writer.Write([byte]$dimension); $writer.Write([byte]$dimension)
        $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]32)
        $writer.Write([uint32]$images[$i].Length); $writer.Write([uint32]$offset)
        $offset += $images[$i].Length
    }
    foreach ($bytes in $images) { $writer.Write([byte[]]$bytes) }
} finally { $writer.Dispose() }
Write-Host 'Generated player.svg and multi-resolution player.ico (16–256px).'
