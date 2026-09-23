# Builds the layered-floor sidecar index: one small feature file per floor, so the runtime
# can answer "which floor is the player standing in?".
#
# Why this exists: several floors of a layered map share one tile coordinate, so the pack
# itself cannot say which floor a minimap belongs to - every appearance lives at the same
# coordinates. The floor is identifiable from the imagery (measured: on a real 叩天关
# minimap the correct floor's own composite scored 21 near-anchor matches where the other
# floors scored 0-6), so each floor gets its own descriptor set and the runtime votes.
#
# One file per floor, in the IMF format the runtime already reads (FeatureBinaryCodec), so
# no new container format is needed. Output:
#   <OutputRoot>/L<layer>_F<floor>.imf + <OutputRoot>/floor-index.json
#
#   pwsh -File scripts\New-LayeredFloorIndex.ps1 -RegionId jinzhou

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RegionId,
    [string]$SourceRoot,
    [string]$CompositeRoot,
    [string]$LayerArchiveRoot,
    [string]$OutputRoot,
    [string]$Version = 'B50F4135DCCC4D8DA87ED33CE95EA31D',
    [ValidateSet('k100', 'k035')]
    [string]$Factor = 'k035'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $SourceRoot) { $SourceRoot = Split-Path -Parent $PSScriptRoot }
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
if (-not $CompositeRoot) { $CompositeRoot = Join-Path $SourceRoot "out/map-regions/composite/$RegionId/$Factor" }
if (-not $LayerArchiveRoot) { $LayerArchiveRoot = Join-Path $SourceRoot 'map-regions/layers' }
if (-not $OutputRoot) { $OutputRoot = Join-Path $SourceRoot "out/map-regions/packs/$RegionId/layered-floors" }

$builder = Join-Path $SourceRoot 'x64/RelWithDebInfo/KuroMapFeatureBuilder.exe'
$converter = Join-Path $SourceRoot 'x64/Release/IMaoFeatureConverter.exe'
foreach ($tool in $builder, $converter) {
    if (-not (Test-Path -LiteralPath $tool)) { throw "Missing tool: $tool (build it first)." }
}
$env:PATH = (Join-Path $SourceRoot 'x64/Release') + ';' + $env:PATH

$compositeManifestPath = Join-Path (Split-Path -Parent $CompositeRoot) 'composite.manifest.json'
if (-not (Test-Path -LiteralPath $compositeManifestPath)) {
    throw "Composite manifest is missing: $compositeManifestPath (run scripts/New-LayeredTileComposite.ps1 first)."
}
$compositeManifest = Get-Content -LiteralPath $compositeManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
$factorNode = $compositeManifest.factors.PSObject.Properties[$Factor]
if ($null -eq $factorNode) {
    throw "The composite manifest has no '$Factor' factor. Available: $(($compositeManifest.factors.PSObject.Properties.Name) -join ', ')"
}
$tiles = @($factorNode.Value.tiles)
if ($tiles.Count -eq 0) { throw "The composite manifest has no tiles for $Factor." }

$state = [int]$compositeManifest.frame

