# Marks every layered-floor index with whether its map is open to the surface ("开放分层地图") or
# enclosed ("封闭分层地图"). See scripts/LayeredSurfaceAccess.ps1 for the rule and the evidence.
#
# Run after the index exists; it does not rebuild any feature file, so the packs' .imf and their
# hashes are untouched - only floor-index.json gains one field per floor.
#
#   pwsh -File scripts\Set-LayeredSurfaceAccess.ps1 -Report        # report only, writes nothing
#   pwsh -File scripts\Set-LayeredSurfaceAccess.ps1                # report + write
#   pwsh -File scripts\Set-LayeredSurfaceAccess.ps1 -Region lahai
#
# This is the retro-fit path for packs built before the field existed. New-LayeredFloorIndex.ps1
# writes the same value while it builds, so a rebuilt pack carries it without this step.
#
# A report run also verifies the DERIVED open set against the recorded expectation in
# LayeredSurfaceAccess.ps1 (`$LayeredSurfaceAccessExpectedOpen`): a data change upstream (an entrance
# marker added or removed, a rebuilt tile) moves the derivation, and the answer to "did a map's
# class change?" must be a failing script, not a player noticing that markers vanish again.

[CmdletBinding()]
param(
    [string]$SourceRoot,
    [string]$PackRoot,
    [string]$Region,
    [switch]$Report
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $SourceRoot) { $SourceRoot = Split-Path -Parent $PSScriptRoot }
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
if (-not $PackRoot) { $PackRoot = Join-Path $SourceRoot 'Assets/FeaturesDatas/KuroTilePacks' }

. (Join-Path $PSScriptRoot 'LayeredSurfaceAccess.ps1')

$rows = New-Object System.Collections.ArrayList
$resolvedOpen = New-Object System.Collections.ArrayList
$touched = 0
$changed = 0

$regions = Get-ChildItem -LiteralPath $PackRoot -Directory |
    Where-Object { -not $Region -or $_.Name -eq $Region } |
    Sort-Object Name

foreach ($regionDirectory in $regions) {
    $indexPath = Join-Path $regionDirectory.FullName 'layered-floors/floor-index.json'
    if (-not (Test-Path -LiteralPath $indexPath)) { continue }
    $index = Get-Content -LiteralPath $indexPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $frame = [int]$index.frame
    $gridSize = [int]$index.gridSize
    $entranceLayers = @(Get-LayeredMapEntranceLayers -SourceRoot $SourceRoot -Frame $frame)

    # Openness is a property of the map, and the measurement that can see it lives on the ground
    # floor, so that floor's value is resolved once per layer and used for all of its floors.
    $groundCopied = @{}
    foreach ($floor in @($index.floors)) {
        if ([int]("$($floor.floorId)".Split('/')[0]) -ne -1) { continue }
        $groundCopied[[int]$floor.layerId] = Get-LayeredFloorCopiedFraction -Floor $floor -GridSize $gridSize
    }

    $regionChanged = 0
    foreach ($floor in @($index.floors)) {
        $layerId = [int]$floor.layerId
        $floorId = [string]$floor.floorId
        $level = [int]$floorId.Split('/')[0]
        if (-not $groundCopied.ContainsKey($layerId)) {
            Write-Warning "$($regionDirectory.Name) $floorId`: no level -1 floor in this index; the map is treated as enclosed unless a fact says otherwise"
        }
        $ground = if ($groundCopied.ContainsKey($layerId)) { [double]$groundCopied[$layerId] } else { 0.0 }
        $resolved = Resolve-LayeredSurfaceAccess -Region $regionDirectory.Name -LayerId $layerId -FloorId $floorId `
            -LayerGroundCopiedFraction $ground -HasEntrance ($entranceLayers -contains $layerId)
        $stored = if ($null -ne $floor.PSObject.Properties['surfaceAccess']) { [string]$floor.surfaceAccess } else { '' }
        $isChange = $stored -ne $resolved.Access
        if ($isChange) { ++$regionChanged }
        if ($resolved.Access -eq $script:SurfaceAccessOpen) { [void]$resolvedOpen.Add("$($regionDirectory.Name)/$floorId") }

        $floor | Add-Member -NotePropertyName surfaceAccess -NotePropertyValue $resolved.Access -Force
        $floor | Add-Member -NotePropertyName surfaceAccessReason -NotePropertyValue $resolved.Reason -Force
        [void]$rows.Add([pscustomobject]@{
                Region = $regionDirectory.Name; Layer = $layerId; Floor = $floorId; Name = $floor.floorName
                GroundCopied = [math]::Round($ground, 3); Entrance = ($entranceLayers -contains $layerId)
                Access = $resolved.Access; Source = $resolved.Source; Stored = $stored; Change = $isChange
                Reason = $resolved.Reason
            })
    }

    $changed += $regionChanged
    if (-not $Report) {
        $json = ($index | ConvertTo-Json -Depth 8) + [Environment]::NewLine
        [IO.File]::WriteAllText($indexPath, $json, [Text.UTF8Encoding]::new($false))
        ++$touched
    }
    Write-Host ("{0,-14} frame={1,-4} floors={2,2} entranceMaps={3,2} open={4,2} changed={5,2}" -f `
            $regionDirectory.Name, $frame, @($index.floors).Count, $entranceLayers.Count,
        @($rows | Where-Object { $_.Region -eq $regionDirectory.Name -and $_.Access -eq 'open' }).Count,
        $regionChanged)
}

Write-Host ''
Write-Host ('{0,-14} {1,-8} {2,-22} {3,7} {4,9} {5,-9} {6,-10} {7,7}' -f `
        'region', 'floor', 'name', 'ground', 'entrance', 'access', 'source', 'changed')
foreach ($row in $rows) {
    Write-Host ('{0,-14} {1,-8} {2,-22} {3,7:N3} {4,9} {5,-9} {6,-10} {7,7}' -f `
            $row.Region, $row.Floor, $row.Name, $row.GroundCopied, $row.Entrance, $row.Access, $row.Source,
        $(if ($row.Change) { 'yes' } else { '' }))
}

$open = @($rows | Where-Object { $_.Access -eq 'open' })
Write-Host ''
Write-Host ("open floors: {0}" -f $(if ($open.Count -eq 0) { '(none)' } else { ($open | ForEach-Object { "$($_.Region)/$($_.Floor)" }) -join ', ' }))
Write-Host ("expected:    {0}" -f ($script:LayeredSurfaceAccessExpectedOpen -join ', '))

# Which floors the derivation opens, independent of what the shipped index currently says. A
# mismatch here is a data change, not a forgotten retro-fit, and the two must not be confused.
$derivedOpen = @($resolvedOpen | Sort-Object)
$expectedOpen = @($script:LayeredSurfaceAccessExpectedOpen | Sort-Object)
if (($derivedOpen -join ',') -ne ($expectedOpen -join ',')) {
    throw "The derived open set changed: got [$($derivedOpen -join ', ')] but expected [$($expectedOpen -join ', ')]. Review the rule and the recorded facts in scripts/LayeredSurfaceAccess.ps1 before shipping packs."
}

if ($Report) { Write-Host 'Report only: no index was rewritten.' -ForegroundColor Yellow }
else { Write-Host "Rewrote $touched floor-index.json file(s); $changed floor(s) changed." -ForegroundColor Green }
