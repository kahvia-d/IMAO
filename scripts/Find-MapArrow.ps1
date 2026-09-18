[CmdletBinding()]
param(
    # Comma-separated, because -File invocation cannot bind an array. Paths contain no commas.
    [Parameter(Mandatory = $true)][string]$Image,
    # Half-size of the crop written around the detected arrow.
    [int]$CropRadius = 256,
    [string]$OutputDirectory,
    [string]$Prefix = 'arrow'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

# The big-map player marker is a saturated gold triangle. Detection is deliberately
# narrow: the map's own collectible icons are paler and mostly translucent, so a
# tight red/green with almost no blue keeps them out.
function Find-ArrowCenter([string]$Path) {
    $source = [System.Drawing.Bitmap]::FromFile($Path)
    try {
        $rect = [System.Drawing.Rectangle]::new(0, 0, $source.Width, $source.Height)
        $data = $source.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        try {
            $stride = $data.Stride
            $bytes = [byte[]]::new($stride * $source.Height)
            [Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
        }
        finally { $source.UnlockBits($data) }

        # Coarse grid of hit counts; the densest cell seeds the cluster centroid.
        $cell = 16
        $cells = @{}
        for ($y = 0; $y -lt $source.Height; $y++) {
            $row = $y * $stride
            for ($x = 0; $x -lt $source.Width; $x++) {
                $i = $row + $x * 4
                $b = $bytes[$i]; $g = $bytes[$i + 1]; $r = $bytes[$i + 2]
                if ($r -ge 190 -and $g -ge 150 -and $b -le 90 -and ($r - $b) -ge 110) {
                    $key = "$([int]($x / $cell)),$([int]($y / $cell))"
                    if ($cells.ContainsKey($key)) { $cells[$key]++ } else { $cells[$key] = 1 }
                }
            }
        }
        if ($cells.Count -eq 0) { return $null }
        $best = $cells.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1
        $bx = [int]($best.Key -split ',')[0] * $cell
        $by = [int]($best.Key -split ',')[1] * $cell

        # Refine: centroid of matching pixels inside a generous window around that cell.
        $sumX = 0.0; $sumY = 0.0; $count = 0
        for ($y = [Math]::Max(0, $by - 96); $y -le [Math]::Min($source.Height - 1, $by + 96); $y++) {
            $row = $y * $stride
            for ($x = [Math]::Max(0, $bx - 96); $x -le [Math]::Min($source.Width - 1, $bx + 96); $x++) {
                $i = $row + $x * 4
                $b = $bytes[$i]; $g = $bytes[$i + 1]; $r = $bytes[$i + 2]
                if ($r -ge 190 -and $g -ge 150 -and $b -le 90 -and ($r - $b) -ge 110) { $sumX += $x; $sumY += $y; ++$count }
            }
        }
        if ($count -eq 0) { return $null }
        return [pscustomobject]@{ X = $sumX / $count; Y = $sumY / $count; Hits = $count
            Width = $source.Width; Height = $source.Height; TotalCells = $cells.Count }
    }
    finally { $source.Dispose() }
}

if (-not $OutputDirectory) { $OutputDirectory = Join-Path $env:TEMP 'arrow-crops' }
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null

$images = @($Image -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
if ($images.Count -eq 0) { throw 'Image is empty.' }

$index = 0
foreach ($path in $images) {
    ++$index
    if (-not (Test-Path -LiteralPath $path)) { Write-Warning "missing: $path"; continue }
    $found = Find-ArrowCenter $path
    if ($null -eq $found) { Write-Host "  [$index] $(Split-Path $path -Leaf): no arrow pixels found"; continue }
    Write-Host ("  [{0}] {1}: arrow at screen ({2:N1}, {3:N1}) hits={4} of {5}x{6}" -f `
        $index, (Split-Path $path -Leaf), $found.X, $found.Y, $found.Hits, $found.Width, $found.Height)

    $size = $CropRadius * 2
    $left = [int][Math]::Round($found.X) - $CropRadius
    $top = [int][Math]::Round($found.Y) - $CropRadius
    $left = [Math]::Max(0, [Math]::Min($left, $found.Width - $size))
    $top = [Math]::Max(0, [Math]::Min($top, $found.Height - $size))
    # Named sourceImage: $Image is the [string] parameter and PowerShell variable names are case-insensitive.
    $sourceImage = [System.Drawing.Image]::FromFile($path)
    try {
        $crop = [System.Drawing.Bitmap]::new($size, $size)
        $graphics = [System.Drawing.Graphics]::FromImage($crop)
        try {
            $graphics.DrawImage($sourceImage, [System.Drawing.Rectangle]::new(0, 0, $size, $size),
                [System.Drawing.Rectangle]::new($left, $top, $size, $size), [System.Drawing.GraphicsUnit]::Pixel)
        }
        finally { $graphics.Dispose() }
        $out = Join-Path $OutputDirectory ("{0}-{1:d2}.png" -f $Prefix, $index)
        $crop.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
        $crop.Dispose()
        # The arrow's offset inside the crop; the matcher localizes the crop centre.
        Write-Host ("        crop {0} -> {1}  arrow offset in crop = ({2:N1}, {3:N1})" -f `
            "$left,$top", (Split-Path $out -Leaf), ($found.X - $left), ($found.Y - $top))
    }
    finally { $sourceImage.Dispose() }
}
