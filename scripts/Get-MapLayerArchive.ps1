# Archives the upstream layered-map ("分层地图") overlays for offline, reproducible builds.
#
# The public map front end publishes one layer manifest per frame:
#   https://web-static.kurobbs.com/mcmap/layer/<version>/<state>/layer.json
# and draws each floor from its own tile image:
#   https://web-static.kurobbs.com/mcmap/tiles/<version>/<state><image_name>
# where <image_name> is the path recorded in layer.json (e.g. /1/-1/3_-3.png).
#
# Layout written under <ArchiveRoot>/<generation>/ mirrors the upstream path:
#   <generation>/8/1/-1/3_-3.png
# Frames without layered maps (903 Avinoleum, 910 TimeRiftRuins) are recorded as
# absent rather than treated as an error: country.json marks both haveLayer=false.
#
#   pwsh -File scripts\Get-MapLayerArchive.ps1 -ResolveCurrentVersion

[CmdletBinding()]
param(
    [string]$SourceRoot,
    [string]$RegistryPath,
    [string]$ArchiveRoot,
    # Generation to download from. Defaults to the generation the region registry was
    # derived from; -ResolveCurrentVersion asks the public API instead.
    [string]$ResourceVersion,
    [switch]$ResolveCurrentVersion,
    # Comma-separated frame ids, because -File invocation cannot bind an array.
    [string]$State = '',
    [switch]$Check
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $SourceRoot) { $SourceRoot = Split-Path -Parent $PSScriptRoot }
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
if (-not $RegistryPath) { $RegistryPath = Join-Path $SourceRoot 'map-regions/regions.json' }
if (-not $ArchiveRoot) { $ArchiveRoot = Join-Path $SourceRoot 'map-regions/layers' }
$RegistryPath = [IO.Path]::GetFullPath($RegistryPath)
$ArchiveRoot = [IO.Path]::GetFullPath($ArchiveRoot)

$kuroStaticHost = 'web-static.kurobbs.com'
$kuroApiHost = 'api.kurobbs.com'

if (-not (Test-Path -LiteralPath $RegistryPath)) { throw "Missing region registry: $RegistryPath" }
$registry = Get-Content -LiteralPath $RegistryPath -Raw -Encoding UTF8 | ConvertFrom-Json -AsHashtable
$recordedVersion = ([string]$registry['generatedFrom']['kuroResourceVersion']).ToUpperInvariant()
if ($recordedVersion -notmatch '^[A-Fa-f0-9]{32}$') { throw "Registry records an invalid resource version: $recordedVersion" }

function Get-CurrentKuroResourceVersion {
    $response = & curl.exe --fail --silent --show-error --location --proto '=https' --tlsv1.2 `
        --connect-timeout 15 --max-time 60 -X POST "https://$kuroApiHost/map/core/config/getMapResource" `
        -H 'content-type: application/json' -d '{}' 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Could not read the current Kuro map resource version: $response" }
    $parsed = $response | ConvertFrom-Json
    if ([int]$parsed.code -ne 200 -or ([string]$parsed.data) -notmatch '^[A-Fa-f0-9]{32}$') {
        throw "Kuro map resource response was invalid: $response"
    }
    return ([string]$parsed.data).ToUpperInvariant()
}

$tileVersion = $recordedVersion
# The registry records the generation it was derived from, but that generation can be
# retired upstream (the archived one is). When a tile archive exists, its generation is
# the one actually on disk, so it is the right default for the overlays that must line
# up with those tiles.
$tileManifestPath = Join-Path $SourceRoot 'map-regions/tiles/tiles.manifest.json'
if (Test-Path -LiteralPath $tileManifestPath) {
    $archived = ([string](Get-Content -LiteralPath $tileManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json).tileResourceVersion).ToUpperInvariant()
    if ($archived -match '^[A-Fa-f0-9]{32}$') {
        if ($archived -ne $recordedVersion) {
            Write-Warning "Using the generation recorded by the tile archive ($archived) instead of the registry generation ($recordedVersion)."
        }
        $tileVersion = $archived
    }
}
if ($ResolveCurrentVersion) { $tileVersion = Get-CurrentKuroResourceVersion }
elseif ($ResourceVersion) {
    if ($ResourceVersion -notmatch '^[A-Fa-f0-9]{32}$') { throw "Resource version is not a 32-character hex value: $ResourceVersion" }
    $tileVersion = $ResourceVersion.ToUpperInvariant()
}
Write-Host "Registry generation: $recordedVersion"
Write-Host "Layer generation:    $tileVersion"

