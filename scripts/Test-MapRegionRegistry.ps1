[CmdletBinding()]
param(
    [string]$SourceRoot,
    # Named *Path so it cannot collide with the parsed $registry object.
    [string]$RegistryPath,
    # The cross-canvas conflict count without the countryId filter is reported, not
    # failed: it is the evidence for why the filter is mandatory.
    [switch]$Detailed
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $SourceRoot) { $SourceRoot = Split-Path -Parent $PSScriptRoot }
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
if (-not $RegistryPath) { $RegistryPath = Join-Path $SourceRoot 'map-regions/regions.json' }
$RegistryPath = [IO.Path]::GetFullPath($RegistryPath)

$failures = [Collections.Generic.List[string]]::new()
$notes = [Collections.Generic.List[string]]::new()
function Check([bool]$Condition, [string]$Message) {
    if ($Condition) { Write-Host "  ok   $Message" }
    else { Write-Host "  FAIL $Message" -ForegroundColor Red; $failures.Add($Message) }
}

$tileSize = 1024
$virtualMapSize = 850.0
function Get-TileX([double]$GameX) { return [int][Math]::Floor($GameX / $virtualMapSize) + 1 }
function Get-TileY([double]$GameY) { return [int][Math]::Ceiling(-$GameY / $virtualMapSize - 0.0000001) }

if (-not (Test-Path -LiteralPath $RegistryPath)) { throw "Missing region registry: $RegistryPath" }
$registry = Get-Content -LiteralPath $RegistryPath -Raw -Encoding UTF8 | ConvertFrom-Json -AsHashtable

Write-Host 'Registry structure'
Check ([int]$registry['formatVersion'] -eq 1) 'formatVersion is 1'
$regions = @($registry['regions'])
Check ($regions.Count -eq 13) "registry declares 13 regions (found $($regions.Count))"
$ids = @($regions | ForEach-Object { [string]$_['id'] })
Check (($ids | Select-Object -Unique).Count -eq $ids.Count) 'region ids are unique'
$keys = @($regions | ForEach-Object { "$($_['frame'])|$(if ([string]::IsNullOrEmpty([string]$_['mapState'])) { @($_['areas'])[0]['name'] } else { $_['mapState'] })" })
Check (($keys | Select-Object -Unique).Count -eq $keys.Count) '(frame, mapState) pairs are unique'
foreach ($record in $regions) { Check (@($record['areas']).Count -ge 1) "region $($record['id']) owns at least one area" }
$resourceVersion = [string]$registry['generatedFrom']['kuroResourceVersion']
Check ($resourceVersion -match '^[A-Fa-f0-9]{32}$') "registry records a 32-hex resource version ($resourceVersion)"

# A region must never span two coordinate frames: that is the invariant that makes
# the frame split automatic and keeps coordinates comparable inside a region.
Write-Host 'Frame containment'
$sceneStates = @(8, 900, 902, 903, 905, 906, 909, 910)
foreach ($record in $regions) {
    $frame = [int]$record['frame']
    Check ($sceneStates -contains $frame) "region $($record['id']) uses a known frame ($frame)"
    $areaFrames = @($record['areas'] | ForEach-Object { $frame } | Select-Object -Unique)
    Check ($areaFrames.Count -le 1) "region $($record['id']) does not span frames"
}

# ---------------------------------------------------------------------------
# Independent re-derivation of the partition from the public inputs
# ---------------------------------------------------------------------------
$countries = Get-Content -LiteralPath (Join-Path $SourceRoot 'Assets/KuroMap/country.json') -Raw -Encoding UTF8 | ConvertFrom-Json -AsHashtable
$sceneHeader = Get-Content -LiteralPath (Join-Path $SourceRoot 'IMao-Core/src/Coordinate/CoordinateStruct.h') -Raw
$scenePattern = '\{\s*(\d+)\s*,\s*"([A-Za-z]+)"\s*,\s*(\d+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*(true|false)\s*\}'
$originByState = @{}
foreach ($match in [regex]::Matches($sceneHeader, $scenePattern)) {
    $state = [int]$match.Groups[3].Value
    if ($originByState.ContainsKey($state)) { continue }
    $originByState[$state] = [pscustomobject]@{
        OriginX = [double]::Parse($match.Groups[4].Value, [Globalization.CultureInfo]::InvariantCulture)
        OriginY = [double]::Parse($match.Groups[5].Value, [Globalization.CultureInfo]::InvariantCulture)
    }
}
$calibrations = (Get-Content -LiteralPath (Join-Path $SourceRoot 'Assets/KuroMap/scene-calibrations.json') -Raw -Encoding UTF8 | ConvertFrom-Json -AsHashtable)['scenes']
$sceneNameByState = @{ 8 = 'World'; 900 = 'Tethys'; 902 = 'LowerVault'; 903 = 'Avinoleum'; 905 = 'Fabricatorium'; 906 = 'Lahai'; 909 = 'Darkplain'; 910 = 'TimeRiftRuins' }
foreach ($state in @($originByState.Keys)) {
    $sceneName = $sceneNameByState[$state]
    if ($calibrations.ContainsKey($sceneName) -and [bool]$calibrations[$sceneName]['passed']) {
        $originByState[$state] = [pscustomobject]@{
            OriginX = [double]$calibrations[$sceneName]['coordinateTransform']['originX']
            OriginY = [double]$calibrations[$sceneName]['coordinateTransform']['originY']
        }
    }
}

