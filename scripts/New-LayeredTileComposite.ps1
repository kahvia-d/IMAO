# Builds "layered view" tiles for a region by compositing its layered-map overlays onto
# the surface tiles, so the feature pack matches what the game draws inside a layer.
#
# Evidence for the model (Docs/LayeredMapFeatureMeasurement_20260922.md, sections 7-8):
#   * upstream layered tiles are RGBA overlays (93.7-100% fully transparent);
#   * the in-game layered view is the surface tile dimmed to 0.197x with the current
#     layer on top;
#   * the corner minimap inside a layer draws the current layer over the surface tile.
# So the composited tile is:  surface * k  then alpha-over each overlay of that tile.
#
# Tiles of the region that carry no overlay are hard-linked from the surface archive, so
# the output directory is a drop-in tile-archive layout and costs almost no disk.
#
#   pwsh -File scripts\New-LayeredTileComposite.ps1 -RegionId jinzhou

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RegionId,
    [string]$SourceRoot,
    [string]$RegistryPath,
    [string]$TileArchiveRoot,
    [string]$LayerArchiveRoot,
    [string]$OutputRoot,
    [string]$ResourceVersion,
    # Surface brightness under the overlays. Both are kept because the in-game test in
    # scripts/Test-LayeredTileComposite.ps1 prefers 0.35 (30 near-anchor matches on a real
    # minimap captured inside 眠龙庭, against 13 for 1.0), while 1.0 is the plain
    # surface+overlay composite. Never below ~0.2: that is the measured layered large-map
    # brightness and it costs keypoints without helping.
    [double[]]$BaseFactor = @(1.0, 0.35)
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

if (-not $SourceRoot) { $SourceRoot = Split-Path -Parent $PSScriptRoot }
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
if (-not $RegistryPath) { $RegistryPath = Join-Path $SourceRoot 'map-regions/regions.json' }
if (-not $TileArchiveRoot) { $TileArchiveRoot = Join-Path $SourceRoot 'map-regions/tiles' }
if (-not $LayerArchiveRoot) { $LayerArchiveRoot = Join-Path $SourceRoot 'map-regions/layers' }
if (-not $OutputRoot) { $OutputRoot = Join-Path $SourceRoot 'out/map-regions/composite' }

$registry = Get-Content -LiteralPath $RegistryPath -Raw -Encoding UTF8 | ConvertFrom-Json
$region = @($registry.regions | Where-Object { $_.id -eq $RegionId })
if ($region.Count -ne 1) { throw "Region '$RegionId' is not in the registry exactly once." }
$region = $region[0]

$tileManifestPath = Join-Path $TileArchiveRoot 'tiles.manifest.json'
if (-not (Test-Path -LiteralPath $tileManifestPath)) { throw "Tile archive manifest is missing: $tileManifestPath" }
$tileManifest = Get-Content -LiteralPath $tileManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
$tileVersion = if ($ResourceVersion) { $ResourceVersion.ToUpperInvariant() } else { [string]$tileManifest.tileResourceVersion }
if ($tileVersion -notmatch '^[A-Fa-f0-9]{32}$') { throw "Tile generation is invalid: $tileVersion" }

$layerManifestPath = Join-Path $LayerArchiveRoot 'layers.manifest.json'
if (-not (Test-Path -LiteralPath $layerManifestPath)) {
    throw "Layer archive manifest is missing: $layerManifestPath (run scripts/Get-MapLayerArchive.ps1 first)."
}
$layerManifest = Get-Content -LiteralPath $layerManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
if ([string]$layerManifest.layerResourceVersion -ne $tileVersion) {
    throw "Layer archive generation $($layerManifest.layerResourceVersion) does not match tile generation $tileVersion."
}

$state = [int]$region.frame
$stateNode = $layerManifest.states.PSObject.Properties[[string]$state]
if ($null -eq $stateNode -or -not $stateNode.Value.hasLayers) {
    throw "Frame $state has no layered maps; nothing to composite for $RegionId."
}
$layers = @($stateNode.Value.layers)
$surfaceRecords = @($tileManifest.regions.$RegionId.tiles | Where-Object { -not $_.absent })
if ($surfaceRecords.Count -eq 0) { throw "No present surface tiles recorded for $RegionId." }