$states = if ([string]::IsNullOrWhiteSpace($State)) {
    @(Get-ChildItem -LiteralPath (Join-Path $SourceRoot 'Assets/KuroMap/states') -Filter 'state-*.json' |
        ForEach-Object { [int]($_.BaseName -replace '^state-', '') } | Sort-Object)
}
else { @($State.Split(',') | ForEach-Object { [int]$_.Trim() }) }
if ($states.Count -eq 0) { throw 'No states to archive.' }

function Invoke-LayerDownload([string]$Url, [string]$Destination) {
    $parent = Split-Path -Parent $Destination
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    $partial = "$Destination.partial"
    # A confirmed 404 is data ("this frame has no layered maps"); anything else is a failure.
    $code = & curl.exe --silent --show-error --location --proto '=https' --tlsv1.2 `
        --connect-timeout 15 --max-time 60 -w '%{http_code}' -o $partial $Url 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Layer download failed ($LastExitCode): $Url" }
    $status = [int]($code | Select-Object -Last 1)
    if ($status -eq 404) { if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Force }; return 404 }
    if ($status -ne 200) { throw "Layer download returned HTTP ${status}: $Url" }
    Move-Item -LiteralPath $partial -Destination $Destination -Force
    return 200
}

$manifest = [ordered]@{
    formatVersion = 1
    registryResourceVersion = $recordedVersion
    layerResourceVersion = $tileVersion
    generatedAtUtc = [DateTime]::UtcNow.ToString('o')
    archiveRoot = "map-regions/layers/$tileVersion"
    states = [ordered]@{}
}
$totalTiles = 0
$totalAbsent = 0

foreach ($stateId in $states) {
    $layerJsonUrl = "https://$kuroStaticHost/mcmap/layer/$tileVersion/$stateId/layer.json"
    $layerJsonPath = Join-Path $ArchiveRoot "$tileVersion/$stateId/layer.json"
    $status = Invoke-LayerDownload $layerJsonUrl $layerJsonPath
    if ($status -eq 404) {
        Write-Host ("  state {0,-4} no layered maps (layer.json 404)" -f $stateId)
        $manifest.states[[string]$stateId] = [ordered]@{ hasLayers = $false; layers = @(); tiles = @() }
        continue
    }
    $layers = @(Get-Content -LiteralPath $layerJsonPath -Raw -Encoding UTF8 | ConvertFrom-Json)
    $unique = [ordered]@{}
    foreach ($layer in $layers) {
        foreach ($floor in $layer.floors) {
            foreach ($image in @($floor.tiles)) { $unique[[string]$image] = $true }
        }
    }
    $tileRecords = New-Object System.Collections.ArrayList
    $present = 0; $absent = 0
    foreach ($image in $unique.Keys) {
        if ($image -notmatch '^/[^/]+/[^/]+/[^/]+\.png$') { throw "Unexpected layer tile path: $image" }
        $relative = "$stateId$image"
        $destination = Join-Path $ArchiveRoot "$tileVersion/$relative"
        $status = Invoke-LayerDownload "https://$kuroStaticHost/mcmap/tiles/$tileVersion/$relative" $destination
        if ($status -eq 404) {
            [void]$tileRecords.Add([ordered]@{ file = "tiles/$relative"; sha256 = $null; absent = $true })
            $absent++
            continue
        }
        [void]$tileRecords.Add([ordered]@{
            file = "tiles/$relative"
            sha256 = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
            absent = $false
        })
        $present++
    }
    $floorCount = (@($layers | ForEach-Object { $_.floors.Count }) | Measure-Object -Sum).Sum
    Write-Host ("  state {0,-4} layers={1,-3} floors={2,-3} tiles={3,-3} present={4,-3} absent={5}" -f `
        $stateId, $layers.Count, $floorCount, $unique.Count, $present, $absent)
    $manifest.states[[string]$stateId] = [ordered]@{
        hasLayers = $true
        layers = @($layers | ForEach-Object {
            [ordered]@{ id = [string]$_.id; name = [string]$_.name; floors = @($_.floors | ForEach-Object {
                [ordered]@{ id = [string]$_.id; name = [string]$_.name; sort = $_.sort; tiles = @($_.tiles) } }) }
        })
        tiles = @($tileRecords)
    }
    $totalTiles += $unique.Count
    $totalAbsent += $absent
}

if ($Check) {
    Write-Host "Check only: $totalTiles layer tiles enumerated, $totalAbsent absent upstream."
    return
}

$manifestPath = Join-Path $ArchiveRoot 'layers.manifest.json'
[IO.File]::WriteAllText($manifestPath, (($manifest | ConvertTo-Json -Depth 12) + [Environment]::NewLine), [Text.UTF8Encoding]::new($false))
Write-Host "Archived $totalTiles layer tiles ($totalAbsent absent upstream) under $ArchiveRoot/$tileVersion" -ForegroundColor Green
Write-Host "Manifest: $manifestPath"
