# Adds the "this descriptor came from the layer's own art" grid to every layered-floor index.
#
# Run after the index exists; it does not rebuild any feature file, so the packs' .imf and their
# hashes are untouched - only floor-index.json gains two fields per floor. See
# scripts/LayeredOwnArtMask.ps1 for what the grid means and why the runtime needs it.
#
#   pwsh -File scripts\Set-LayeredOwnArtMask.ps1                 # report + write
#   pwsh -File scripts\Set-LayeredOwnArtMask.ps1 -Report         # report only
#   pwsh -File scripts\Set-LayeredOwnArtMask.ps1 -Region jinzhou
#
# This is the retro-fit path for packs built before the grid existed. New-LayeredFloorIndex.ps1
# calls the same helper while it builds, so a rebuilt pack carries the grid without this step.

[CmdletBinding()]
param(
    [string]$SourceRoot,
    [string]$PackRoot,
    [string]$LayerArchiveRoot,
    [string]$Region,
    [switch]$Report
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $SourceRoot) { $SourceRoot = Split-Path -Parent $PSScriptRoot }
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
if (-not $PackRoot) { $PackRoot = Join-Path $SourceRoot 'Assets/FeaturesDatas/KuroTilePacks' }
if (-not $LayerArchiveRoot) { $LayerArchiveRoot = Join-Path $SourceRoot 'map-regions/layers' }

. (Join-Path $PSScriptRoot 'LayeredOwnArtMask.ps1')

$alphaCache = @{}
$rows = New-Object System.Collections.ArrayList
$touched = 0

$regions = Get-ChildItem -LiteralPath $PackRoot -Directory |
    Where-Object { -not $Region -or $_.Name -eq $Region } |
    Sort-Object Name

foreach ($regionDirectory in $regions) {
    $indexPath = Join-Path $regionDirectory.FullName 'layered-floors/floor-index.json'
    if (-not (Test-Path -LiteralPath $indexPath)) { continue }
    $index = Get-Content -LiteralPath $indexPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $frame = [int]$index.frame
    $version = if ($index.PSObject.Properties.Name -contains 'tileResourceVersion') {
        [string]$index.tileResourceVersion
    } else { 'B50F4135DCCC4D8DA87ED33CE95EA31D' }
    $transform = @{
        originX = [double]$index.coordinateTransform.originX
        originY = [double]$index.coordinateTransform.originY
        scale = [double]$index.coordinateTransform.scale
        virtualMapSize = [double]$index.coordinateTransform.virtualMapSize
        tileSize = [double]$index.coordinateTransform.tileSize
    }
    $floorRoot = Join-Path $regionDirectory.FullName 'layered-floors'

    $regionOwn = 0
    $regionTotal = 0
    $regionFloors = 0
    foreach ($floor in $index.floors) {
        $imfPath = Join-Path $floorRoot $floor.file
        if (-not (Test-Path -LiteralPath $imfPath)) {
            Write-Warning "$($regionDirectory.Name) $($floor.floorId): missing $($floor.file); left unset"
            continue
        }
        $level = [int]("$($floor.floorId)".Split('/')[0])
        $overlays = Get-FloorTileOverlays -Floor $floor -Frame $frame -LayerId ([int]$floor.layerId) `
            -Level $level -LayerArchiveRoot $LayerArchiveRoot -Version $version
        if ($overlays.Count -eq 0) {
            Write-Warning "$($regionDirectory.Name) $($floor.floorId): no overlay tiles archived; left unset"
            continue
        }
        $points = Read-ImfKeypoints -Path $imfPath
        # The `shared` grid travels with the same tile list; the mask needs it so that ground the
        # layer copied from the surface is not counted as art the layer drew.
        $sharedGrids = @{}
        foreach ($tile in $floor.tiles) {
            $shared = [string]$tile.shared
            if ($shared) { $sharedGrids["$([int]$tile.x),$([int]$tile.y)"] = $shared }
        }
        # A retro-fit cannot repair a .imf whose coordinates belong to a different frame than the
        # index; Get-OwnArtMask refuses in that case. Skipping the floor leaves whatever mask it
        # already has untouched, which is the safe outcome - writing the zero mask it would otherwise
        # produce would silently switch the own-art veto off for that floor.
        try {
            $mask = Get-OwnArtMask -Points $points -Transform $transform -TileOverlays $overlays `
                -AlphaCache $alphaCache -TileSharedGrids $sharedGrids -GridSize ([int]$index.gridSize)
        }
        catch {
            Write-Warning "$($regionDirectory.Name) $($floor.floorId): $($_.Exception.Message)"
            continue
        }

        $floor | Add-Member -NotePropertyName ownMask -NotePropertyValue $mask.Hex -Force
        $floor | Add-Member -NotePropertyName ownMaskKeypoints -NotePropertyValue $mask.Total -Force
        $regionOwn += $mask.Own
        $regionTotal += $mask.Total
        ++$regionFloors
        [void]$rows.Add([pscustomobject]@{
                Region = $regionDirectory.Name; Floor = "$($floor.layerId)/$($floor.floorId)"
                Name = $floor.floorName; Keypoints = $mask.Total; Own = $mask.Own
                OwnPercent = if ($mask.Total -gt 0) { 100.0 * $mask.Own / $mask.Total } else { 0.0 }
                Outside = $mask.Outside
            })
    }

    if (-not $Report) {
        $json = ($index | ConvertTo-Json -Depth 8) + [Environment]::NewLine
        [IO.File]::WriteAllText($indexPath, $json, [Text.UTF8Encoding]::new($false))
        ++$touched
    }
    Write-Host ("{0,-14} floors={1,2} descriptors={2,7} on own art={3,7} ({4,5:N1}%)" -f `
            $regionDirectory.Name, $regionFloors, $regionTotal, $regionOwn,
        $(if ($regionTotal -gt 0) { 100.0 * $regionOwn / $regionTotal } else { 0 }))
}

Write-Host ''
Write-Host ('{0,-14} {1,-10} {2,-22} {3,8} {4,8} {5,9} {6,8}' -f 'region', 'floor', 'name', 'keypoints', 'own', 'own%', 'outside')
foreach ($row in $rows) {
    Write-Host ('{0,-14} {1,-10} {2,-22} {3,8} {4,8} {5,8:N1}% {6,8}' -f `
            $row.Region, $row.Floor, $row.Name, $row.Keypoints, $row.Own, $row.OwnPercent, $row.Outside)
}
$totalKeypoints = ($rows | Measure-Object -Property Keypoints -Sum).Sum
$totalOwn = ($rows | Measure-Object -Property Own -Sum).Sum
Write-Host ''
Write-Host ("TOTAL floors={0} descriptors={1} on own art={2} ({3:N1}%)" -f `
        $rows.Count, $totalKeypoints, $totalOwn, (100.0 * $totalOwn / $totalKeypoints))
if ($Report) { Write-Host 'Report only: no index was rewritten.' -ForegroundColor Yellow }
else { Write-Host "Rewrote $touched floor-index.json file(s)." -ForegroundColor Green }
