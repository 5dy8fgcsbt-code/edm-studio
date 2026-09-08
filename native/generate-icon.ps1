# Rasterize the original simple SVG with Windows GDI+; no external tools or fonts.
# The checked-in ICO is used directly by the normal build.
param([string]$AssetDirectory = (Join-Path $PSScriptRoot 'assets'))
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
[xml]$art = Get-Content -LiteralPath (Join-Path $AssetDirectory 'edm-studio.svg') -Raw
$sizes = @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
$frames = [Collections.Generic.List[byte[]]]::new()
foreach ($size in $sizes) {
    $canvas = [Drawing.Bitmap]::new($size * 4, $size * 4, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [Drawing.Graphics]::FromImage($canvas)
    $bitmap = [Drawing.Bitmap]::new($size, $size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $output = [Drawing.Graphics]::FromImage($bitmap)
    $memory = [IO.MemoryStream]::new()
    try {
        $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $graphics.ScaleTransform(($size * 4 / 256.0), ($size * 4 / 256.0))
        foreach ($shape in $art.DocumentElement.ChildNodes) {
            if ($shape.LocalName -eq 'title') { continue }
            $path = [Drawing.Drawing2D.GraphicsPath]::new()
            $brush = [Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml($shape.fill))
            try {
                switch ($shape.LocalName) {
                    'rect' {
                        $x = [float]$shape.x; $y = [float]$shape.y
                        $w = [float]$shape.width; $h = [float]$shape.height
                        $d = 2 * [float]$shape.rx
                        $path.AddArc($x, $y, $d, $d, 180, 90)
                        $path.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
                        $path.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
                        $path.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
                        $path.CloseFigure()
                    }
                    'polygon' {
                        [Drawing.PointF[]]$points = @($shape.points.Trim() -split '\s+' | ForEach-Object {
                            $pair = $_ -split ','
                            [Drawing.PointF]::new([float]$pair[0], [float]$pair[1])
                        })
                        $path.AddPolygon($points)
                    }
                    default { throw "Unsupported icon SVG element: $($shape.LocalName)" }
                }
                $graphics.FillPath($brush, $path)
            } finally { $brush.Dispose(); $path.Dispose() }
        }
        $output.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $output.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $output.DrawImage($canvas, [Drawing.Rectangle]::new(0, 0, $size, $size))
        $bitmap.Save($memory, [Drawing.Imaging.ImageFormat]::Png)
        $frames.Add($memory.ToArray())
        if ($size -eq 256) { $bitmap.Save((Join-Path $AssetDirectory 'edm-studio.png'), [Drawing.Imaging.ImageFormat]::Png) }
    } finally {
        $memory.Dispose(); $output.Dispose(); $bitmap.Dispose(); $graphics.Dispose(); $canvas.Dispose()
    }
}
$file = [IO.File]::Create((Join-Path $AssetDirectory 'edm-studio.ico'))
$writer = [IO.BinaryWriter]::new($file)
try {
    $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$sizes.Count)
    $offset = 6 + 16 * $sizes.Count
    for ($index = 0; $index -lt $sizes.Count; ++$index) {
        $writer.Write([byte]($sizes[$index] % 256)); $writer.Write([byte]($sizes[$index] % 256))
        $writer.Write([byte]0); $writer.Write([byte]0)
        $writer.Write([uint16]1); $writer.Write([uint16]32)
        $writer.Write([uint32]$frames[$index].Length); $writer.Write([uint32]$offset)
        $offset += $frames[$index].Length
    }
    foreach ($frame in $frames) { $writer.Write($frame) }
} finally { $writer.Dispose(); $file.Dispose() }
Write-Host "Generated original EDM Studio icon: $($sizes -join ', ') px."
