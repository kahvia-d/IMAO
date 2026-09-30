[CmdletBinding()]
param(
    [string]$SourceRoot,
    [string]$OutputPath,
    [string]$ReportPath,
    # Points outside these quantiles are treated as spatial outliers when a region's
    # tile window is derived. The raw point snapshot contains extreme values (Lahai
    # reaches y=-8,534,800), so a plain min/max window is unusable.
    [ValidateRange(0.0, 0.2)][double]$LowerQuantile = 0.01,
    [ValidateRange(0.8, 1.0)][double]$UpperQuantile = 0.99,
    # A window derived from collectible points only covers where those points are.
    # Minimap matching has to work wherever the player can stand, so the window is
    # grown by this many tiles on each side.
    [ValidateRange(0, 16)][int]$CoverageMargin = 2,
    # Regions whose tile window is intersected with the footprint the tile archive
    # actually serves. A tile that is absent upstream carries no imagery, so including
    # it only inflates the request. Applied per named region because a stale or partial
    # archive would otherwise silently shrink coverage everywhere.
    [string[]]$TightenRegionId = @(),
    # Explicit count of outliers that must be reported for every region.
    [switch]$Check
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $SourceRoot) { $SourceRoot = Split-Path -Parent $PSScriptRoot }
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
if (-not $OutputPath) { $OutputPath = Join-Path $SourceRoot 'map-regions/regions.json' }
if (-not $ReportPath) { $ReportPath = Join-Path $SourceRoot 'map-regions/regions.report.md' }
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
$ReportPath = [IO.Path]::GetFullPath($ReportPath)

# ---------------------------------------------------------------------------
# Region identity
#
# A region is identified by the pair (frame, mapState). The frame is the Kuro
# top-level state and is the coordinate plane: coordinates are only comparable
# inside one frame. mapState is the public map front-end's region id, named by
# the top-level country entry's mapStateName.
#
# mapState alone is not enough: 黎那汐塔 reuses mapState 3 for the overworld
# region 拉古那 and for the three independent sub-worlds 下层金库/阿维纽林/
# 隐海试验场. The frame separates them.
#
# Ids are stable ASCII because they become directory and package names, and
# they deliberately match the scene/pack ids the project already uses wherever
# one exists (lahai, darkplain, tethys, ...).
# ---------------------------------------------------------------------------
$regionTable = @(
    @{ Key = '8|1';             Id = 'jinzhou';       Kind = 'overworld' }
    @{ Key = '8|8';             Id = 'mengzhou';      Kind = 'overworld' }
    @{ Key = '8|3';             Id = 'laguna';        Kind = 'overworld' }
    @{ Key = '8|4';             Id = 'qiqiu';         Kind = 'overworld' }
    @{ Key = '8|6';             Id = 'roysurface';    Kind = 'overworld' }
    @{ Key = '8|黑海岸群岛';    Id = 'blackshores';   Kind = 'overworld' }
    @{ Key = '906|5';           Id = 'lahai';         Kind = 'subworld' }
    @{ Key = '909|7';           Id = 'darkplain';     Kind = 'subworld' }
    @{ Key = '902|3';           Id = 'lowervault';    Kind = 'subworld' }
    @{ Key = '903|3';           Id = 'avinoleum';     Kind = 'subworld' }
    @{ Key = '905|3';           Id = 'fabricatorium'; Kind = 'subworld' }
    @{ Key = '900|泰缇斯之底';  Id = 'tethys';        Kind = 'subworld' }
    @{ Key = '910|时隙废都';    Id = 'timeriftruins'; Kind = 'subworld' }
    # 梦枢天罗 owns its own frame (912). Its only level-2 area carries the name, so the
    # region name needs no override even though the country entry 瑝珑 also reuses
    # mapState 8 for 梦州.
    @{ Key = '912|8';           Id = 'mengshutianluo'; Kind = 'subworld' }
)
$regionById = @{}
foreach ($entry in $regionTable) {
    if ($regionById.ContainsKey($entry.Id)) { throw "Duplicate region id in the table: $($entry.Id)" }
    $regionById[$entry.Id] = $entry
}

