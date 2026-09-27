# Shared classifier for "is this layered map open to the surface, or is it enclosed?".
#
# Why this exists
# ---------------
# A layered map is either a place the player walks into from the surface, or a place the surface
# map is not. Until 2026-09-27 the runtime assumed the second for every layered map, so standing on
# any floor hid every marker that carries no floor of its own - which is correct inside a cave and
# wrong in 星炬学院, an above-ground building whose 广场区 IS the surface: a player reported that
# 终声残卷 never showed there while he was in the academy. See
# Docs/ZscjLayerAttribute_20260927.md for the evidence, and Docs/LayeredMapOpenness_20260927.md for
# the rule the two kinds now follow.
#
#   开放分层地图 (open):   本身不封闭、没有限定的出入口、地表本身也算一层。星炬学院是今天全游戏唯一一个。
#   封闭分层地图 (enclosed): 洞穴 / 地下 / 建筑内部，地表不算一层，出入口有限或者只有一个。
#                          下层金库、眠龙庭、虚妄摇篮、泅森涡道……都属于这一类。
#
# The data never states "is there a wall", so the class is derived from two measurements, and a
# recorded game fact can override either of them:
#
#   1. the floor next to the surface (level -1) draws its ground from the surface's OWN pixels
#      (copiedFraction >= 0.5). An open map is drawn on top of the surface map, so its ground floor
#      is that drawing; 星炬学院·广场区 measures 0.733, while the highest enclosed ground floor that
#      has no entrance is 眶折谷·本段 at 0.16 - a 4.5x margin.
#   2. the layered map has NO 分层入口 (FCRK) marker. 第三/第四日树 copy the surface just as heavily
#      (0.51 / 0.80) but DO have entrances, so they stay enclosed: that second signal is what keeps
#      "looks like the surface" from being the whole rule.
#
# Measured 2026-09-27 over all 57 layered maps / 90 floors of the game: exactly one map comes out
# open (拉海洛 layer 30, all four of its floors), which is what the player saw in the game.
#
# A per-floor flag is what ships (see FloorEntry::openToSurface and Set-LayeredSurfaceAccess.ps1):
# the layer decides by default, and a single floor can be marked differently when a map turns out to
# be mixed (地面开放 + 地面下封闭), which is why the fact tables below exist at both granularities.
#
# Dot-source this file:
#   . (Join-Path $PSScriptRoot 'LayeredSurfaceAccess.ps1')

# Ways the class can be known, in the order they win. Recorded facts beat the derivation, because
# "is this place enclosed" is a fact about the game and the two measurements are only evidence.
$script:SurfaceAccessOpen = 'open'
$script:SurfaceAccessEnclosed = 'enclosed'

# Recorded game facts, layer grain: region + layerId -> open/enclosed. Add a line only after
# confirming it in the game; the derivation below is a proposal, this table is the answer.
$script:LayeredSurfaceAccessLayerFacts = @(
    @{ Region = 'lahai'; LayerId = 30; Name = '星炬学院'; Access = 'open'
       Reason = '露天多层建筑，无限定出入口，广场区就是地表（2026-09-27 用户确认 + 上游无分层入口标记 + copiedFraction 0.733）' }
)

# Recorded game facts, floor grain: region + floorId -> open/enclosed. Empty today: it exists for a
# mixed map (地面开放 + 地面下封闭), where one floor of an otherwise open map is enclosed.
$script:LayeredSurfaceAccessFloorFacts = @(
)

# Which floors the shipped index is expected to mark open, for scripts that verify a shipped tree
# without re-deriving anything (the pack test cannot see the item data). Format: "<region>/<floorId>".
$script:LayeredSurfaceAccessExpectedOpen = @(
    'lahai/-1/30', 'lahai/-2/30', 'lahai/-3/30', 'lahai/-4/30'
)

# The floor next to the surface is the only one that can be the surface itself, and the two
# populations of copiedFraction are far apart (0.733 vs 0.16 among floors without an entrance).
$script:LayeredSurfaceAccessCopiedGate = 0.5

# frame -> the runtime scene whose item data carries that frame's 分层入口 markers. Read from the
# shipped manifest rather than restated, so a new subworld is added in one place.
function Get-LayeredFrameRuntime {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$SourceRoot)

    $manifestPath = Join-Path $SourceRoot 'Assets/KuroMap/manifest.json'
    if (-not (Test-Path -LiteralPath $manifestPath)) { throw "Kuro map manifest is missing: $manifestPath" }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $byFrame = @{}
    foreach ($state in $manifest.states) { $byFrame[[int]$state.state] = [string]$state.runtime }
    return $byFrame
}

function Get-LayeredItemDataPath {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$SourceRoot,
        [Parameter(Mandatory = $true)][string]$Runtime
    )

    $embedded = Join-Path $SourceRoot "IMao-Core/src/Resource/itemsData_$Runtime.json"
    if (Test-Path -LiteralPath $embedded) { return $embedded }
    $staged = Join-Path $SourceRoot "Assets/KuroMap/runtime/itemsData_$Runtime.json"
    if (Test-Path -LiteralPath $staged) { return $staged }
    return $null
}

