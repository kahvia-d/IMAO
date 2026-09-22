# Measures how many SURF keypoints the upstream layered-map ("分层地图") tiles carry,
# so that "can the tool localize inside a layered map?" is answered with numbers
# instead of a guess.
#
# It reuses the shipped KuroMapFeatureBuilder with the exact SURF parameters the
# published region packs were built with (SURF(60, 8, 4, true, true)), once per
# layered floor, plus a surface control over the same tile coordinates. Everything
# is written under out/layer-probe/; no pack is rebuilt, published, or committed.
#
#   pwsh -File scripts/Measure-LayeredMapFeatures.ps1
#
# Inputs come from the public layer manifest:
#   https://web-static.kurobbs.com/mcmap/layer/<version>/<state>/layer.json
# Outputs: out/layer-probe/results.csv, runs/<tag>.features.yml, runs/<tag>.report.json

[CmdletBinding()]
param(
    # Region whose layered maps are measured. Only regions whose frame carries a
    # layer.json can be measured; the default is Jinzhou (state 8 / mapState 1).
    [ValidateSet('jinzhou', 'mengzhou', 'laguna', 'qiqiu', 'roysurface', 'lahai', 'darkplain', 'lowervault', 'fabricatorium', 'tethys', 'blackshores')]
    [string]$RegionId = 'jinzhou',
    [string]$Version = 'B50F4135DCCC4D8DA87ED33CE95EA31D'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$probe = Join-Path $repoRoot 'out/layer-probe'
$tilesDir = Join-Path $probe 'tiles'
$runsDir = Join-Path $probe 'runs'
New-Item -ItemType Directory -Force -Path $tilesDir, $runsDir | Out-Null

$builder = Join-Path $repoRoot 'x64/RelWithDebInfo/KuroMapFeatureBuilder.exe'
if (-not (Test-Path -LiteralPath $builder)) { throw "Feature builder is missing: $builder" }
$env:PATH = (Join-Path $repoRoot 'x64/Release') + ';' + $env:PATH

# Region -> (frame, mapState). Layered tiles live under the frame, the tiles a
# region owns are the ones its points and entrance markers occupy.
$regions = @{
    jinzhou       = @{ state = 8; mapState = '1'; scene = 'World' }
    mengzhou      = @{ state = 8; mapState = '8'; scene = 'World' }
    laguna        = @{ state = 8; mapState = '3'; scene = 'World' }
    qiqiu         = @{ state = 8; mapState = '4'; scene = 'World' }
    roysurface    = @{ state = 8; mapState = '6'; scene = 'World' }
    blackshores   = @{ state = 8; mapState = ''; scene = 'World' }
    lahai         = @{ state = 906; mapState = '5'; scene = 'Lahai' }
    darkplain     = @{ state = 909; mapState = '7'; scene = 'Darkplain' }
    lowervault    = @{ state = 902; mapState = '3'; scene = 'LowerVault' }
    fabricatorium = @{ state = 905; mapState = '3'; scene = 'Fabricatorium' }
    tethys        = @{ state = 900; mapState = ''; scene = 'Tethys' }
}
$region = $regions[$RegionId]

# The layered maps a region owns are read off the region registry: the entrance
# markers (item FCRK) of the region's own scene name the layers it opens into.
$registryPath = Join-Path $repoRoot 'map-regions/regions.json'
if (-not (Test-Path -LiteralPath $registryPath)) { throw "Region registry is missing: $registryPath" }
$registry = Get-Content -LiteralPath $registryPath -Raw | ConvertFrom-Json
$pointFile = Join-Path $repoRoot "Assets/KuroMap/states/state-$($region.state).json"
if (-not (Test-Path -LiteralPath $pointFile)) { throw "Point snapshot is missing: $pointFile" }
$points = Get-Content -LiteralPath $pointFile -Raw | ConvertFrom-Json

$origins = @{}
foreach ($entry in $registry.regions) {
    $xs = @(); $ys = @()
    foreach ($area in $entry.areas) { $xs += ($area.rawX - $area.gameX * 100); $ys += ($area.rawY - $area.gameY * 100) }
    if ($xs.Count -gt 0) {
        $origins[[string]$entry.frame] = @([Math]::Round(($xs | Measure-Object -Average).Average, 1), [Math]::Round(($ys | Measure-Object -Average).Average, 1))
    }
}
$origin = $origins[[string]$region.state]
if ($null -eq $origin) { throw "Cannot derive a tile origin for frame $($region.state)." }
$scale = 1.205

$layerJson = Join-Path $probe "layer-$($region.state).json"
if (-not (Test-Path -LiteralPath $layerJson)) {
    & curl.exe --fail --silent --show-error --location --max-time 60 `
        "https://web-static.kurobbs.com/mcmap/layer/$Version/$($region.state)/layer.json" --output $layerJson
    if ($LASTEXITCODE -ne 0) { throw "No upstream layer.json for state $($region.state)." }
}
$layers = Get-Content -LiteralPath $layerJson -Raw | ConvertFrom-Json

# Attribute a layer to the region through its entrance markers: the marker is a
# surface point, so its nearest same-country anchor names the owning region.
$anchors = New-Object System.Collections.ArrayList
function Add-Anchors($node) {
    if ($node.level -ge 2) {
        [void]$anchors.Add([pscustomobject]@{ name = $node.name; countryId = $node.countryId; mapState = [string]$node.mapState; x = [double]$node.xPosition; y = [double]$node.yPosition })
    }
    # Leaf nodes carry no children property at all, and StrictMode turns that into
    # a terminating error, so the property has to be probed instead of read.
    $children = if ($null -ne $node.PSObject.Properties['children']) { @($node.children) } else { @() }
    foreach ($child in $children) { Add-Anchors $child }
}
foreach ($country in (Get-Content -LiteralPath (Join-Path $repoRoot 'Assets/KuroMap/country.json') -Raw | ConvertFrom-Json)) {
    foreach ($node in $country.countrys) { Add-Anchors $node }
}

$entrances = @{}
foreach ($item in $points) {
    foreach ($location in $item.location) {
        if ($item.id -ne 'FCRK') { continue }
        $best = $null; $bestDistance = [double]::MaxValue
        foreach ($anchor in $anchors | Where-Object { $_.countryId -eq $location.countryId }) {
            $distance = [Math]::Sqrt([Math]::Pow($anchor.x - $location.x, 2) + [Math]::Pow($anchor.y - $location.y, 2))
            if ($distance -lt $bestDistance) { $bestDistance = $distance; $best = $anchor }
        }
        $entrances[[string]$location.floorId] = [pscustomobject]@{
            mapState = if ($best) { $best.mapState } else { '' }
            gameX = ([double]$location.x - $origin[0]) / 100.0
            gameY = ([double]$location.y - $origin[1]) / 100.0
        }
    }
}

$owned = @($layers | Where-Object { $entrances.ContainsKey([string]$_.id) -and $entrances[[string]$_.id].mapState -eq $region.mapState })
if ($owned.Count -eq 0) { throw "No layered map in state $($region.state) attributes to region $RegionId." }

function Get-TileSpec([string]$imageName, [string]$localName, [bool]$layered) {
    $leaf = ($imageName -split '/')[-1]
    $parts = ($leaf -replace '\.png$', '').Split('_')
    $file = Join-Path $tilesDir $localName
    if (-not (Test-Path -LiteralPath $file)) {
        $url = if ($layered) { "https://web-static.kurobbs.com/mcmap/tiles/$Version/$($region.state)$imageName" }
               else { "https://web-static.kurobbs.com/mcmap/tiles/$Version/$($region.state)/$($region.state)_$($parts[0])_$($parts[1]).png" }
        & curl.exe --fail --silent --show-error --location --max-time 120 $url --output $file
        if ($LASTEXITCODE -ne 0) { throw "Tile download failed: $url" }
    }
    [pscustomobject]@{
        x = [int]$parts[0]; y = [int]$parts[1]; file = $localName
        sha256 = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
        bytes = (Get-Item -LiteralPath $file).Length
    }
}

$sceneIds = @{ World = 1; Tethys = 2; Fabricatorium = 3; Avinoleum = 4; Lahai = 5; LowerVault = 6; Darkplain = 7; TimeRiftRuins = 8 }

function Invoke-Build([string]$tag, [object[]]$specs) {
    $manifestPath = Join-Path $probe "manifest-$tag.json"
    $manifest = [ordered]@{
        formatVersion = 1; packId = "probe-$tag"; resourceVersion = $Version
        scene = $region.scene; sceneId = $sceneIds[$region.scene]
        source = [ordered]@{ static = 'https://web-static.kurobbs.com'; state = $region.state; tileSize = 1024; virtualMapSize = 850.0 }
        coordinateTransform = [ordered]@{ originX = $origin[0]; originY = $origin[1]; scale = $scale }
        tiles = @($specs | ForEach-Object { [ordered]@{ x = $_.x; y = $_.y; file = "tiles/$($_.file)"; sha256 = $_.sha256 } })
    }
    [IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
    $stdout = & $builder --input $manifestPath --output (Join-Path $runsDir "$tag.features.yml") --report (Join-Path $runsDir "$tag.report.json") 2>&1
    $exit = $LASTEXITCODE
    $perTile = @{}
    foreach ($line in $stdout) {
        $match = [regex]::Match([string]$line, '^processed tile (-?\d+),(-?\d+) features=(\d+)$')
        if ($match.Success) { $perTile["$($match.Groups[1].Value)_$($match.Groups[2].Value)"] = [int]$match.Groups[3].Value }
    }
    [pscustomobject]@{ tag = $tag; exitCode = $exit; perTile = $perTile
        error = if ($exit -ne 0) { ($stdout | Where-Object { $_ -match 'failed' }) -join ' ' } else { '' } }
}

$results = New-Object System.Collections.ArrayList
$controlCoords = @{}
foreach ($layer in $owned | Sort-Object { [int]$_.id }) {
    foreach ($floor in $layer.floors) {
        $tag = "$RegionId-layer$($layer.id)-floor$($floor.id -replace '/','-')"
        $specs = foreach ($tile in $floor.tiles) {
            $leaf = ($tile -split '/')[-1]
            $spec = Get-TileSpec $tile "L$($layer.id)_F$($floor.id -replace '/','-')_$leaf" $true
            $controlCoords["$($spec.x),$($spec.y)"] = $spec
            $spec
        }
        $run = Invoke-Build $tag @($specs)
        $index = 0
        foreach ($tile in $floor.tiles) {
            $leaf = ($tile -split '/')[-1]
            $parts = ($leaf -replace '\.png$', '').Split('_')
            [void]$results.Add([pscustomobject]@{
                kind = 'layered'; layerId = [int]$layer.id; layerName = $layer.name
                floorId = [string]$floor.id; floorName = [string]$floor.name
                tile = "$($parts[0]),$($parts[1])"
                keypoints = $run.perTile["$($parts[0])_$($parts[1])"]
                bytes = $specs[$index].bytes; run = $tag; exit = $run.exitCode; error = $run.error })
            $index++
        }
    }
}

# Surface control: the same coordinates built by the same extractor.
$controlSpecs = foreach ($key in $controlCoords.Keys) {
    $parts = $key.Split(',')
    Get-TileSpec "/$($key -replace ',','_').png" "SURF_$($region.state)_$($parts[0])_$($parts[1]).png" $false
}
$controlRun = Invoke-Build 'surface-control' @($controlSpecs)
foreach ($key in $controlCoords.Keys) {
    $parts = $key.Split(',')
    [void]$results.Add([pscustomobject]@{
        kind = 'surface'; layerId = $null; layerName = '地表(同坐标对照)'; floorId = ''; floorName = ''
        tile = $key; keypoints = $controlRun.perTile["$($parts[0])_$($parts[1])"]
        bytes = ($controlSpecs | Where-Object { $_.x -eq [int]$parts[0] -and $_.y -eq [int]$parts[1] }).bytes
        run = 'surface-control'; exit = $controlRun.exitCode; error = $controlRun.error })
}

$csv = Join-Path $probe "results-$RegionId.csv"
$results | Export-Csv -LiteralPath $csv -NoTypeInformation -Encoding UTF8
$results | Format-Table -AutoSize | Out-String -Width 220 | Write-Host
Write-Host "results: $csv"