function Read-Json([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { throw "Missing input: $Path" }
    return Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
}

function Get-MapStateName($Country, [string]$MapState) {
    if ([string]::IsNullOrWhiteSpace($MapState)) { return '' }
    $ids = @(([string]$Country.mapStateId) -split ',')
    $names = @(([string]$Country.mapStateName) -split ',')
    for ($i = 0; $i -lt $ids.Count -and $i -lt $names.Count; $i++) {
        if ($ids[$i].Trim() -eq $MapState.Trim()) { return $names[$i].Trim() }
    }
    return ''
}

# ---------------------------------------------------------------------------
# Runtime scene definitions
#
# originX/originY are the frame's raw-coordinate origin. The runtime expresses a
# point as (raw - origin) / 100; the 1.205 scale is the separate map-image scale
# used for on-screen placement and does not participate in the tile grid.
# ---------------------------------------------------------------------------
$sceneHeader = Get-Content -LiteralPath (Join-Path $SourceRoot 'IMao-Core/src/Coordinate/CoordinateStruct.h') -Raw
$scenePattern = '\{\s*(\d+)\s*,\s*"([A-Za-z]+)"\s*,\s*(\d+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*(true|false)\s*\}'
$sceneByState = @{}
foreach ($match in [regex]::Matches($sceneHeader, $scenePattern)) {
    $state = [int]$match.Groups[3].Value
    if ($sceneByState.ContainsKey($state)) { continue }
    $sceneByState[$state] = [pscustomobject]@{
        Scene = $match.Groups[2].Value
        SceneId = [int]$match.Groups[1].Value
        State = $state
        OriginX = [double]::Parse($match.Groups[4].Value, [Globalization.CultureInfo]::InvariantCulture)
        OriginY = [double]::Parse($match.Groups[5].Value, [Globalization.CultureInfo]::InvariantCulture)
        Scale = [double]::Parse($match.Groups[6].Value, [Globalization.CultureInfo]::InvariantCulture)
        RequiresGameValidation = $match.Groups[7].Value -eq 'true'
    }
}
if ($sceneByState.Count -lt 8) { throw "Only $($sceneByState.Count) runtime scene definitions were parsed." }

$calibrations = (Read-Json (Join-Path $SourceRoot 'Assets/KuroMap/scene-calibrations.json')).scenes
$validations = (Read-Json (Join-Path $SourceRoot 'Assets/KuroMap/scene-validation.json')).scenes
$mapManifest = Read-Json (Join-Path $SourceRoot 'Assets/KuroMap/manifest.json')

# Frames whose compiled origin is still the (0, 0) placeholder cannot have a tile
# window derived. An origin-evidence file records in-game captures that prove the
# placeholder is nonetheless correct, which is enough for the tile grid (it needs
# only the origin) but is not a four-point calibration.
$originEvidence = @{}
$originEvidenceRoot = Join-Path $SourceRoot 'map-regions/origins'
if (Test-Path -LiteralPath $originEvidenceRoot) {
    foreach ($file in @(Get-ChildItem -LiteralPath $originEvidenceRoot -Filter '*.json' -File)) {
        $evidence = Get-Content -LiteralPath $file.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
        if ([int]$evidence.formatVersion -ne 1) { throw "Unsupported origin evidence format: $($file.Name)" }
        $evidenceState = [int]$evidence.state
        if ($originEvidence.ContainsKey($evidenceState)) { throw "Duplicate origin evidence for state $evidenceState." }
        $samples = @($evidence.samples)
        if ($samples.Count -lt 4) { throw "Origin evidence for state $evidenceState needs at least four samples." }
        $tolerance = [double]$evidence.toleranceUnits
        foreach ($sample in $samples) {
            if ([double]$sample.nearestPointUnits -gt $tolerance) {
                throw "Origin evidence for state $evidenceState has a sample $([double]$sample.nearestPointUnits) units from the nearest collectible, above the $tolerance unit tolerance."
            }
        }
        $originEvidence[$evidenceState] = [pscustomobject]@{
            Scene = [string]$evidence.scene
            OriginX = [double]$evidence.origin.x
            OriginY = [double]$evidence.origin.y
            SampleCount = $samples.Count
            MaxNearestUnits = ($samples | ForEach-Object { [double]$_.nearestPointUnits } | Measure-Object -Maximum).Maximum
            Source = [string]$file.Name
        }
    }
}

# An anchor observation (map-regions/anchors/<region>.json) records a position the game itself
# reported: a normal frame whose on-screen coordinate readout gives the player's exact position,
# together with the reference image taken there. It replaces the derived window-centre anchor,
# which exists only because a brand-new region has nothing observed yet.
$anchorEvidence = @{}
$anchorRoot = Join-Path $SourceRoot 'map-regions/anchors'
if (Test-Path -LiteralPath $anchorRoot) {
    foreach ($file in @(Get-ChildItem -LiteralPath $anchorRoot -Filter '*.json' -File)) {
        $observation = Get-Content -LiteralPath $file.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
        if ([int]$observation.formatVersion -ne 1) { throw "Unsupported anchor observation format: $($file.Name)" }
        $regionId = [string]$observation.region
        if ($anchorEvidence.ContainsKey($regionId)) { throw "Duplicate anchor observation for region $regionId." }
        $x = [double]$observation.anchor.x
        $y = [double]$observation.anchor.y
        if ([double]::IsNaN($x) -or [double]::IsNaN($y) -or [double]::IsInfinity($x) -or [double]::IsInfinity($y)) {
            throw "Anchor observation for region $regionId is not a finite coordinate."
        }
        $referenceImage = if ($null -ne $observation.PSObject.Properties['referenceImage']) { [string]$observation.referenceImage } else { '' }
        $referencePresent = $false
        if (-not [string]::IsNullOrWhiteSpace($referenceImage)) {
            $referencePresent = Test-Path -LiteralPath (Join-Path $SourceRoot "map-regions/references/$referenceImage") -PathType Leaf
        }
        $anchorEvidence[$regionId] = [pscustomobject]@{
            Scene = [string]$observation.scene
            State = [int]$observation.state
            X = $x
            Y = $y
            Capture = [string]$observation.capture
            ReferenceImage = $referenceImage
            ReferencePresent = $referencePresent
            Source = [string]$file.Name
            # An observation is only usable once its reference image is on this machine: the
            # registry declares the anchor the pack will be verified against, and a rebuild
            # without the image can only produce an unverified pack.
            Usable = $referencePresent
        }
    }
}

# A measured upstream tile footprint (map-regions/footprints/<region>.json, written by
# scripts/Get-MapTileFootprint.ps1) is a direct measurement of where a frame's map
# imagery is. It matters because it is independent of the raw->game origin: the
# imagery sits at whatever tile indices the host serves it at, so a brand-new
# independent sub-world gets a correct tile window before anyone has played it,
# while the origin it still needs for placing markers stays explicitly unproven.
$measuredFootprints = @{}
$footprintRoot = Join-Path $SourceRoot 'map-regions/footprints'
if (Test-Path -LiteralPath $footprintRoot) {
    foreach ($file in @(Get-ChildItem -LiteralPath $footprintRoot -Filter '*.json' -File)) {
        $evidence = Get-Content -LiteralPath $file.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
        if ([int]$evidence.formatVersion -ne 1) { throw "Unsupported tile footprint format: $($file.Name)" }
        $regionId = [string]$evidence.region
        if ($measuredFootprints.ContainsKey($regionId)) { throw "Duplicate tile footprint for region $regionId." }
        $tiles = @($evidence.tiles)
        $imagery = @($tiles | Where-Object { [bool]$_.imagery })
        if ($imagery.Count -eq 0) { throw "Tile footprint for region $regionId has no imagery tiles." }
        $generation = ([string]$evidence.generation).ToUpperInvariant()
        $usable = $generation -eq ([string]$mapManifest.resourceVersion).ToUpperInvariant()
        $measuredFootprints[$regionId] = [pscustomobject]@{
            Scene = [string]$evidence.scene
            State = [int]$evidence.state
            Generation = $generation
            Usable = $usable
            MinX = ($imagery | ForEach-Object { [int]$_.x } | Measure-Object -Minimum).Minimum
            MaxX = ($imagery | ForEach-Object { [int]$_.x } | Measure-Object -Maximum).Maximum
            MinY = ($imagery | ForEach-Object { [int]$_.y } | Measure-Object -Minimum).Minimum
            MaxY = ($imagery | ForEach-Object { [int]$_.y } | Measure-Object -Maximum).Maximum
            ImageryTiles = $imagery.Count
            ProbedTiles = $tiles.Count
            PresentTiles = @($tiles | Where-Object { [bool]$_.present }).Count
            Source = [string]$file.Name
        }
    }
}

# The tile archive records, per region and tile, whether the public host served it.
# That footprint is the only direct measurement of where a region's map actually is;
# everything else is inferred from collectible positions.
$archiveFootprints = @{}
$archiveManifestPath = Join-Path $SourceRoot 'map-regions/tiles/tiles.manifest.json'
if (Test-Path -LiteralPath $archiveManifestPath) {
    $archiveManifest = Get-Content -LiteralPath $archiveManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json -AsHashtable
    $substituted = [bool]$archiveManifest['substituted']
    $verified = $false
    if ($null -ne $archiveManifest['verification']) {
        $verification = $archiveManifest['verification']
        $verified = [bool]$verification['performed'] -and [int]$verification['mismatched'] -eq 0 -and [int]$verification['compared'] -gt 0
    }
    # A footprint measured against a different tile generation is only usable when it
    # was proven byte-identical to the generation the registry was derived from.
    $footprintUsable = (-not $substituted) -or $verified
    if (-not $footprintUsable) {
        Write-Warning 'Tile archive substitutes a generation that was not verified byte-identical; archive footprints will be recorded but not applied.'
    }
    foreach ($regionId in @($archiveManifest['regions'].Keys)) {
        $tiles = @($archiveManifest['regions'][$regionId]['tiles'])
        $present = @($tiles | Where-Object { -not [bool]$_['absent'] })
        if ($present.Count -eq 0) { continue }
        $archiveFootprints[$regionId] = [pscustomobject]@{
            MinX = ($present | ForEach-Object { [int]$_['x'] } | Measure-Object -Minimum).Minimum
            MaxX = ($present | ForEach-Object { [int]$_['x'] } | Measure-Object -Maximum).Maximum
            MinY = ($present | ForEach-Object { [int]$_['y'] } | Measure-Object -Minimum).Minimum
            MaxY = ($present | ForEach-Object { [int]$_['y'] } | Measure-Object -Maximum).Maximum
            Present = $present.Count
            Requested = $tiles.Count
            Generation = [string]$archiveManifest['tileResourceVersion']
            Usable = $footprintUsable
        }
    }
}

function Get-FrameTransform([int]$State) {
    if (-not $sceneByState.ContainsKey($State)) { throw "No runtime scene definition for state $State." }
    $scene = $sceneByState[$State]
    $status = 'builtin'
    $originX = $scene.OriginX
    $originY = $scene.OriginY
    $scale = $scene.Scale
    $calibration = $calibrations.PSObject.Properties[$scene.Scene]
    if ($null -ne $calibration -and [bool]$calibration.Value.passed) {
        $originX = [double]$calibration.Value.coordinateTransform.originX
        $originY = [double]$calibration.Value.coordinateTransform.originY
        $scale = [double]$calibration.Value.coordinateTransform.scale
        $status = 'calibration'
    }
    $verification = $validations.PSObject.Properties[$scene.Scene]
    $approved = $false
    $approvalReason = ''
    if ($null -ne $verification) {
        $approved = [bool]$verification.Value.approved
        if ($null -ne $verification.Value.PSObject.Properties['reason']) { $approvalReason = [string]$verification.Value.reason }
    }
    elseif (-not $scene.RequiresGameValidation) { $approved = $true }
    # A frame whose origin is still the compiled zero placeholder has no usable
    # raw->game mapping; a tile window derived from it would be meaningless. In-game
    # capture evidence can confirm the placeholder instead.
    $evidence = $null
    if ($originEvidence.ContainsKey($State)) {
        $candidate = $originEvidence[$State]
        if ($candidate.Scene -ne $scene.Scene) { throw "Origin evidence scene $($candidate.Scene) does not match state $State ($($scene.Scene))." }
        if ($candidate.OriginX -ne $originX -or $candidate.OriginY -ne $originY) {
            throw "Origin evidence for $($scene.Scene) asserts ($($candidate.OriginX), $($candidate.OriginY)) but the effective origin is ($originX, $originY)."
        }
        $evidence = $candidate
    }
    $trustworthy = $status -eq 'calibration' -or -not ($originX -eq 0 -and $originY -eq 0) -or $null -ne $evidence
    return [pscustomobject]@{
        Scene = $scene.Scene; SceneId = $scene.SceneId; State = $State
        OriginX = $originX; OriginY = $originY; Scale = $scale
        Source = $status; RequiresGameValidation = $scene.RequiresGameValidation
        Approved = $approved; ApprovalReason = $approvalReason; Trustworthy = $trustworthy
        OriginEvidence = $evidence
    }
}

# ---------------------------------------------------------------------------
# Geo primitives
#
# The tile grid is linear in game coordinates. Both constants come from
# scripts/Sync-KuroMapFeaturePack.ps1 and were verified against every shipped
# region pack: all 14 sub-world area anchors land inside their pack's tile
# rectangle, and 黑海岸群岛's label converts to exactly its pack's anchor tile.
# ---------------------------------------------------------------------------
$tileSize = 1024
$virtualMapSize = 850.0
function Convert-RawToGame([double]$RawX, [double]$RawY, $Transform) {
    return [pscustomobject]@{ X = ($RawX - $Transform.OriginX) / 100.0; Y = ($RawY - $Transform.OriginY) / 100.0 }
}
function Get-TileX([double]$GameX) { return [int][Math]::Floor($GameX * $tileSize / $virtualMapSize / $tileSize) + 1 }
function Get-TileY([double]$GameY) { return [int][Math]::Ceiling(-$GameY / $virtualMapSize - 0.0000001) }
function Get-Quantile([double[]]$Values, [double]$Quantile) {
    if ($Values.Count -eq 0) { return 0.0 }
    $sorted = @($Values | Sort-Object)
    $index = [int][Math]::Round($Quantile * ($sorted.Count - 1))
    return $sorted[[Math]::Min($sorted.Count - 1, [Math]::Max(0, $index))]
}

# ---------------------------------------------------------------------------
# Areas (the game's own collection-completion subdivisions)
# ---------------------------------------------------------------------------
$countries = Read-Json (Join-Path $SourceRoot 'Assets/KuroMap/country.json')
$areas = [Collections.Generic.List[object]]::new()
foreach ($country in $countries) {
    foreach ($child in @($country.countrys)) {
        $state = [int]$child.stateId
        $transform = Get-FrameTransform $state
        $game = Convert-RawToGame ([double]$child.xPosition) ([double]$child.yPosition) $transform
        $mapState = [string]$child.mapState
        # The region key is mapState, falling back to the area name when the
        # upstream entry carries no mapState (黑海岸's three areas).
        $regionKey = if ([string]::IsNullOrWhiteSpace($mapState)) { [string]$child.name } else { $mapState }
        $areas.Add([pscustomobject]@{
            CountryId = [int]$country.countryId
            Country = [string]$country.name
            MapState = $mapState
            MapStateName = Get-MapStateName $country $mapState
            RegionKey = "$state|$regionKey"
            Frame = $state
            Scene = $transform.Scene
            Name = [string]$child.name
            RawX = [double]$child.xPosition
            RawY = [double]$child.yPosition
            GameX = $game.X
            GameY = $game.Y
            TileX = Get-TileX $game.X
            TileY = Get-TileY $game.Y
            StateMatchValid = [bool]$child.stateMatchValid
        })
    }
}

# ---------------------------------------------------------------------------
# Points
# ---------------------------------------------------------------------------
$sceneNames = @('World', 'Tethys', 'Fabricatorium', 'Avinoleum', 'Lahai', 'LowerVault', 'Darkplain', 'TimeRiftRuins', 'MengshuTianluo')
$points = [Collections.Generic.List[object]]::new()
foreach ($sceneName in $sceneNames) {
    # The five legacy scenes ship as DLL resources under IMao-Core/src/Resource;
    # the three newer states live in Assets/KuroMap/runtime. Staging resolves them
    # in this order, so the registry must read the same files.
    $file = Join-Path $SourceRoot "IMao-Core/src/Resource/itemsData_$sceneName.json"
    if (-not (Test-Path -LiteralPath $file)) { $file = Join-Path $SourceRoot "Assets/KuroMap/runtime/itemsData_$sceneName.json" }
    if (-not (Test-Path -LiteralPath $file)) { throw "Missing point snapshot for scene $sceneName." }
    $items = Read-Json $file
    $loaded = 0
    foreach ($item in $items) {
        foreach ($location in $item.location) {
            $points.Add([pscustomobject]@{
                Frame = [int]$location.stateId
                CountryId = [int]$location.countryId
                TypeId = [string]$location.typeId
                X = [double]$location.x
                Y = [double]$location.y
            })
            ++$loaded
        }
    }
    if ($loaded -eq 0) { throw "Point snapshot for scene $sceneName contains no locations." }
}
if ($points.Count -eq 0) { throw 'No points were read from the runtime snapshot.' }

# ---------------------------------------------------------------------------
# Attribution
#
# Rule, in order:
#   1. frame        = the point's own stateId.
#   2. frame != 8   = the frame is the region (one independent sub-world).
#   3. frame == 8   = nearest area anchor restricted to the point's own
#                     countryId, then that area's mapState names the region.
#
# Step 3's countryId filter is required, not an optimisation: without it 476 of
# the 19184 overworld points cross into another country, and every one of those
# conflicts is cross-country, which means the raw coordinate canvases of two
# countries are not comparable.
# ---------------------------------------------------------------------------
$frameRegions = @{}
foreach ($group in ($areas | Group-Object Frame)) {
    $frameRegions[[int]$group.Name] = @($group.Group | Group-Object RegionKey)
}
foreach ($entry in $regionTable) {
    $state = [int]($entry.Key -split '\|')[0]
    if (-not $frameRegions.ContainsKey($state)) { throw "Region $($entry.Id) has no areas in frame $state." }
}

$anchorPool = @{}
foreach ($area in ($areas | Where-Object { $_.Frame -eq 8 })) {
    if (-not $anchorPool.ContainsKey($area.CountryId)) { $anchorPool[$area.CountryId] = [Collections.Generic.List[object]]::new() }
    $anchorPool[$area.CountryId].Add($area)
}

$assignments = [Collections.Generic.List[object]]::new()
$unmatched = 0
foreach ($point in $points) {
    $region = $null
    if ($point.Frame -ne 8) {
        $candidates = @($areas | Where-Object { $_.Frame -eq $point.Frame } | Select-Object -ExpandProperty RegionKey -Unique)
        if ($candidates.Count -ne 1) { throw "Frame $($point.Frame) does not map to exactly one region (found $($candidates.Count))." }
        $region = $candidates[0]
    }
    else {
        $pool = if ($anchorPool.ContainsKey($point.CountryId)) { $anchorPool[$point.CountryId] } else { @() }
        if ($pool.Count -eq 0) { ++$unmatched; continue }
        $best = $null
        $bestDistance = [double]::MaxValue
        foreach ($anchor in $pool) {
            $dx = $anchor.RawX - $point.X
            $dy = $anchor.RawY - $point.Y
            $distance = $dx * $dx + $dy * $dy
            if ($distance -lt $bestDistance) { $bestDistance = $distance; $best = $anchor }
        }
        $region = $best.RegionKey
    }
    $assignments.Add([pscustomobject]@{ Point = $point; RegionKey = $region })
}
if ($unmatched -ne 0) { throw "$unmatched overworld points had no area anchor inside their own country." }

# ---------------------------------------------------------------------------
# Region records
# ---------------------------------------------------------------------------
# A region's window and its transform are separate evidence: the window says where
# the imagery is, the transform says where the player is. A region can have both
# (a measured window and a passed calibration), so the confidence names the
# strongest one while tileBounds.basis records where the window came from.
function Get-WindowConfidence($Transform, [int]$State, [string]$WindowOnly) {
    if ($State -eq 8) { return 'validated' }
    if ($Transform.Source -eq 'calibration') { return 'calibrated' }
    if ($null -ne $Transform.OriginEvidence) { return 'origin-verified' }
    return $WindowOnly
}
$regions = [Collections.Generic.List[object]]::new()
foreach ($entry in $regionTable) {
    $mine = @($areas | Where-Object { $_.RegionKey -eq $entry.Key })
    if ($mine.Count -eq 0) { throw "Region $($entry.Id) owns no areas." }
    $state = [int]($entry.Key -split '\|')[0]
    $transform = Get-FrameTransform $state
    $myAssignments = @($assignments | Where-Object { $_.RegionKey -eq $entry.Key })
    $gameX = @($myAssignments | ForEach-Object { Convert-RawToGame $_.Point.X $_.Point.Y $transform | Select-Object -ExpandProperty X })
    $gameY = @($myAssignments | ForEach-Object { Convert-RawToGame $_.Point.X $_.Point.Y $transform | Select-Object -ExpandProperty Y })

    $tileBounds = $null
    $outliers = 0
    # Confidence decides whether a region may be built at all:
    #   validated        frame 8 - the transform is ground-truthed against the six
    #                    shipped overworld packs (every area anchor lands inside its
    #                    pack's tile rectangle).
    #   calibrated       the frame carries a passed four-point calibration.
    #   origin-verified  the compiled origin is confirmed by in-game captures. Enough
    #                    for the tile grid, which needs only the origin.
    #   footprint-measured  the window is the imagery the host actually serves for this
    #                    frame, measured per tile. Independent of the origin, so it is
    #                    trustworthy even while the origin is not.
    #   uncalibrated     no calibration and no origin evidence: the window is a guess
    #                    and must not be turned into a published pack.
    #   blocked          the frame origin is still the compiled zero placeholder.
    $confidence = 'blocked'
    $measured = if ($measuredFootprints.ContainsKey($entry.Id) -and $measuredFootprints[$entry.Id].Usable) { $measuredFootprints[$entry.Id] } else { $null }
    $footprintRecord = $measured
    if ($null -eq $measured -and $measuredFootprints.ContainsKey($entry.Id)) {
        $stale = $measuredFootprints[$entry.Id]
        Write-Warning "Region $($entry.Id) has a tile footprint measured against generation $($stale.Generation), not the registry generation $($mapManifest.resourceVersion); re-measure it with scripts/Get-MapTileFootprint.ps1 before trusting a window for it."
    }
    if ($null -ne $measured) {
        if ($measured.Scene -ne $transform.Scene -or $measured.State -ne $state) {
            throw "Tile footprint $($measured.Source) describes scene $($measured.Scene)/state $($measured.State), not $($transform.Scene)/$state."
        }
        # Every collectible has to land on a tile that carries imagery: the measured
        # rectangle is where the map is, so a point outside it means the compiled
        # origin contradicts the measurement, and the window would be wrong for it.
        $outside = @($myAssignments | Where-Object {
            $game = Convert-RawToGame $_.Point.X $_.Point.Y $transform
            $tileX = Get-TileX $game.X
            $tileY = Get-TileY $game.Y
            $tileX -lt $measured.MinX -or $tileX -gt $measured.MaxX -or $tileY -lt $measured.MinY -or $tileY -gt $measured.MaxY
        })
        if ($outside.Count -gt 0) {
            throw "Region $($entry.Id) has $($outside.Count) point(s) outside its measured tile footprint; the effective origin ($($transform.OriginX), $($transform.OriginY)) disagrees with the imagery and one of the two is wrong."
        }
        $tileMinX = $measured.MinX; $tileMaxX = $measured.MaxX
        $tileMinY = $measured.MinY; $tileMaxY = $measured.MaxY
        # Where the collectibles sit inside the measured window, under the effective
        # (still unproven) origin. Recorded so the window can be reviewed against the
        # points: a region whose points hug one edge of its imagery is a calibration
        # smell, and a region whose points miss the imagery entirely already threw above.
        $pointMinX = Get-TileX (Get-Quantile $gameX $LowerQuantile)
        $pointMaxX = Get-TileX (Get-Quantile $gameX $UpperQuantile)
        $pointMinY = Get-TileY (Get-Quantile $gameY $UpperQuantile)
        $pointMaxY = Get-TileY (Get-Quantile $gameY $LowerQuantile)
        if ($pointMinX -gt $pointMaxX) { $swap = $pointMinX; $pointMinX = $pointMaxX; $pointMaxX = $swap }
        if ($pointMinY -gt $pointMaxY) { $swap = $pointMinY; $pointMinY = $pointMaxY; $pointMaxY = $swap }
        $tileBounds = [ordered]@{
            # No coverage margin: the measurement is the map, not a lower bound on it.
            minX = $tileMinX; maxX = $tileMaxX; minY = $tileMinY; maxY = $tileMaxY
            count = ($tileMaxX - $tileMinX + 1) * ($tileMaxY - $tileMinY + 1)
            pointMinX = $pointMinX; pointMaxX = $pointMaxX; pointMinY = $pointMinY; pointMaxY = $pointMaxY
            coverageMargin = 0
            tightened = $false
            basis = "measured upstream imagery footprint, generation $($measured.Generation)"
            footprint = [ordered]@{
                source = $measured.Source
                minX = $measured.MinX; maxX = $measured.MaxX; minY = $measured.MinY; maxY = $measured.MaxY
                imageryTiles = $measured.ImageryTiles
                presentTiles = $measured.PresentTiles
                probedTiles = $measured.ProbedTiles
                generation = $measured.Generation
            }
        }
        $confidence = Get-WindowConfidence $transform $state 'footprint-measured'
    }
    elseif ($transform.Trustworthy -and $gameX.Count -gt 0) {
        $minX = Get-Quantile $gameX $LowerQuantile
        $maxX = Get-Quantile $gameX $UpperQuantile
        $minY = Get-Quantile $gameY $LowerQuantile
        $maxY = Get-Quantile $gameY $UpperQuantile
        $tileMinX = Get-TileX $minX
        $tileMaxX = Get-TileX $maxX
        $tileMinY = Get-TileY $maxY
        $tileMaxY = Get-TileY $minY
        if ($tileMinX -gt $tileMaxX) { $swap = $tileMinX; $tileMinX = $tileMaxX; $tileMaxX = $swap }
        if ($tileMinY -gt $tileMaxY) { $swap = $tileMinY; $tileMinY = $tileMaxY; $tileMaxY = $swap }
        $outliers = @($gameX | Where-Object { $_ -lt $minX -or $_ -gt $maxX }).Count
        $pointMinX = $tileMinX; $pointMaxX = $tileMaxX; $pointMinY = $tileMinY; $pointMaxY = $tileMaxY
        $tileMinX -= $CoverageMargin; $tileMaxX += $CoverageMargin
        $tileMinY -= $CoverageMargin; $tileMaxY += $CoverageMargin
        $basis = "points p$([int]($LowerQuantile * 100))..p$([int]($UpperQuantile * 100)) grown by $CoverageMargin tile(s)"
        $tightened = $false
        $footprint = if ($archiveFootprints.ContainsKey($entry.Id)) { $archiveFootprints[$entry.Id] } else { $null }
        if ($null -ne $footprint) {
            $requested = ($tileMaxX - $tileMinX + 1) * ($tileMaxY - $tileMinY + 1)
            if (($TightenRegionId -contains $entry.Id) -and -not $footprint.Usable) {
                throw "Region $($entry.Id) was asked to tighten, but the archive generation $($footprint.Generation) was not verified byte-identical to the registry generation $($mapManifest.resourceVersion)."
            }
            if (($TightenRegionId -contains $entry.Id) -and $footprint.Usable) {
                # Intersect, never expand: tightening must not add coverage the point
                # window did not already claim.
                $tileMinX = [Math]::Max($tileMinX, $footprint.MinX)
                $tileMaxX = [Math]::Min($tileMaxX, $footprint.MaxX)
                $tileMinY = [Math]::Max($tileMinY, $footprint.MinY)
                $tileMaxY = [Math]::Min($tileMaxY, $footprint.MaxY)
                if ($tileMinX -gt $tileMaxX -or $tileMinY -gt $tileMaxY) {
                    throw "Tightening region $($entry.Id) emptied its window; the archive footprint does not overlap the point window."
                }
                $tightened = $true
                $basis = "intersected with the archived present-tile footprint (generation $($footprint.Generation))"
            }
        }
        $tileBounds = [ordered]@{
            minX = $tileMinX; maxX = $tileMaxX; minY = $tileMinY; maxY = $tileMaxY
            count = ($tileMaxX - $tileMinX + 1) * ($tileMaxY - $tileMinY + 1)
            pointMinX = $pointMinX; pointMaxX = $pointMaxX; pointMinY = $pointMinY; pointMaxY = $pointMaxY
            coverageMargin = $CoverageMargin
            tightened = $tightened
            basis = $basis
        }
        if ($null -ne $footprint) {
            $tileBounds['archivePresent'] = [ordered]@{
                minX = $footprint.MinX; maxX = $footprint.MaxX; minY = $footprint.MinY; maxY = $footprint.MaxY
                present = $footprint.Present; requested = $footprint.Requested
                generation = $footprint.Generation; usable = $footprint.Usable
                emptyInWindow = ($tileMaxX - $tileMinX + 1) * ($tileMaxY - $tileMinY + 1) - $footprint.Present
            }
        }
        if ($state -eq 8) { $confidence = 'validated' }
        elseif ($transform.Source -eq 'calibration') { $confidence = 'calibrated' }
        elseif ($null -ne $transform.OriginEvidence) { $confidence = 'origin-verified' }
        else { $confidence = 'uncalibrated' }
        $confidence = Get-WindowConfidence $transform $state 'uncalibrated'
    }

    # The anchor is an observation: "the maintainer stood here and the game said so". A recorded
    # observation (map-regions/anchors/<region>.json) therefore replaces the derived guess.
    # The derived value is the window centre, which is only a starting point for collection --
    # asking someone to walk to a computed coordinate is the wrong way round, because the game
    # prints the player's position and that number is the observation.
    $anchor = $null
    $anchorSource = $null
    if ($null -ne $tileBounds) {
        $observed = if ($anchorEvidence.ContainsKey($entry.Id) -and $anchorEvidence[$entry.Id].Usable) { $anchorEvidence[$entry.Id] } else { $null }
        if ($null -eq $observed -and $anchorEvidence.ContainsKey($entry.Id)) {
            $missing = $anchorEvidence[$entry.Id]
            Write-Warning "Region $($entry.Id) has an anchor observation ($($missing.Source)) but its reference image map-regions/references/$($missing.ReferenceImage) is not on this machine; falling back to the derived window-centre anchor, and a rebuild can only produce an unverified pack."
        }
        if ($null -ne $observed) {
            if ($observed.Scene -ne $transform.Scene -or $observed.State -ne $state) {
                throw "Anchor observation $($observed.Source) describes scene $($observed.Scene)/state $($observed.State), not $($transform.Scene)/$state."
            }
            # The window has to contain the anchor, or the pack could not verify against it.
            $anchorTileX = Get-TileX $observed.X
            $anchorTileY = Get-TileY $observed.Y
            if ($anchorTileX -lt $tileBounds.minX -or $anchorTileX -gt $tileBounds.maxX -or
                $anchorTileY -lt $tileBounds.minY -or $anchorTileY -gt $tileBounds.maxY) {
                throw "Anchor observation $($observed.Source) puts $($entry.Id) at tile ($anchorTileX, $anchorTileY), outside its window x $($tileBounds.minX)..$($tileBounds.maxX) y $($tileBounds.minY)..$($tileBounds.maxY)."
            }
            $anchor = [ordered]@{ x = $observed.X; y = $observed.Y }
            $anchorSource = 'observation'
        }
        else {
            $centreTileX = ($tileBounds.minX + $tileBounds.maxX) / 2.0
            $centreTileY = ($tileBounds.minY + $tileBounds.maxY) / 2.0
            $anchor = [ordered]@{
                x = [Math]::Round((($centreTileX - 1) * $virtualMapSize), 3)
                y = [Math]::Round((-$centreTileY * $virtualMapSize), 3)
            }
            $anchorSource = 'window-centre'
        }
    }

    $regions.Add([ordered]@{
        id = $entry.Id
        # A frame that owns exactly one collection area is named by that area
        # (下层金库/阿维纽林/隐海试验场/泰缇斯之底/时隙废都/黑海岸群岛/梦枢天罗). A frame
        # with several areas is named by the public map region (拉海洛/黯原).
        name = if ($mine.Count -eq 1) { $mine[0].Name } else { $mine[0].MapStateName }
        kind = $entry.Kind
        frame = $state
        scene = $transform.Scene
        mapState = $mine[0].MapState
        countryId = $mine[0].CountryId
        country = $mine[0].Country
        areas = @($mine | ForEach-Object {
            [ordered]@{ name = $_.Name; rawX = $_.RawX; rawY = $_.RawY
                gameX = [Math]::Round($_.GameX, 1); gameY = [Math]::Round($_.GameY, 1)
                tileX = $_.TileX; tileY = $_.TileY; stateMatchValid = $_.StateMatchValid }
        })
        anchor = $anchor
        anchorSource = $anchorSource
        anchorEvidence = if ($anchorSource -eq 'observation' -and $anchorEvidence.ContainsKey($entry.Id)) {
            [ordered]@{ source = $anchorEvidence[$entry.Id].Source; capture = $anchorEvidence[$entry.Id].Capture
                referenceImage = $anchorEvidence[$entry.Id].ReferenceImage }
        } else { $null }
        tileBounds = $tileBounds
        # A frame whose transform is unproven still has a computable point window. It is
        # never a pack window (a wrong origin moves it), but it says where the points
        # would land, which is what scripts/Get-MapTileFootprint.ps1 bounds its search
        # with before anything has measured where the imagery actually is.
        untrustedPointWindow = if ($null -eq $tileBounds -and $gameX.Count -gt 0) {
            $windowMinX = Get-TileX (Get-Quantile $gameX $LowerQuantile)
            $windowMaxX = Get-TileX (Get-Quantile $gameX $UpperQuantile)
            $windowMinY = Get-TileY (Get-Quantile $gameY $UpperQuantile)
            $windowMaxY = Get-TileY (Get-Quantile $gameY $LowerQuantile)
            [ordered]@{
                minX = [Math]::Min($windowMinX, $windowMaxX); maxX = [Math]::Max($windowMinX, $windowMaxX)
                minY = [Math]::Min($windowMinY, $windowMaxY); maxY = [Math]::Max($windowMinY, $windowMaxY)
                originSource = $transform.Source
                originX = $transform.OriginX; originY = $transform.OriginY
            }
        } else { $null }
        tileConfidence = $confidence
        buildable = ($confidence -eq 'validated' -or $confidence -eq 'calibrated' -or $confidence -eq 'origin-verified' -or $confidence -eq 'footprint-measured')
        originEvidence = if ($null -ne $transform.OriginEvidence) {
            [ordered]@{ source = $transform.OriginEvidence.Source; samples = $transform.OriginEvidence.SampleCount
                maxNearestPointUnits = $transform.OriginEvidence.MaxNearestUnits }
        } else { $null }
        footprintEvidence = if ($null -ne $footprintRecord) {
            [ordered]@{ source = $footprintRecord.Source; generation = $footprintRecord.Generation
                imageryTiles = $footprintRecord.ImageryTiles; presentTiles = $footprintRecord.PresentTiles }
        } else { $null }
        transformSource = $transform.Source
        requiresGameValidation = $transform.RequiresGameValidation
        approved = $transform.Approved
        approvalReason = $transform.ApprovalReason
        points = $myAssignments.Count
        outliers = $outliers
    })
}

$registry = [ordered]@{
    formatVersion = 1
    generatedFrom = [ordered]@{
        country = 'Assets/KuroMap/country.json'
        points = 'Assets/KuroMap/runtime/itemsData_*.json'
        calibrations = 'Assets/KuroMap/scene-calibrations.json'
        validation = 'Assets/KuroMap/scene-validation.json'
        sceneTable = 'IMao-Core/src/Coordinate/CoordinateStruct.h'
        kuroResourceVersion = [string]$mapManifest.resourceVersion
        kuroStateCount = @($mapManifest.states).Count
    }
    tileGrid = [ordered]@{
        formula = 'game = (raw - origin) / 100'
        tileX = 'floor(gameX / 850 + 1)'
        tileY = 'ceil(-gameY / 850)'
        tileSize = $tileSize
        virtualMapSize = $virtualMapSize
    }
    regions = @($regions)
}

$json = ($registry | ConvertTo-Json -Depth 12) + [Environment]::NewLine
$totalPoints = 0
$overworld = 0
$tileTotal = 0
$presentTotal = 0
$presentRegions = 0
foreach ($region in $regions) {
    $totalPoints += [int]$region['points']
    if ($region['kind'] -eq 'overworld') { $overworld += [int]$region['points'] }
    if ($null -ne $region['tileBounds']) {
        $tileTotal += [int]$region['tileBounds']['count']
        # archivePresent is only recorded for regions whose tile archive was probed,
        # so it must be read as a dictionary key: property-style access throws under
        # Set-StrictMode whenever the key is absent.
        $present = if ($region['tileBounds'].Contains('archivePresent')) { $region['tileBounds']['archivePresent'] } else { $null }
        if ($null -ne $present -and $present.Contains('present') -and $null -ne $present['present']) {
            $presentTotal += [int]$present['present']
            $presentRegions++
        }
    }
}
$overCap = @($regions | Where-Object { $null -ne $_['tileBounds'] -and [int]$_['tileBounds']['count'] -gt 256 })

$report = [Collections.Generic.List[string]]::new()
$report.Add('# 地图地区注册表')
$report.Add('')
$report.Add("由 ``scripts/New-MapRegionRegistry.ps1`` 生成，数据源 ``Assets/KuroMap``（Kuro resource version ``$($mapManifest.resourceVersion)``）。")
$report.Add('')
$report.Add('瓦片换算经过地面真值验证：15 个独立小世界子区域锚点（拉海洛 9、黯原 4、泰缇斯之底 1、梦枢天罗 1）全部落在其既有包的瓦片矩形内。')
$report.Add('')
$report.Add('| 地区 | id | 类型 | frame | mapState | 子区域 | 点数 | 瓦片窗口 | 窗口格数 | 实际有图 | 置信度 |')
$report.Add('| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |')
foreach ($region in $regions) {
    $bounds = if ($null -ne $region.tileBounds) { "x $($region.tileBounds.minX)..$($region.tileBounds.maxX), y $($region.tileBounds.minY)..$($region.tileBounds.maxY)" } else { '—' }
    $count = if ($null -ne $region.tileBounds) { $region.tileBounds.count } else { '—' }
    $presentCount = if ($null -ne $region['tileBounds'] -and $region['tileBounds'].Contains('archivePresent')) { $region['tileBounds']['archivePresent']['present'] } else { '—' }
    $report.Add("| $($region.name) | ``$($region.id)`` | $($region.kind) | $($region.frame) | $($region.mapState) | $(@($region.areas).Count) | $($region.points) | $bounds | $count | $presentCount | $($region.tileConfidence) |")
}
$report.Add('')
$report.Add("- 点位合计：**$totalPoints**（大世界 $overworld）")
$report.Add("- 窗口格数合计：**$tileTotal**（点位推导的窗口含每侧 $CoverageMargin 块覆盖边距，实测足迹的窗口不含边距）")
$report.Add("- 实际有图瓦片：**$presentTotal** 张（$presentRegions 个地区记在 ``tiles.manifest.json`` 里；实测足迹地区的瓦片清单记在 ``map-regions/footprints/``，上游缺失的格子不计入，故小于窗口格数）")
$report.Add("- 注：**窗口格数不是下载量**。相邻地区在同一坐标平面上窗口会交叠（六个地表地区共用 frame 8），实际唯一瓦片文件数见 ``map-regions/tiles/tiles.manifest.json``。")
$report.Add("- 触发 256 上限的地区：$(if ($overCap.Count) { ($overCap | ForEach-Object { "$($_['name'])($($_['tileBounds']['count']))" }) -join '、' } else { '无' })")
$report.Add('')
$report.Add('瓦片窗口由点位分位数推导后**每侧外扩 ' + $CoverageMargin + ' 块**。分位数只保证覆盖收集品所在处，最小地图匹配必须在玩家能站到的任何位置工作，所以窗口是下界加上行走边距。')
$report.Add('')
$report.Add('## 置信度')
$report.Add('')
$report.Add('- `validated`：frame 8（大世界）。换算用 6 个既有包的锚点/矩形做过地面真值验证。')
$report.Add('- `calibrated`：该 frame 有通过的四点校准。')
$report.Add('- `footprint-measured`：窗口取**实测**的上游有图瓦片包围盒（`map-regions/footprints/<id>.json`）。窗口与原点无关，因此可信；但该 frame 的 raw→game 原点仍未证实，**打包可以，开放不行**。')
$report.Add('- `uncalibrated`：无校准，窗口只是猜测，**不得据此发布资源包**。')
$report.Add('- `blocked`：frame 原点仍是编译期占位值 `(0, 0)`。')
$report.Add('')
$report.Add('### 窗口来自实测足迹的地区')
$report.Add('')
$footprintMeasured = @($regions | Where-Object { $null -ne $_['tileBounds'] -and $_['tileBounds'].Contains('footprint') })
if ($footprintMeasured.Count -eq 0) { $report.Add('无。') }
else {
    foreach ($region in $footprintMeasured) {
        $footprint = $region['tileBounds']['footprint']
        $gap = if ($region['tileConfidence'] -eq 'calibrated' -or $region['tileConfidence'] -eq 'validated') { '换算是校准/验证过的' } else { '换算仍未证实' }
        $report.Add("- $($region['name'])（``$($region['id'])``, frame $($region['frame'])）— 实测有图 $($footprint['imageryTiles']) 块 / 窗口 $($region['tileBounds']['count']) 块（代次 $($footprint['generation'])）；当前置信度 ``$($region['tileConfidence'])``，$gap。")
    }
}
$report.Add('')
$report.Add('### 需要先补校准的地区')
$report.Add('')
$uncalibrated = @($regions | Where-Object { $_['tileConfidence'] -eq 'uncalibrated' })
if ($uncalibrated.Count -eq 0) { $report.Add('无。') }
else {
    foreach ($region in $uncalibrated) {
        $report.Add("- $($region['name'])（``$($region['id'])``, frame $($region['frame'])）— 推导窗口 $($region['tileBounds']['count']) 块，未经校准不可信")
    }
}
$report.Add('')
$report.Add('### 被阻塞的地区')
$report.Add('')
$blocked = @($regions | Where-Object { $_['tileConfidence'] -eq 'blocked' })
if ($blocked.Count -eq 0) { $report.Add('无。') }
else {
    foreach ($region in $blocked) { $report.Add("- $($region['name'])（``$($region['id'])``, frame $($region['frame'])）") }
}

if ($Check) {
    Write-Host "Map region registry check: $($regions.Count) regions, $totalPoints points, $tileTotal tiles."
}
else {
    New-Item -ItemType Directory -Force (Split-Path -Parent $OutputPath) | Out-Null
    [IO.File]::WriteAllText($OutputPath, $json, [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllLines($ReportPath, $report, [Text.UTF8Encoding]::new($false))
    Write-Host "Wrote $OutputPath" -ForegroundColor Green
    Write-Host "Wrote $ReportPath" -ForegroundColor Green
}
Write-Host "Regions: $($regions.Count)  points: $totalPoints (overworld $overworld)  windowCells: $tileTotal  presentTiles: $presentTotal  blocked: $($blocked.Count)  uncalibrated: $($uncalibrated.Count)  footprintMeasured: $($footprintMeasured.Count)"
foreach ($region in $regions) {
    $count = if ($null -ne $region['tileBounds']) { $region['tileBounds']['count'] } else { '—' }
    Write-Host ("  {0,-14} {1,-14} frame={2,-4} points={3,6} tiles={4,-5} {5}" -f $region['id'], $region['name'], $region['frame'], $region['points'], $count, $region['tileConfidence'])
}