# Which layered maps of this frame carry a 分层入口 marker upstream. The marker is what says "this
# place is entered at a defined place" - the open kind has none, because there is nothing to enter.
function Get-LayeredMapEntranceLayers {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$SourceRoot,
        [Parameter(Mandatory = $true)][int]$Frame
    )

    $byFrame = Get-LayeredFrameRuntime -SourceRoot $SourceRoot
    if (-not $byFrame.ContainsKey($Frame)) { return @() }
    $path = Get-LayeredItemDataPath -SourceRoot $SourceRoot -Runtime $byFrame[$Frame]
    if ($null -eq $path) { return @() }

    $items = Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
    $layers = [Collections.Generic.HashSet[int]]::new()
    foreach ($group in $items) {
        $name = [string]$group.name
        $id = [string]$group.id
        if ($name -notlike '*分层入口*' -and $id -ne 'FCRK') { continue }
        foreach ($location in @($group.location)) {
            $floor = [string]$location.floorId
            if ([string]::IsNullOrWhiteSpace($floor)) { continue }
            $null = $layers.Add([int]$floor)
        }
    }
    return @($layers)
}

# shared cells / opaque cells over the whole floor, the same quantity the runtime computes in
# LayeredFloors::Load (FloorEntry::copiedFraction) - so a script and the runtime cannot disagree.
function Get-LayeredFloorCopiedFraction {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]$Floor,
        [Parameter(Mandatory = $true)][int]$GridSize
    )

    $bits = $GridSize * $GridSize
    $nibbleValue = {
        param([char]$Nibble)
        if ($Nibble -ge '0' -and $Nibble -le '9') { return [int]$Nibble - [int][char]'0' }
        if ($Nibble -ge 'a' -and $Nibble -le 'f') { return [int]$Nibble - [int][char]'a' + 10 }
        if ($Nibble -ge 'A' -and $Nibble -le 'F') { return [int]$Nibble - [int][char]'A' + 10 }
        return 0
    }
    $occupied = 0
    $shared = 0
    foreach ($tile in @($Floor.tiles)) {
        $occupancy = [string]$tile.occupancy
        if ([string]::IsNullOrEmpty($occupancy) -or $occupancy.Length * 4 -ne $bits) { continue }
        $sharedGrid = [string]$tile.shared
        $hasShared = -not [string]::IsNullOrEmpty($sharedGrid) -and $sharedGrid.Length * 4 -eq $bits
        for ($bit = 0; $bit -lt $bits; ++$bit) {
            if (((& $nibbleValue $occupancy[[int][math]::Floor($bit / 4)]) -shr ($bit % 4)) -band 1) {
                ++$occupied
                if ($hasShared -and (((& $nibbleValue $sharedGrid[[int][math]::Floor($bit / 4)]) -shr ($bit % 4)) -band 1)) {
                    ++$shared
                }
            }
        }
    }
    if ($occupied -eq 0) { return 0.0 }
    return $shared / [double]$occupied
}

function Get-LayeredSurfaceAccessLayerFact {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Region,
        [Parameter(Mandatory = $true)][int]$LayerId
    )

    foreach ($fact in $script:LayeredSurfaceAccessLayerFacts) {
        if ($fact.Region -eq $Region -and [int]$fact.LayerId -eq $LayerId) { return $fact }
    }
    return $null
}

function Get-LayeredSurfaceAccessFloorFact {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Region,
        [Parameter(Mandatory = $true)][string]$FloorId
    )

    foreach ($fact in $script:LayeredSurfaceAccessFloorFacts) {
        if ($fact.Region -eq $Region -and $fact.FloorId -eq $FloorId) { return $fact }
    }
    return $null
}

# The class of one map, from the measurements, before any fact overrides it. `GroundCopiedFraction`
# is the level -1 floor's value; `HasEntrance` says whether upstream gives the map an entrance marker.
function Get-DerivedLayeredSurfaceAccess {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][double]$GroundCopiedFraction,
        [Parameter(Mandatory = $true)][bool]$HasEntrance
    )

    if ($HasEntrance) { return $script:SurfaceAccessEnclosed }
    if ($GroundCopiedFraction -ge $script:LayeredSurfaceAccessCopiedGate) { return $script:SurfaceAccessOpen }
    return $script:SurfaceAccessEnclosed
}

# The class of one floor: floor fact, then layer fact, then the derived class of the whole map.
#
# `LayerGroundCopiedFraction` is the LEVEL -1 floor's value and is the same for every floor of the
# map: openness is a property of the map ("地表本身也算一层"), and the measurement that can see it
# lives on the ground floor. A caller that passes 0 for an upper floor would derive "enclosed" for
# it, which is why the writer script resolves the ground floor once per layer.
function Resolve-LayeredSurfaceAccess {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Region,
        [Parameter(Mandatory = $true)][int]$LayerId,
        [Parameter(Mandatory = $true)][string]$FloorId,
        [Parameter(Mandatory = $true)][double]$LayerGroundCopiedFraction,
        [Parameter(Mandatory = $true)][bool]$HasEntrance
    )

    $floorFact = Get-LayeredSurfaceAccessFloorFact -Region $Region -FloorId $FloorId
    if ($null -ne $floorFact) {
        return [pscustomobject]@{ Access = [string]$floorFact.Access; Source = 'floor-fact'; Reason = [string]$floorFact.Reason }
    }
    $layerFact = Get-LayeredSurfaceAccessLayerFact -Region $Region -LayerId $LayerId
    if ($null -ne $layerFact) {
        return [pscustomobject]@{ Access = [string]$layerFact.Access; Source = 'layer-fact'; Reason = [string]$layerFact.Reason }
    }
    $derived = Get-DerivedLayeredSurfaceAccess -GroundCopiedFraction $LayerGroundCopiedFraction -HasEntrance $HasEntrance
    $entranceText = if ($HasEntrance) { 'has a 分层入口 marker' } else { 'no 分层入口 marker' }
    return [pscustomobject]@{ Access = $derived; Source = 'derived'
        Reason = "ground copiedFraction=$([math]::Round($LayerGroundCopiedFraction, 3)), $entranceText" }
}
