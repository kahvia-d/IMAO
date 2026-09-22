# Builds "layered view" tiles for a region by compositing each layered-map overlay onto
# the surface tile it sits on, ONE IMAGE PER FLOOR.
#
# Evidence for the model (Docs/LayeredMapFeatureMeasurement_20260922.md sections 7-8,
# Docs/LayeredMapFeaturePackPlan.md section 0):
#   * upstream layered tiles are RGBA overlays (93.7-100% fully transparent);
#   * the in-game layered view is the surface tile dimmed to 0.197x with the current
#     layer on top, and the corner minimap draws the current layer over the surface;
#   * several floors share a tile coordinate (叩天关 上/中/下 all sit on (3,-3)), and
#     stacking them into one image lets whichever is drawn last hide the others: measured
#     against a real 叩天关 minimap, the stacked image scored 5 near-anchor matches while
#     the correct floor's own composite scored 21.
# So each floor gets its own composite, and the pack lists them all at the same
# coordinate - the runtime then matches whichever floor the player is actually in.
#
# Output: <OutputRoot>/<region>/k<factor>/L<layer>_F<floor>_<state>_<x>_<y>.png
# plus composite.manifest.json listing the lot. Tiles without an overlay are simply not
# written: the pack keeps using the surface archive for those coordinates.
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
    # Surface brightness under the overlays. 0.35 measured best against a real in-game
    # minimap inside 眠龙庭 (30 near-anchor matches vs 13 at 1.0) and again inside 叩天关;
    # 1.0 is kept as the plain surface+overlay composite.
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

# A layer belongs to this region when its entrance marker (item FCRK) is nearest to one of
# the region's own anchors - the same attribution the region registry uses.
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

$tileRoot = Join-Path $TileArchiveRoot $tileVersion

function Get-OpaqueSampleCount([string]$path) {
    $bitmap = [System.Drawing.Bitmap]::FromFile($path)
    $count = 0
    for ($y = 0; $y -lt $bitmap.Height; $y += 4) {
        for ($x = 0; $x -lt $bitmap.Width; $x += 4) { if ($bitmap.GetPixel($x, $y).A -gt 8) { $count++ } }
    }
    $bitmap.Dispose()
    return $count
}

function New-LayeredTile([string]$surfacePath, [string]$overlayPath, [string]$outPath, [double]$factor) {
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
    $overlay = [System.Drawing.Image]::FromFile($overlayPath)
    $graphics.DrawImage($overlay, $rect)
    $overlay.Dispose()
    $graphics.Dispose()
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $outPath) | Out-Null
    # Never open the destination in place: a previous run may have left a hard link
    # there, and GDI+ would then write through it into a file outside this directory.
    $tempPath = "$outPath.tmp"
    $canvas.Save($tempPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $canvas.Dispose()
    if (Test-Path -LiteralPath $outPath) { Remove-Item -LiteralPath $outPath -Force }
    Move-Item -LiteralPath $tempPath -Destination $outPath -Force
}

$records = New-Object System.Collections.ArrayList
foreach ($layer in $regionLayers) {
    foreach ($floor in $layer.floors) {
        foreach ($image in @($floor.tiles)) {
            $leaf = ($image -split '/')[-1]                                # 3_-3.png
            $surfacePath = Join-Path $tileRoot "$state/${state}_$leaf"
            $overlayPath = Join-Path $LayerArchiveRoot "$tileVersion/$state$image"
            if (-not (Test-Path -LiteralPath $surfacePath)) {
                Write-Warning "No archived surface tile for $state/$leaf; skipping $($layer.name) $($floor.name)."
                continue
            }
            if (-not (Test-Path -LiteralPath $overlayPath)) { throw "Layer tile is missing: $overlayPath" }
            $opaque = Get-OpaqueSampleCount $overlayPath
            if ($opaque -eq 0) { continue }                                 # fully transparent floor
            $parts = ($leaf -replace '\.png$', '').Split('_')
            $floorTag = $floor.id -replace '/', '-'
            [void]$records.Add([pscustomobject]@{
                layerId = [string]$layer.id; layerName = [string]$layer.name
                floorId = [string]$floor.id; floorName = [string]$floor.name
                x = [int]$parts[0]; y = [int]$parts[1]
                surfacePath = $surfacePath; overlayPath = $overlayPath
                opaqueSamples = $opaque
                file = "L$($layer.id)_F$floorTag`_${state}_$leaf"
            })
        }
    }
}
if ($records.Count -eq 0) { throw "No opaque layered tile found for $RegionId." }

$manifest = [ordered]@{
    formatVersion = 1
    regionId = $RegionId
    frame = $state
    tileResourceVersion = $tileVersion
    generatedAtUtc = [DateTime]::UtcNow.ToString('o')
    factors = [ordered]@{}
}

foreach ($factor in @($BaseFactor | Sort-Object -Descending)) {
    $tag = 'k{0:D3}' -f [int][Math]::Round($factor * 100)
    $outDir = Join-Path $OutputRoot "$RegionId/$tag"
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $entries = New-Object System.Collections.ArrayList
    foreach ($record in $records) {
        $outPath = Join-Path $outDir $record.file
        New-LayeredTile $record.surfacePath $record.overlayPath $outPath $factor
        [void]$entries.Add([ordered]@{
            x = $record.x; y = $record.y; file = $record.file
            layerId = $record.layerId; layerName = $record.layerName
            floorId = $record.floorId; floorName = $record.floorName
            opaqueSamples = $record.opaqueSamples
            # Kept so the floor index can read this floor's own alpha mask without having to
            # guess the upstream path layout again.
            overlay = "$($record.layerId)/$(([string]$record.floorId -split '/')[0])/$($record.x)_$($record.y).png"
            sha256 = (Get-FileHash -LiteralPath $outPath -Algorithm SHA256).Hash.ToLowerInvariant()
        })
    }
    $manifest.factors[$tag] = [ordered]@{ baseFactor = $factor; tiles = @($entries) }
    Write-Host ("  {0}: {1} per-floor composites -> {2}" -f $tag, $entries.Count, $outDir)
}

$manifestPath = Join-Path $OutputRoot "$RegionId/composite.manifest.json"
[IO.File]::WriteAllText($manifestPath, (($manifest | ConvertTo-Json -Depth 10) + [Environment]::NewLine), [Text.UTF8Encoding]::new($false))
Write-Host "Manifest: $manifestPath" -ForegroundColor Green
