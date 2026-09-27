[CmdletBinding()]
param(
    [string]$SourceRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$Region,
    [string]$Item,
    [int]$Top = 12
)

# Which collectible points in a layered-map ("分层地图") region carry a floor of their own, and
# which do not. A point with no floor is a SURFACE point: the runtime draws it as "outside the
# layer" and hides it whenever the player is classified onto any floor of that region
# (IMao-Core/src/Runtime/LayeredMapState.cpp, RoleFor -> SurfaceRole). So this report answers,
# before anyone has to play the game, "would this item type disappear while I stand in there?".
#
# Reported per region:
#   points        all points of the region
#   layered       points carrying floorId + a real level (they follow a floor: Current/Above/Below)
#   surface       points with no floor - hidden while any floor of the region is active
#   inFootprint   of those surface points, the ones a floor's occupancy grid actually covers:
#                 these are the points the player is most likely standing next to when the icon
#                 vanishes, and the ones worth checking against upstream data first
#   releasable    the region has a floor next to the surface whose art is NOT a copy of the
#                 surface's own (LayeredFloors::SharesSurfaceGround): there, standing on copied
#                 ground sets Snapshot::sharedGround and the surface points come back. Regions
#                 where that is false hide their surface points unconditionally - 拉海洛's
#                 星炬学院 is the case that made the distinction (its floors are 58-84% the
#                 surface's own pixels, so the release would resurrect markers the layer owns).
#
# Usage:
#   pwsh -File scripts\Get-LayeredMarkerCoverage.ps1
#   pwsh -File scripts\Get-LayeredMarkerCoverage.ps1 -Region lahai
#   pwsh -File scripts\Get-LayeredMarkerCoverage.ps1 -Item 终声残卷
#
# Note for frame 8: 今州/梦州/拉古那/七丘/冰原 all live in ONE item file (itemsData_World.json),
# so those rows repeat the whole file per region and must not be added up.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$packRoot = Join-Path $SourceRoot 'Assets\FeaturesDatas\KuroTilePacks'
$stateManifest = Get-Content -LiteralPath (Join-Path $SourceRoot 'Assets\KuroMap\manifest.json') -Raw | ConvertFrom-Json
$runtimeByState = @{}
foreach ($state in $stateManifest.states) { $runtimeByState[[int]$state.state] = [string]$state.runtime }

function Get-ItemDataPath([string]$Runtime) {
    $embedded = Join-Path $SourceRoot "IMao-Core/src/Resource/itemsData_$Runtime.json"
    if (Test-Path -LiteralPath $embedded) { return $embedded }
    $staged = Join-Path $SourceRoot "Assets/KuroMap/runtime/itemsData_$Runtime.json"
    if (Test-Path -LiteralPath $staged) { return $staged }
    return $null
}

function ConvertFrom-Grid([string]$Grid, [int]$Bits) {
    if ([string]::IsNullOrEmpty($Grid) -or $Grid.Length * 4 -ne $Bits) { return $null }
    $cells = [bool[]]::new($Bits)
    for ($bit = 0; $bit -lt $Bits; $bit++) {
        $nibble = [Convert]::ToInt32($Grid[[int][math]::Floor($bit / 4)], 16)
        $cells[$bit] = (($nibble -shr ($bit % 4)) -band 1) -eq 1
    }
    return , $cells
}

# Point data is stored in Kuro's hundredths of a game unit (x = -64000 is game (-640, -5377)), while
# LocateCell takes the runtime's ImgMap coordinate: mapX = gameX * scale + origin. Reading the point
# as if it already were that coordinate shifts every query by ~2 tiles and silently reports "no
# footprint covers this point" for nearly everything - measured on the points upstream DOES give a
# floor: 100/104 land inside their own floor with this conversion, 55/104 without it.
function ConvertTo-MapCoordinate($Transform, [double]$RawX, [double]$RawY) {
    return @{
        X = ($RawX / 100.0) * $Transform.scale + $Transform.originX
        Y = ($RawY / 100.0) * $Transform.scale + $Transform.originY
    }
}

