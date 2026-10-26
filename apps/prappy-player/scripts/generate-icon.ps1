#Requires -Version 7.0
# Original gatefold PP monogram, rasterized with Windows' built-in drawing APIs.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$assets = Join-Path $PSScriptRoot '..\assets'
@'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">
  <rect x="2" y="2" width="60" height="60" rx="12" fill="#252e32"/>
  <rect x="2" y="2" width="60" height="60" rx="12" fill="none" stroke="#536766" stroke-width="1"/>
  <path fill="#f5bd63" fill-rule="evenodd" d="M23 9H41L54 22V32L41 45H35V51H23ZM35 20V33H37L43 27V26L37 20Z"/>
  <path fill="#40f0b8" stroke="#252e32" stroke-width="3" stroke-linejoin="round" d="M11 17H31L44 30V36L31 49H23V55H11Z"/>
  <path fill="#252e32" d="M23 29H28L32 33V34L28 38H23Z"/>
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
    function Points([int[]]$coords) {
        $result = for($i=0;$i -lt $coords.Length;$i+=2) { [Drawing.PointF]::new($coords[$i],$coords[$i+1]) }
        return ,([Drawing.PointF[]]$result)
    }
    $back = [Drawing.Drawing2D.GraphicsPath]::new([Drawing.Drawing2D.FillMode]::Alternate)
    $back.AddPolygon((Points @(23,9,41,9,54,22,54,32,41,45,35,45,35,51,23,51)))
    $back.AddPolygon((Points @(35,20,35,33,37,33,43,27,43,26,37,20)))
    $gold = [Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml('#f5bd63'))
    $g.FillPath($gold,$back)
    $front = [Drawing.Drawing2D.GraphicsPath]::new()
    $front.AddPolygon((Points @(11,17,31,17,44,30,44,36,31,49,23,49,23,55,11,55)))
    $mint = [Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml('#40f0b8'))
    $seam = [Drawing.Pen]::new([Drawing.ColorTranslator]::FromHtml('#252e32'),3)
    $seam.LineJoin = [Drawing.Drawing2D.LineJoin]::Round
    $g.FillPath($mint,$front); $g.DrawPath($seam,$front)
    $g.FillPolygon($background,(Points @(23,29,28,29,32,33,32,34,28,38,23,38)))
    $bitmap = [Drawing.Bitmap]::new($size,$size)
    $small = [Drawing.Graphics]::FromImage($bitmap)
    $small.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $small.DrawImage($large,0,0,$size,$size)
    $stream = [IO.MemoryStream]::new()
    $bitmap.Save($stream,[Drawing.Imaging.ImageFormat]::Png)
    $images += ,$stream.ToArray()
    $build = Join-Path $PSScriptRoot '..\build'
    if (Test-Path -LiteralPath $build) { $bitmap.Save((Join-Path $build "player-icon-$size.png"),[Drawing.Imaging.ImageFormat]::Png) }
    foreach ($item in @($stream,$small,$bitmap,$seam,$mint,$front,$gold,$back,$outline,$background,$shape,$g,$large)) { $item.Dispose() }
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
