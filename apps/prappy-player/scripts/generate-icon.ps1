#Requires -Version 7.0
# Original slim, staggered PP monogram, rasterized with Windows' drawing APIs.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$assets = Join-Path $PSScriptRoot '..\assets'
@'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">
  <rect x="2" y="2" width="60" height="60" rx="12" fill="#252e32"/>
  <rect x="2" y="2" width="60" height="60" rx="12" fill="none" stroke="#536766" stroke-width="1"/>
  <g fill="none" stroke-width="4.5" stroke-linejoin="round" stroke-linecap="square">
    <path stroke="#f5bd63" d="M36 43V14H43C54 14 54 31 43 31H36"/>
    <path stroke="#40f0b8" d="M15 51V23H23C34 23 34 40 23 40H15"/>
  </g>
</svg>
'@ | Set-Content "$assets\player.svg" -Encoding utf8
$images = @()
foreach ($size in @(16,24,32,48,64,128,256)) {
    $large = [Drawing.Bitmap]::new($size*4,$size*4)
    $g = [Drawing.Graphics]::FromImage($large)
    $g.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.ScaleTransform($size*4/64.0,$size*4/64.0)
    $shape = [Drawing.Drawing2D.GraphicsPath]::new()
    $shape.AddArc(2,2,24,24,180,90); $shape.AddArc(38,2,24,24,270,90)
    $shape.AddArc(38,38,24,24,0,90); $shape.AddArc(2,38,24,24,90,90); $shape.CloseFigure()
    $background = [Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml('#252e32'))
    $outline = [Drawing.Pen]::new([Drawing.ColorTranslator]::FromHtml('#536766'),1)
    $g.FillPath($background,$shape); $g.DrawPath($outline,$shape)
    $back = [Drawing.Drawing2D.GraphicsPath]::new()
    $back.AddLine(36,43,36,14); $back.AddLine(36,14,43,14)
    $back.AddBezier(43,14,54,14,54,31,43,31); $back.AddLine(43,31,36,31)
    $gold = [Drawing.Pen]::new([Drawing.ColorTranslator]::FromHtml('#f5bd63'),4.5)
    $gold.LineJoin = [Drawing.Drawing2D.LineJoin]::Round
    $gold.StartCap = $gold.EndCap = [Drawing.Drawing2D.LineCap]::Square
    $g.DrawPath($gold,$back)
    $front = [Drawing.Drawing2D.GraphicsPath]::new()
    $front.AddLine(15,51,15,23); $front.AddLine(15,23,23,23)
    $front.AddBezier(23,23,34,23,34,40,23,40); $front.AddLine(23,40,15,40)
    $mint = [Drawing.Pen]::new([Drawing.ColorTranslator]::FromHtml('#40f0b8'),4.5)
    $mint.LineJoin = [Drawing.Drawing2D.LineJoin]::Round
    $mint.StartCap = $mint.EndCap = [Drawing.Drawing2D.LineCap]::Square
    $g.DrawPath($mint,$front)
    $bitmap = [Drawing.Bitmap]::new($size,$size)
    $small = [Drawing.Graphics]::FromImage($bitmap)
    $small.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $small.DrawImage($large,0,0,$size,$size)
    $stream = [IO.MemoryStream]::new()
    $bitmap.Save($stream,[Drawing.Imaging.ImageFormat]::Png)
    $images += ,$stream.ToArray()
    $build = Join-Path $PSScriptRoot '..\build'
    if (Test-Path -LiteralPath $build) { $bitmap.Save((Join-Path $build "player-icon-$size.png"),[Drawing.Imaging.ImageFormat]::Png) }
    foreach ($item in @($stream,$small,$bitmap,$mint,$front,$gold,$back,$outline,$background,$shape,$g,$large)) { $item.Dispose() }
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