# The same map -> tile -> cell mapping LayeredFloors::LocateCell uses, kept in one place so the
# report cannot drift from the runtime.
function Get-CellLocation($Floor, $Transform, [int]$GridSize, [double]$MapX, [double]$MapY) {
    $gameX = ($MapX - $Transform.originX) / $Transform.scale
    $gameY = ($MapY - $Transform.originY) / $Transform.scale
    $tileX = [int][math]::Floor($gameX / $Transform.virtualMapSize + 1.0)
    $tileY = [int][math]::Ceiling(-$gameY / $Transform.virtualMapSize)
    $cellSize = $Transform.tileSize / $GridSize
    $pixelX = $gameX * $Transform.tileSize / $Transform.virtualMapSize + $Transform.tileSize - $tileX * $Transform.tileSize
    $pixelY = $tileY * $Transform.tileSize + $gameY * $Transform.tileSize / $Transform.virtualMapSize
    if ($pixelX -lt 0 -or $pixelY -lt 0 -or $pixelX -ge $Transform.tileSize -or $pixelY -ge $Transform.tileSize) { return $null }
    $tile = $null
    foreach ($candidate in $Floor.tiles) {
        if ($candidate.x -eq $tileX -and $candidate.y -eq $tileY) { $tile = $candidate; break }
    }
    if ($null -eq $tile) { return $null }
    return @{
        Occupancy = $tile.OccupancyCells
        CellX     = [int][math]::Floor($pixelX / $cellSize)
        CellY     = [int][math]::Floor($pixelY / $cellSize)
        GridSize  = $GridSize
    }
}

function Test-FloorContains($Floor, $Transform, [int]$GridSize, [double]$MapX, [double]$MapY) {
    $location = Get-CellLocation $Floor $Transform $GridSize $MapX $MapY
    if ($null -eq $location) { return $false }
    # One cell of slack, exactly as LayeredFloors::Contains.
    for ($dy = -1; $dy -le 1; $dy++) {
        for ($dx = -1; $dx -le 1; $dx++) {
            $gx = $location.CellX + $dx
            $gy = $location.CellY + $dy
            if ($gx -lt 0 -or $gy -lt 0 -or $gx -ge $location.GridSize -or $gy -ge $location.GridSize) { continue }
            if ($location.Occupancy[$gy * $location.GridSize + $gx]) { return $true }
        }
    }
    return $false
}

