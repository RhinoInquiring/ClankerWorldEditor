# Renders the README's pictures with the editor's own command-line renderer (no window) into docs/images.
# Run from the repository root after a Release build:
#   powershell -ExecutionPolicy Bypass -File tools\readme-images.ps1 -Data "<client>\Data"
# Pictures of the editor's panels are taken by hand: see docs/images/README.md.
param(
    [Parameter(Mandatory = $true)][string]$Data,   # the client's Data folder
    [string]$Exe = "build\Release\wow-world-editor.exe",
    [string]$Out = "docs\images"
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force $Out | Out-Null
$raw = Join-Path $env:TEMP "wwe-readme-images"
New-Item -ItemType Directory -Force $raw | Out-Null

# Hardware device, full terrain detail near the camera (the CLI does not wait for the far bakes).
$env:WWE_HW = "1"; $env:WWE_NO_LOD = "1"

function Render($name, $map, $x, $y, $yaw, $pitch, $above, $fx, $fz, $radius = 2) {
    $env:WWE_RADIUS = "$radius"
    & $Exe --render $Data $map $x $y (Join-Path $raw "$name.png") $yaw $pitch $above $fx $fz | Out-Null
}

# Scale to $width (keeping the aspect) and save as JPEG; $crop = trim flat background to the content first.
function Save($name, $file, $width, [switch]$crop, [switch]$png) {
    $src = [System.Drawing.Bitmap]::FromFile((Join-Path $raw "$name.png"))
    $rect = New-Object System.Drawing.Rectangle 0, 0, $src.Width, $src.Height
    if ($crop) {
        $bg = $src.GetPixel(0, 0); $l = $src.Width; $t = $src.Height; $r = 0; $b = 0
        for ($y = 0; $y -lt $src.Height; $y += 2) { for ($x = 0; $x -lt $src.Width; $x += 2) {
            if ($src.GetPixel($x, $y) -ne $bg) { $l = [Math]::Min($l, $x); $r = [Math]::Max($r, $x); $t = [Math]::Min($t, $y); $b = [Math]::Max($b, $y) } } }
        $rect = New-Object System.Drawing.Rectangle ([Math]::Max(0, $l - 8)), ([Math]::Max(0, $t - 8)), ([Math]::Min($src.Width, $r + 9) - [Math]::Max(0, $l - 8)), ([Math]::Min($src.Height, $b + 9) - [Math]::Max(0, $t - 8))
    }
    $w = [Math]::Min($width, $rect.Width); $h = [int]($rect.Height * $w / $rect.Width)
    $dst = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($dst)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.DrawImage($src, (New-Object System.Drawing.Rectangle 0, 0, $w, $h), $rect, [System.Drawing.GraphicsUnit]::Pixel)
    $path = Join-Path (Resolve-Path $Out) $file
    if ($png) { $dst.Save($path, [System.Drawing.Imaging.ImageFormat]::Png) }
    else {
        $codec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq "image/jpeg" }
        $params = New-Object System.Drawing.Imaging.EncoderParameters 1
        $params.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter ([System.Drawing.Imaging.Encoder]::Quality), 85L
        $dst.Save($path, $codec, $params)
    }
    $g.Dispose(); $dst.Dispose(); $src.Dispose()
    Write-Host "$file  $w x $h"
}

Render "stormwind" Azeroth 30 48 200 -24 230 1.15 1.55          # Stormwind from the hills to the south
Save "stormwind" "stormwind.jpg" 1280
Render "city" Azeroth 30 48 160 -32 70 0.85 0.55                # inside the city: WMOs, doodads, trees
Save "city" "city.jpg" 960
& $Exe --map-preview $Data Azeroth (Join-Path $raw "map.png") | Out-Null
Save "map" "map-picture.png" 420 -crop -png
& $Exe --catalog-check $Data (Join-Path $raw "catalog.png") | Out-Null
Save "catalog" "catalog.png" 512 -png
& $Exe --skin-check $Data (Join-Path $raw "skins.png") 1985 4259 4997 | Out-Null   # Marshal Dughan, an orc grunt, a dwarf
Save "skins" "skins.jpg" 512
Remove-Item -Recurse -Force $raw