# A layer belongs to this region when its entrance marker (item FCRK) is nearest to one
# of the region's own anchors - the same attribution the region registry uses.
$pointFile = Join-Path $SourceRoot "Assets/KuroMap/states/state-$state.json"
$points = Get-Content -LiteralPath $pointFile -Raw -Encoding UTF8 | ConvertFrom-Json
$country = Get-Content -LiteralPath (Join-Path $SourceRoot 'Assets/KuroMap/country.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$anchors = New-Object System.Collections.ArrayList
function Add-Anchors($node) {
    if ($node.level -ge 2) {
        [void]$anchors.Add([pscustomobject]@{ countryId = $node.countryId; mapState = [string]$node.mapState
            x = [double]$node.xPosition; y = [double]$node.yPosition })
    }
    $children = if ($null -ne $node.PSObject.Properties['children']) { @($node.children) } else { @() }
    foreach ($child in $children) { Add-Anchors $child }
}
foreach ($entry in $country) { foreach ($node in $entry.countrys) { Add-Anchors $node } }

$ownedLayerIds = New-Object System.Collections.Generic.HashSet[string]
foreach ($item in $points) {
    foreach ($location in $item.location) {
        if ($item.id -ne 'FCRK') { continue }
        $best = $null; $bestDistance = [double]::MaxValue
        foreach ($anchor in $anchors | Where-Object { $_.countryId -eq $location.countryId }) {
            $distance = [Math]::Sqrt([Math]::Pow($anchor.x - $location.x, 2) + [Math]::Pow($anchor.y - $location.y, 2))
            if ($distance -lt $bestDistance) { $bestDistance = $distance; $best = $anchor }
        }
        if ($null -ne $best -and $best.mapState -eq [string]$region.mapState) { [void]$ownedLayerIds.Add([string]$location.floorId) }
    }
}
$regionLayers = @($layers | Where-Object { $ownedLayerIds.Contains([string]$_.id) })
if ($regionLayers.Count -eq 0) { throw "No layer in frame $state attributes to region $RegionId." }
Write-Host ("Region {0} (frame {1}) owns {2} layered maps: {3}" -f $RegionId, $state, $regionLayers.Count,
    (($regionLayers | ForEach-Object { "$($_.name)[$($_.id)]" }) -join ', '))

# coordinate -> ordered overlay images belonging to this region
$overlaysByTile = @{}
foreach ($layer in $regionLayers) {
    foreach ($floor in $layer.floors) {
        foreach ($image in @($floor.tiles)) {
            $parts = (($image -split '/')[-1] -replace '\.png$', '').Split('_')
            $key = "$([int]$parts[0]),$([int]$parts[1])"
            if (-not $overlaysByTile.ContainsKey($key)) { $overlaysByTile[$key] = New-Object System.Collections.ArrayList }
            [void]$overlaysByTile[$key].Add([pscustomobject]@{ layerId = [string]$layer.id; layerName = $layer.name
                floorId = [string]$floor.id; image = [string]$image })
        }
    }
}

function Get-OpaqueStats([string]$path) {
    $bmp = [System.Drawing.Bitmap]::FromFile($path)
    $total = 0; $opaque = 0
    for ($y = 0; $y -lt $bmp.Height; $y += 4) {
        for ($x = 0; $x -lt $bmp.Width; $x += 4) { $total++; if ($bmp.GetPixel($x, $y).A -gt 8) { $opaque++ } }
    }
    $bmp.Dispose()
    return [Math]::Round(100.0 * $opaque / $total, 2)
}

function New-CompositeTile([string]$surfacePath, [object[]]$overlays, [string]$outPath, [double]$factor) {
    $canvas = New-Object System.Drawing.Bitmap 1024, 1024, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($canvas)
    $graphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceOver
    $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
    $graphics.Clear([System.Drawing.Color]::Black)
    $surface = [System.Drawing.Image]::FromFile($surfacePath)
    $attributes = New-Object System.Drawing.Imaging.ImageAttributes
    $matrix = New-Object System.Drawing.Imaging.ColorMatrix
    $matrix.Matrix00 = $factor; $matrix.Matrix11 = $factor; $matrix.Matrix22 = $factor; $matrix.Matrix33 = 1.0
    $attributes.SetColorMatrix($matrix)
    $rect = New-Object System.Drawing.Rectangle 0, 0, 1024, 1024
    $graphics.DrawImage($surface, $rect, 0, 0, 1024, 1024, [System.Drawing.GraphicsUnit]::Pixel, $attributes)
    $surface.Dispose(); $attributes.Dispose()
    foreach ($overlay in $overlays) {
        $image = [System.Drawing.Image]::FromFile($overlay.path)
        $graphics.DrawImage($image, $rect)
        $image.Dispose()
    }
    $graphics.Dispose()
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $outPath) | Out-Null
    # Never open the destination in place: an earlier run may have left a hard link
    # there, and GDI+ would then write through it into the archived source tile.
    $tempPath = "$outPath.tmp"
    $canvas.Save($tempPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $canvas.Dispose()
    if (Test-Path -LiteralPath $outPath) { Remove-Item -LiteralPath $outPath -Force }
    Move-Item -LiteralPath $tempPath -Destination $outPath -Force
}

$factorList = @($BaseFactor | Sort-Object -Descending)
$summary = New-Object System.Collections.ArrayList

foreach ($factor in $factorList) {
    $tag = 'k{0:D3}' -f [int][Math]::Round($factor * 100)
    $outDir = Join-Path $OutputRoot "$RegionId/$tag"
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $composited = 0; $linked = 0
    foreach ($record in $surfaceRecords) {
        $leaf = Split-Path -Leaf $record.file                     # 8_-3_1.png
        $surfacePath = Join-Path $TileArchiveRoot "$tileVersion/$state/$leaf"
        if (-not (Test-Path -LiteralPath $surfacePath)) { throw "Surface tile is missing: $surfacePath" }
        $outPath = Join-Path $outDir $leaf
        # `-replace` binds tighter than `+`, so the prefix pattern must be built first.
        $prefix = '^' + $state + '_'
        $key = ($leaf -replace $prefix, '') -replace '\.png$', ''
        $key = $key -replace '_', ','
        if ($overlaysByTile.ContainsKey($key)) {
            $resolved = foreach ($overlay in $overlaysByTile[$key]) {
                $relative = "$state$($overlay.image)"
                $path = Join-Path $LayerArchiveRoot "$tileVersion/$relative"
                if (-not (Test-Path -LiteralPath $path)) { throw "Layer tile is missing: $path" }
                [pscustomobject]@{ path = $path; layerId = $overlay.layerId; layerName = $overlay.layerName
                    floorId = $overlay.floorId; opaquePct = (Get-OpaqueStats $path) }
            }
            New-CompositeTile $surfacePath @($resolved) $outPath $factor
            $composited++
            $overlapNote = ''
            if (@($resolved).Count -gt 1) { $overlapNote = " overlays=$(@($resolved).Count)" }
            [void]$summary.Add([pscustomobject]@{ factor = $factor; tile = $key; overlays = @($resolved).Count
                opaquePct = (($resolved | ForEach-Object { $_.opaquePct }) -join '/')
                layers = (($resolved | ForEach-Object { "$($_.layerName)/$($_.floorId)" }) -join ' + ') })
        }
        else {
            if (Test-Path -LiteralPath $outPath) { Remove-Item -LiteralPath $outPath -Force }
            try { New-Item -ItemType HardLink -Path $outPath -Target $surfacePath -ErrorAction Stop | Out-Null }
            catch { Copy-Item -LiteralPath $surfacePath -Destination $outPath -Force }
            $linked++
        }
    }
    Write-Host ("  {0}: composited {1} tiles, linked {2} surface tiles -> {3}" -f $tag, $composited, $linked, $outDir)
}

Write-Host ''
$summary | Sort-Object tile, factor | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