# Scene name and runtime scene id come from the region registry and its shipped pack: the
# builder refuses a frame that does not belong to the scene it is told to build.
$registry = Get-Content -LiteralPath (Join-Path $SourceRoot 'map-regions/regions.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$regionRecord = @($registry.regions | Where-Object { $_.id -eq $RegionId })
if ($regionRecord.Count -ne 1) { throw "Region '$RegionId' is not in the registry exactly once." }
$scene = [string]$regionRecord[0].scene
# The footprint grid and the scope centre are both expressed as map coordinates, and 每个 scene 有
# 自己的坐标系: World is (2474,1957) @1.205 while, for example, 下层金库 is (-3.5,-2.5) @1.2053.
# Hardcoding World's numbers made every non-World index test containment against the wrong point,
# so no floor ever contained the player there (runtime log: containing=[] in 下层金库).
$originX = 2474.0; $originY = 1957.0; $scale = 1.205
$calibrationPath = Join-Path $SourceRoot 'Assets/KuroMap/scene-calibrations.json'
if (Test-Path -LiteralPath $calibrationPath) {
    $calibrations = Get-Content -LiteralPath $calibrationPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $calibration = $calibrations.scenes.PSObject.Properties[$scene]
    if ($null -ne $calibration) {
        $originX = [double]$calibration.Value.coordinateTransform.originX
        $originY = [double]$calibration.Value.coordinateTransform.originY
        $scale = [double]$calibration.Value.coordinateTransform.scale
        Write-Host ("  scene transform: origin=({0},{1}) scale={2}" -f [math]::Round($originX, 3), [math]::Round($originY, 3), [math]::Round($scale, 5))
    }
    else { Write-Host "  no calibration for scene '$scene'; using the World transform" }
}
$sceneId = 1
foreach ($candidate in (Join-Path $SourceRoot 'Assets/FeaturesDatas/KuroTilePacks'), (Join-Path $SourceRoot "out/map-regions/packs/$RegionId")) {
    $manifestPath = Join-Path $candidate "$RegionId/manifest.json"
    if ($RegionId -eq (Split-Path -Leaf $candidate)) { $manifestPath = Join-Path $candidate 'manifest.json' }
    if (Test-Path -LiteralPath $manifestPath) {
        $sceneId = [int](Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json).sceneId
        break
    }
}
Write-Host "Region $RegionId builds in scene '$scene' (sceneId=$sceneId), frame $state"

# Coarse "is the player standing inside this floor's cave" mask: the overlay's alpha
# downsampled to a 64x64 grid per tile (16 px cells), written as hex. The classifier's vote
# count alone is not enough - on a real 眠龙庭·上层 position it scored 4-11 against a
# threshold of 10 while surface frames reach 6 - but the floors' footprints barely overlap
# (IoU 13.7% between 上层 and 下层 on the same tile), so containment separates them.
$gridSize = 64
$cell = 1024 / $gridSize

Add-Type -AssemblyName System.Drawing

function Get-OccupancyHex([string]$overlayPath) {
    $bitmap = [System.Drawing.Bitmap]::FromFile($overlayPath)
    $bytes = New-Object byte[] ($gridSize * $gridSize)
    for ($gy = 0; $gy -lt $gridSize; ++$gy) {
        for ($gx = 0; $gx -lt $gridSize; ++$gx) {
            $opaque = 0
            for ($y = [int]($gy * $cell); $y -lt [int](($gy + 1) * $cell) -and $opaque -eq 0; $y += 4) {
                for ($x = [int]($gx * $cell); $x -lt [int](($gx + 1) * $cell); $x += 4) {
                    if ($bitmap.GetPixel($x, $y).A -gt 8) { $opaque = 1; break }
                }
            }
            $bytes[$gy * $gridSize + $gx] = $opaque
        }
    }
    $bitmap.Dispose()
    $hex = New-Object System.Text.StringBuilder ($gridSize * $gridSize / 4)
    for ($i = 0; $i -lt $bytes.Length; $i += 4) {
        $nibble = $bytes[$i] -bor ($bytes[$i + 1] -shl 1) -bor ($bytes[$i + 2] -shl 2) -bor ($bytes[$i + 3] -shl 3)
        [void]$hex.Append('0123456789abcdef'[$nibble])
    }
    return $hex.ToString()
}

$groups = $tiles | Group-Object { "L$($_.layerId)|$($_.floorId)" }
Write-Host ("Region {0}: {1} floor groups over {2} composite tiles ({3})" -f $RegionId, $groups.Count, $tiles.Count, $Factor)

New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$work = Join-Path $SourceRoot 'out/map-regions/floor-index-work'
New-Item -ItemType Directory -Force -Path $work | Out-Null

$entries = New-Object System.Collections.ArrayList
foreach ($group in $groups | Sort-Object Name) {
    $first = $group.Group[0]
    $layerId = [int]$first.layerId
    $floorId = [string]$first.floorId
    $tag = "L${layerId}_F$($floorId -replace '/','-')"
    $floorDir = Join-Path $work $tag
    $tileDir = Join-Path $floorDir 'tiles'
    New-Item -ItemType Directory -Force -Path $tileDir | Out-Null
    $specs = foreach ($tile in $group.Group) {
        $source = Join-Path $CompositeRoot $tile.file
        if (-not (Test-Path -LiteralPath $source)) { throw "Composite tile is missing: $source" }
        $link = Join-Path $tileDir $tile.file
        if (Test-Path -LiteralPath $link) { Remove-Item -LiteralPath $link -Force }
        try { New-Item -ItemType HardLink -Path $link -Target $source -ErrorAction Stop | Out-Null }
        catch { Copy-Item -LiteralPath $source -Destination $link -Force }
        [ordered]@{ x = [int]$tile.x; y = [int]$tile.y; file = "tiles/$($tile.file)"
            sha256 = (Get-FileHash -LiteralPath $link -Algorithm SHA256).Hash.ToLowerInvariant() }
    }
    # The builder validates the frame against the scene, so a layered map in 泰缇斯之底 (frame
    # 900) cannot be built as 'World' any more than 今州's can be built as 'Tethys'. The region
    # registry names the scene; the shipped pack for the region is the authority on its runtime
    # scene id.
    $manifest = [ordered]@{
        formatVersion = 1; packId = "$RegionId-floor-$tag"; resourceVersion = $Version
        scene = $scene; sceneId = $sceneId
        source = [ordered]@{ static = 'https://web-static.kurobbs.com'; state = $state; tileSize = 1024; virtualMapSize = 850.0 }
        coordinateTransform = [ordered]@{ originX = $originX; originY = $originY; scale = $scale }
        tiles = @($specs)
    }
    $manifestPath = Join-Path $floorDir 'manifest.json'
    [IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
    $features = Join-Path $floorDir 'features.yml'
    $report = Join-Path $floorDir 'report.json'
    $stdout = & $builder --input $manifestPath --output $features --report $report 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Floor index build failed for $tag`: $($stdout -join ' ')" }
    $keypoints = [int](Get-Content -LiteralPath $report -Raw | ConvertFrom-Json).extractedKeypoints
    $imf = Join-Path $OutputRoot "$tag.imf"
    $imfManifest = Join-Path $OutputRoot "$tag.imf.manifest.json"
    & $converter $features $imf $imfManifest | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Floor index conversion failed for $tag." }
    [void]$entries.Add([ordered]@{
        layerId = $layerId; floorId = $floorId
        layerName = [string]$first.layerName; floorName = [string]$first.floorName
        file = "$tag.imf"; keypointCount = $keypoints
        tiles = @($group.Group | ForEach-Object {
            $overlayPath = Join-Path $LayerArchiveRoot "$Version/$state/$($_.overlay)"
            $occupancy = if (Test-Path -LiteralPath $overlayPath) { Get-OccupancyHex $overlayPath } else { '' }
            [ordered]@{ x = [int]$_.x; y = [int]$_.y; occupancy = $occupancy }
        })
    })
    Write-Host ("  {0,-16} {1,-22} keypoints={2,6}  tiles={3}" -f $tag, $first.floorName, $keypoints, $group.Count)
}

$index = [ordered]@{
    formatVersion = 1
    regionId = $RegionId
    frame = $state
    baseFactor = [double]$factorNode.Value.baseFactor
    tileResourceVersion = $Version
    # Needed to turn a runtime map coordinate into a tile pixel when testing the footprints.
    coordinateTransform = [ordered]@{ originX = $originX; originY = $originY; scale = $scale; virtualMapSize = 850.0; tileSize = 1024 }
    gridSize = $gridSize
    generatedAtUtc = [DateTime]::UtcNow.ToString('o')
    floors = @($entries)
}
$indexPath = Join-Path $OutputRoot 'floor-index.json'
[IO.File]::WriteAllText($indexPath, (($index | ConvertTo-Json -Depth 8) + [Environment]::NewLine), [Text.UTF8Encoding]::new($false))
Write-Host "Floor index: $indexPath" -ForegroundColor Green