$regions = @()
foreach ($pack in Get-ChildItem -LiteralPath $packRoot -Directory) {
    if ($Region -and $pack.Name -ne $Region) { continue }
    $indexPath = Join-Path $pack.FullName 'layered-floors\floor-index.json'
    if (-not (Test-Path -LiteralPath $indexPath)) { continue }
    $index = Get-Content -LiteralPath $indexPath -Raw | ConvertFrom-Json
    $frame = [int]$index.frame
    $runtime = if ($runtimeByState.ContainsKey($frame)) { $runtimeByState[$frame] } else { $null }
    if ($null -eq $runtime) { Write-Warning "no runtime scene is registered for frame $frame ($($pack.Name))"; continue }
    $itemPath = Get-ItemDataPath $runtime
    if ($null -eq $itemPath) { Write-Warning "no item data for $runtime ($($pack.Name))"; continue }

    $gridSize = [int]$index.gridSize
    $bits = $gridSize * $gridSize
    $transform = $index.coordinateTransform
    $floors = @()
    foreach ($floor in $index.floors) {
        $tiles = @()
        $occupied = 0
        $shared = 0
        foreach ($tile in @($floor.tiles)) {
            $occupancy = ConvertFrom-Grid ([string]$tile.occupancy) $bits
            if ($null -eq $occupancy) { continue }
            $sharedGrid = ConvertFrom-Grid ([string]$tile.shared) $bits
            for ($bit = 0; $bit -lt $bits; $bit++) {
                if (-not $occupancy[$bit]) { continue }
                $occupied++
                if ($null -ne $sharedGrid -and $sharedGrid[$bit]) { $shared++ }
            }
            $tiles += [pscustomobject]@{ x = [int]$tile.x; y = [int]$tile.y; OccupancyCells = $occupancy }
        }
        $copiedFraction = if ($occupied -gt 0) { $shared / $occupied } else { 0.0 }
        $floors += [pscustomobject]@{
            floorId        = [string]$floor.floorId
            floorName      = [string]$floor.floorName
            layerName      = [string]$floor.layerName
            level          = [int]($floor.floorId.Split('/')[0])
            tiles          = $tiles
            copiedFraction = $copiedFraction
        }
    }

    $items = Get-Content -LiteralPath $itemPath -Raw | ConvertFrom-Json
    $rows = [Collections.Generic.List[object]]::new()
    $totals = [ordered]@{ points = 0; layered = 0; surface = 0; inFootprint = 0 }
    foreach ($group in $items) {
        $name = [string]$group.name
        if ($Item -and $name -notlike "*$Item*" -and [string]$group.id -notlike "*$Item*") { continue }
        $points = 0; $layeredCount = 0; $surfaceCount = 0; $inFootprint = 0
        foreach ($location in @($group.location)) {
            $points++
            $hasFloor = -not [string]::IsNullOrWhiteSpace([string]$location.floorId) -and
                -not [string]::IsNullOrWhiteSpace([string]$location.level) -and
                [string]$location.level -ne '0'
            if ($hasFloor) { $layeredCount++; continue }
            $surfaceCount++
            $map = ConvertTo-MapCoordinate $transform ([double]$location.x) ([double]$location.y)
            foreach ($floor in $floors) {
                if (Test-FloorContains $floor $transform $gridSize $map.X $map.Y) { $inFootprint++; break }
            }
        }
        $totals.points += $points
        $totals.layered += $layeredCount
        $totals.surface += $surfaceCount
        $totals.inFootprint += $inFootprint
        $rows.Add([pscustomobject]@{
            Region = $pack.Name; Frame = $frame; Item = $name
            Points = $points; Layered = $layeredCount; Surface = $surfaceCount; InFootprint = $inFootprint
        })
    }

    $releasable = @($floors | Where-Object { $_.level -eq -1 -and $_.copiedFraction -lt 0.5 })
    $blocked = @($floors | Where-Object { $_.level -eq -1 -and $_.copiedFraction -ge 0.5 })
    Write-Host ("{0} (frame {1}): points={2} layered={3} surface={4} surface-inside-a-footprint={5}" -f `
        $pack.Name, $frame, $totals.points, $totals.layered, $totals.surface, $totals.inFootprint)
    if ($blocked.Count -gt 0) {
        foreach ($floor in $blocked) {
            Write-Host ("    no surface-ground release: {0} ({1}) copiedFraction={2:N3}" -f `
                $floor.floorId, $floor.floorName, $floor.copiedFraction)
        }
    }
    if ($releasable.Count -eq 0) {
        Write-Host '    every surface point of this region hides unconditionally while a floor is active'
    }
    $offsenders = @($rows | Where-Object { $_.InFootprint -gt 0 } | Sort-Object InFootprint -Descending | Select-Object -First $Top)
    if ($offsenders.Count -gt 0) {
        Write-Host '    item types with surface points a floor covers (candidates for a missing floor):'
        Write-Host ($offsenders | Format-Table -AutoSize | Out-String).TrimEnd()
    }
    $regions += [pscustomobject]@{ Region = $pack.Name; Frame = $frame; Runtime = $runtime; Rows = $rows; Totals = $totals }
}

if ($regions.Count -eq 0) { throw "No layered floor index found under $packRoot." }
$summary = $regions | ForEach-Object {
    [pscustomobject]@{
        Region = $_.Region; Frame = $_.Frame; Runtime = $_.Runtime
        Points = $_.Totals.points; Layered = $_.Totals.layered
        Surface = $_.Totals.surface; SurfaceInFootprint = $_.Totals.inFootprint
    }
}
Write-Output ''
Write-Output '=== per region ==='
($summary | Format-Table -AutoSize | Out-String).TrimEnd()
$all = @($regions | ForEach-Object { $_.Rows }) | Where-Object { $_.Surface -gt 0 }
Write-Output ''
Write-Output "=== item types with the most surface points inside a layered footprint (top $Top) ==="
($all | Sort-Object InFootprint -Descending | Select-Object -First $Top | Format-Table -AutoSize | Out-String).TrimEnd()