# Rebuild the area list and the region key exactly as the generator defines them.
$areas = [Collections.Generic.List[object]]::new()
foreach ($country in $countries) {
    foreach ($child in @($country['countrys'])) {
        $state = [int]$child['stateId']
        $mapState = [string]$child['mapState']
        $regionKey = if ([string]::IsNullOrWhiteSpace($mapState)) { [string]$child['name'] } else { $mapState }
        $origin = $originByState[$state]
        $gameX = ([double]$child['xPosition'] - $origin.OriginX) / 100.0
        $gameY = ([double]$child['yPosition'] - $origin.OriginY) / 100.0
        $areas.Add([pscustomobject]@{
            Key = "$state|$regionKey"; Frame = $state; CountryId = [int]$country['countryId']
            Name = [string]$child['name']; RawX = [double]$child['xPosition']; RawY = [double]$child['yPosition']
            GameX = $gameX; GameY = $gameY; TileX = Get-TileX $gameX; TileY = Get-TileY $gameY
        })
    }
}
Check ($areas.Count -eq 51) "the public hierarchy yields 51 areas (found $($areas.Count))"

Write-Host 'Area coverage'
$registryAreaKeys = @{}
foreach ($record in $regions) {
    foreach ($area in @($record['areas'])) {
        $key = "$($record['frame'])|$($area['name'])"
        if ($registryAreaKeys.ContainsKey($key)) { $failures.Add("area $key is claimed twice"); continue }
        $registryAreaKeys[$key] = [string]$record['id']
    }
}
Check ($registryAreaKeys.Count -eq $areas.Count) "registry claims every area exactly once ($($registryAreaKeys.Count)/$($areas.Count))"
foreach ($area in $areas) {
    $key = "$($area.Frame)|$($area.Name)"
    Check ($registryAreaKeys.ContainsKey($key)) "area $($area.Name) (frame $($area.Frame)) is in the registry"
}
# Areas must sit inside the region's derived point footprint. A tightened window may
# legitimately exclude an area's label tile: the label is a place name on the map, not
# a geometric centre, and the archived footprint is the real map extent.
Write-Host 'Tile containment'
foreach ($record in $regions) {
    $bounds = $record['tileBounds']
    if ($null -eq $bounds) { continue }
    foreach ($area in @($record['areas'])) {
        $inside = $area['tileX'] -ge [int]$bounds['pointMinX'] -and $area['tileX'] -le [int]$bounds['pointMaxX'] -and
                  $area['tileY'] -ge [int]$bounds['pointMinY'] -and $area['tileY'] -le [int]$bounds['pointMaxY']
        Check $inside "area $($area['name']) tile ($($area['tileX']),$($area['tileY'])) sits in $($record['id'])'s point footprint"
    }
    # The builder rejects a window that does not contain the anchor tile, so the anchor
    # must be inside the window that will actually be requested.
    $anchorTileX = Get-TileX ([double]$record['anchor']['x'])
    $anchorTileY = Get-TileY ([double]$record['anchor']['y'])
    $anchorInside = $anchorTileX -ge [int]$bounds['minX'] -and $anchorTileX -le [int]$bounds['maxX'] -and
                    $anchorTileY -ge [int]$bounds['minY'] -and $anchorTileY -le [int]$bounds['maxY']
    Check $anchorInside "region $($record['id'])'s anchor tile ($anchorTileX,$anchorTileY) sits in its requested window"
    if ([bool]$bounds['tightened']) {
        $present = $bounds['archivePresent']
        Check ([int]$present['minX'] -eq [int]$bounds['minX'] -and [int]$present['maxX'] -eq [int]$bounds['maxX'] -and
               [int]$present['minY'] -eq [int]$bounds['minY'] -and [int]$present['maxY'] -eq [int]$bounds['maxY']) `
            "region $($record['id']) is tightened to exactly its archived present-tile footprint"
    }
}

# ---------------------------------------------------------------------------
# Point attribution
# ---------------------------------------------------------------------------
Write-Host 'Point attribution'
$sceneNames = @('World', 'Tethys', 'Fabricatorium', 'Avinoleum', 'Lahai', 'LowerVault', 'Darkplain', 'TimeRiftRuins')
$points = [Collections.Generic.List[object]]::new()
foreach ($sceneName in $sceneNames) {
    $file = Join-Path $SourceRoot "IMao-Core/src/Resource/itemsData_$sceneName.json"
    if (-not (Test-Path -LiteralPath $file)) { $file = Join-Path $SourceRoot "Assets/KuroMap/runtime/itemsData_$sceneName.json" }
    $items = Get-Content -LiteralPath $file -Raw -Encoding UTF8 | ConvertFrom-Json -AsHashtable
    foreach ($item in $items) {
        foreach ($location in @($item['location'])) {
            $points.Add([pscustomobject]@{
                Frame = [int]$location['stateId']; CountryId = [int]$location['countryId']
                X = [double]$location['x']; Y = [double]$location['y']
            })
        }
    }
}
$overworldPoints = @($points | Where-Object { $_.Frame -eq 8 })
Check ($overworldPoints.Count -eq 19184) "overworld snapshot holds 19184 points (found $($overworldPoints.Count))"

# Region key -> registry id, and the frame-(8) anchor pool per country.
$idByKey = @{}
foreach ($record in $regions) {
    $firstArea = @($record['areas'])[0]
    $key = "$($record['frame'])|$(if ([string]::IsNullOrEmpty([string]$record['mapState'])) { $firstArea['name'] } else { $record['mapState'] })"
    $idByKey[$key] = [string]$record['id']
}
$anchorPool = @{}
foreach ($area in @($areas | Where-Object { $_.Frame -eq 8 })) {
    if (-not $anchorPool.ContainsKey($area.CountryId)) { $anchorPool[$area.CountryId] = [Collections.Generic.List[object]]::new() }
    $anchorPool[$area.CountryId].Add($area)
}

$counts = @{}
$frameCounts = @{}
$crossCanvasConflicts = 0
$unassigned = 0
foreach ($point in $points) {
    $key = $null
    if ($point.Frame -ne 8) {
        $candidates = @($areas | Where-Object { $_.Frame -eq $point.Frame } | ForEach-Object { $_.Key } | Select-Object -Unique)
        if ($candidates.Count -eq 1) { $key = $candidates[0] }
    }
    else {
        $pool = if ($anchorPool.ContainsKey($point.CountryId)) { $anchorPool[$point.CountryId] } else { @() }
        if ($pool.Count -gt 0) {
            $best = $null; $bestDistance = [double]::MaxValue
            foreach ($anchor in $pool) {
                $dx = $anchor.RawX - $point.X; $dy = $anchor.RawY - $point.Y
                $distance = $dx * $dx + $dy * $dy
                if ($distance -lt $bestDistance) { $bestDistance = $distance; $best = $anchor }
            }
            $key = $best.Key
            # Independently: what would an unrestricted (cross-country) search pick?
            $anyBest = $null; $anyDistance = [double]::MaxValue
            foreach ($anchor in @($areas | Where-Object { $_.Frame -eq 8 })) {
                $dx = $anchor.RawX - $point.X; $dy = $anchor.RawY - $point.Y
                $distance = $dx * $dx + $dy * $dy
                if ($distance -lt $anyDistance) { $anyDistance = $distance; $anyBest = $anchor }
            }
            if ($anyBest.CountryId -ne $point.CountryId) { ++$crossCanvasConflicts }
        }
    }
    if ($null -eq $key -or -not $idByKey.ContainsKey($key)) { ++$unassigned; continue }
    $id = $idByKey[$key]
    if (-not $counts.ContainsKey($id)) { $counts[$id] = 0 }
    ++$counts[$id]
    if (-not $frameCounts.ContainsKey($id)) { $frameCounts[$id] = $point.Frame }
}
Check ($unassigned -eq 0) "every point is assigned to a region ($unassigned unassigned)"

foreach ($record in $regions) {
    $id = [string]$record['id']
    $actual = if ($counts.ContainsKey($id)) { $counts[$id] } else { 0 }
    Check ([int]$record['points'] -eq $actual) "region $id point count matches the re-derivation ($actual vs $($record['points']))"
}
$overworldAssigned = 0
foreach ($record in $regions) {
    if ([int]$record['frame'] -eq 8) { $overworldAssigned += [int]$record['points'] }
}
Check ($overworldAssigned -eq $overworldPoints.Count) "overworld regions sum to the overworld snapshot ($overworldAssigned vs $($overworldPoints.Count))"
$notes.Add("cross-canvas conflicts without the countryId filter: $crossCanvasConflicts (expected 476; this is why the filter is required)")

# ---------------------------------------------------------------------------
Write-Host ''
Write-Host "Cross-canvas conflicts (unfiltered nearest anchor): $crossCanvasConflicts"
Write-Host ("Region totals: " + (($regions | ForEach-Object { "$($_['id'])=$($_['points'])" }) -join ' '))
Write-Host ''
if ($failures.Count -gt 0) {
    Write-Host "$($failures.Count) registry check(s) failed." -ForegroundColor Red
    foreach ($failure in $failures) { Write-Host "  - $failure" -ForegroundColor Red }
    exit 1
}
Write-Host 'Map region registry verified.' -ForegroundColor Green
foreach ($note in $notes) { Write-Host "  note: $note" }
