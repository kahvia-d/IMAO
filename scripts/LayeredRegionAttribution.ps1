# Which layered maps ("分层地图") belong to which region - the one rule both the composite step
# and the per-floor index step must agree on.
#
# Dot-source it and call Get-OwnedLayeredIds:
#
#   . (Join-Path $PSScriptRoot 'LayeredRegionAttribution.ps1')
#   $owned = Get-OwnedLayeredIds -RegionId 'qiqiu' -SourceRoot $root
#
# Why it is not a one-liner: a layer manifest is published per FRAME, and frame 8 carries six
# regions at once (今州/梦州/拉古那/七丘/冰原/黑海岸). Attributing by the nearest anchor of any
# level looked right until it was tried: dozens of level-3 nodes carry an empty mapState, so
# 石龙寝 (七丘) and 天槎空间站 (冰原) were handed to 黑海岸 - whose mapState is also empty - and
# 七丘/冰原 were told they had no layered maps at all.
#
# The rule that matches the data:
#   * frame != 8        -> the whole frame is that one region (小世界：一个 frame 一个地区).
#   * frame == 8        -> nearest LEVEL-2 anchor whose mapState belongs to a frame-8 region.
#                          Restricting to frame-8 regions is what stops 拉海洛 (frame 906,
#                          mapState 5) from claiming 天槎空间站, which shares its countryId.
#   * a layer with no entrance marker falls back to the centre of its own tiles.

function Get-LayeredAnchors {
    param([string]$SourceRoot, [string[]]$AllowedMapStates)
    $country = Get-Content -LiteralPath (Join-Path $SourceRoot 'Assets/KuroMap/country.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $anchors = New-Object System.Collections.ArrayList
    function Add-Level2($node) {
        if ($node.level -eq 2) {
            $mapState = [string]$node.mapState
            if ($AllowedMapStates -contains $mapState) {
                [void]$anchors.Add([pscustomobject]@{ name = $node.name; countryId = $node.countryId; mapState = $mapState
                    x = [double]$node.xPosition; y = [double]$node.yPosition })
            }
        }
        $children = if ($null -ne $node.PSObject.Properties['children']) { @($node.children) } else { @() }
        foreach ($child in $children) { Add-Level2 $child }
    }
    foreach ($entry in $country) { foreach ($node in $entry.countrys) { Add-Level2 $node } }
    return $anchors
}

function Get-OwnedLayeredIds {
    param(
        [Parameter(Mandatory = $true)][string]$RegionId,
        [Parameter(Mandatory = $true)][string]$SourceRoot
    )
    $registry = Get-Content -LiteralPath (Join-Path $SourceRoot 'map-regions/regions.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $region = @($registry.regions | Where-Object { $_.id -eq $RegionId })
    if ($region.Count -ne 1) { throw "Region '$RegionId' is not in the registry exactly once." }
    $region = $region[0]
    $frame = [int]$region.frame
    $mapState = [string]$region.mapState

    $layerManifest = Get-Content -LiteralPath (Join-Path $SourceRoot 'map-regions/layers/layers.manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $stateNode = $layerManifest.states.PSObject.Properties[[string]$frame]
    if ($null -eq $stateNode -or -not $stateNode.Value.hasLayers) {
        return [pscustomobject]@{ Owned = @(); Layers = @(); Reason = "frame $frame has no layered maps" }
    }
    $layers = @($stateNode.Value.layers)
    if ($frame -ne 8) {
        # 小世界：整个 frame 就是这一个地区。
        return [pscustomobject]@{ Owned = @($layers | ForEach-Object { [string]$_.id }); Layers = $layers
            Reason = "frame $frame belongs entirely to $RegionId" }
    }

    $allowedMapStates = @($registry.regions | Where-Object { [int]$_.frame -eq 8 } | ForEach-Object { [string]$_.mapState } | Sort-Object -Unique)
    $anchors = Get-LayeredAnchors -SourceRoot $SourceRoot -AllowedMapStates $allowedMapStates

    $owned = New-Object System.Collections.ArrayList
    # The entrance markers, read once: floorId -> point.
    $entrances = @{}
    foreach ($item in (Get-Content -LiteralPath (Join-Path $SourceRoot "Assets/KuroMap/states/state-$frame.json") -Raw -Encoding UTF8 | ConvertFrom-Json)) {
        if ($item.id -ne 'FCRK') { continue }
        foreach ($location in $item.location) {
            $entrances[[string]$location.floorId] = @([double]$location.x, [double]$location.y)
        }
    }
    foreach ($layer in $layers) {
        # Representative point: the entrance marker when the snapshot has one, otherwise the
        # centre of the layer's own tiles. Without the fallback a layer that ships no entrance
        # point (several do) would belong to nobody and never be composited.
        $point = if ($entrances.ContainsKey([string]$layer.id)) { $entrances[[string]$layer.id] } else { $null }
        if ($null -eq $point) {
            $xs = @(); $ys = @()
            foreach ($floor in $layer.floors) { foreach ($image in $floor.tiles) {
                $parts = (($image -split '/')[-1] -replace '\.png$', '').Split('_')
                $xs += [int]$parts[0]; $ys += [int]$parts[1] } }
            if ($xs.Count -eq 0) { continue }
            $tileX = ($xs | Measure-Object -Average).Average
            $tileY = ($ys | Measure-Object -Average).Average
            # Tile centre -> game -> raw. The entrance markers above are raw upstream
            # coordinates, so the fallback has to be in the same space, and the Y axis has to
            # be right: tile Y grows south, game Y grows north. Getting that backwards moved
            # the fallback point to the other hemisphere and handed 冰原's layers to 今州.
            $gameX = ($tileX - 0.5) * 850.0
            $gameY = (0.5 - $tileY) * 850.0
            # Parenthesised on purpose: PowerShell binds `,` tighter than `+`.
            $point = @(($gameX * 100.0 + 2474.0), ($gameY * 100.0 + 1957.0))
        }
        $best = $null; $bestDistance = [double]::MaxValue
        foreach ($anchor in $anchors) {
            $distance = [Math]::Sqrt([Math]::Pow($anchor.x - $point[0], 2) + [Math]::Pow($anchor.y - $point[1], 2))
            if ($distance -lt $bestDistance) { $bestDistance = $distance; $best = $anchor }
        }
        if ($null -ne $best -and $best.mapState -eq $mapState) { [void]$owned.Add([string]$layer.id) }
    }
    return [pscustomobject]@{ Owned = @($owned); Layers = @($layers | Where-Object { $owned -contains [string]$_.id })
        Reason = "$($owned.Count) of $($layers.Count) frame-8 layers nearest to $RegionId" }
}
