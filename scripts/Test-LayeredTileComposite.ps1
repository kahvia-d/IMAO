# Verifies the layered-tile composite offline before anything is published.
#
# Packs, all built from the same tile archive with the shipped feature builder
# (SURF(60,8,4,true,true), the parameters the published packs were built with):
#
#   surface  : the archived surface tiles -> today's pack. Its keypoint count must equal
#              the shipped jinzhou pack's, which is the control.
#   multi100, multi035 : the surface tiles PLUS one composite per floor at the same
#              coordinates. Listing several appearances of one coordinate is what lets a
#              single pack cover every floor: stacking the floors into one image instead
#              measured 5 near-anchor matches against a real 叩天关 minimap where the
#              correct floor's own composite scored 21.
#
# Queries:
#   real-surface-minimap : map-regions/references/jinzhou.png, a real in-game minimap at
#                          tile (0,-1), far from every layered tile -> surface regression
#   in-game frames       : full-screen captures taken inside a layered map, supplied as a
#                          comma-separated "path|x|y" list (-File cannot bind an array),
#                          where x,y are the coordinates the HUD shows. These are the only
#                          queries that exercise a real minimap render inside a layer;
#                          earlier revisions also cropped map tiles as proxies, which
#                          turned out to be position-sensitive and are not kept.
#
#   pwsh -File scripts\Test-LayeredTileComposite.ps1 `
#        -InGame 'C:\...\shot1.png|2598.29|2924,C:\...\shot2.png|2209.33|3355'

[CmdletBinding()]
param(
    [string]$SourceRoot,
    [string]$RegionId = 'jinzhou',
    [string]$Version = 'B50F4135DCCC4D8DA87ED33CE95EA31D',
    # Comma-separated "path|x|y" entries, because -File invocation cannot bind an array.
    [string]$InGame = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

if (-not $SourceRoot) { $SourceRoot = Split-Path -Parent $PSScriptRoot }
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
$work = Join-Path $SourceRoot 'out/map-regions/ab'
New-Item -ItemType Directory -Force -Path $work | Out-Null

$builder = Join-Path $SourceRoot 'x64/RelWithDebInfo/KuroMapFeatureBuilder.exe'
if (-not (Test-Path -LiteralPath $builder)) { throw "Feature builder is missing: $builder" }
$env:PATH = (Join-Path $SourceRoot 'x64/Release') + ';' + $env:PATH

$registry = Get-Content -LiteralPath (Join-Path $SourceRoot 'map-regions/regions.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$region = @($registry.regions | Where-Object { $_.id -eq $RegionId })
if ($region.Count -ne 1) { throw "Region '$RegionId' is not in the registry exactly once." }
$region = $region[0]
$state = [int]$region.frame
$originX = 2474.0; $originY = 1957.0; $scale = 1.205

$tileManifest = Get-Content -LiteralPath (Join-Path $SourceRoot 'map-regions/tiles/tiles.manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$surfaceRecords = @($tileManifest.regions.$RegionId.tiles | Where-Object { -not $_.absent })
$surfaceDir = Join-Path $SourceRoot "map-regions/tiles/$Version/$state"
$compositeRoot = Join-Path $SourceRoot "out/map-regions/composite/$RegionId"
foreach ($tag in 'k100', 'k035') {
    if (-not (Test-Path -LiteralPath (Join-Path $compositeRoot $tag))) {
        throw "Composite directory is missing: $compositeRoot/$tag (run scripts/New-LayeredTileComposite.ps1 first)."
    }
}

$packs = [ordered]@{
    surface  = @{ dir = $surfaceDir; extra = $null }
    multi100 = @{ dir = $surfaceDir; extra = (Join-Path $compositeRoot 'k100') }
    multi035 = @{ dir = $surfaceDir; extra = (Join-Path $compositeRoot 'k035') }
}

function Get-CompositeEntries([string]$dir) {
    # L<layer>_F<floor>_<state>_<x>_<y>.png
    foreach ($file in Get-ChildItem -LiteralPath $dir -File -Filter '*.png') {
        $parts = $file.BaseName.Split('_')
        if ($parts.Count -ne 5 -or -not $parts[0].StartsWith('L')) { continue }
        [pscustomobject]@{ x = [int]$parts[3]; y = [int]$parts[4]; path = $file.FullName }
    }
}

function Write-PackManifest([string]$tag, [string]$dir, [string]$extraDir) {
    # The builder resolves tile paths relative to the manifest's own directory and rejects
    # "..", so each pack gets a directory with hard-linked tiles. Hard links are only ever
    # read here; nothing writes through them.
    $packDir = Join-Path $work $tag
    $tilesDir = Join-Path $packDir 'tiles'
    New-Item -ItemType Directory -Force -Path $tilesDir | Out-Null
    function Add-Tile([string]$source, [string]$leaf) {
        $link = Join-Path $tilesDir $leaf
        if (-not (Test-Path -LiteralPath $link)) {
            try { New-Item -ItemType HardLink -Path $link -Target $source -ErrorAction Stop | Out-Null }
            catch { Copy-Item -LiteralPath $source -Destination $link -Force }
        }
        return (Get-FileHash -LiteralPath $link -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $tiles = New-Object System.Collections.ArrayList
    foreach ($record in $surfaceRecords) {
        $leaf = Split-Path -Leaf $record.file
        $source = Join-Path $dir $leaf
        if (-not (Test-Path -LiteralPath $source)) { throw "Tile missing in ${tag}: $source" }
        [void]$tiles.Add([ordered]@{ x = [int]$record.x; y = [int]$record.y; file = "tiles/$leaf"
            sha256 = (Add-Tile $source $leaf) })
    }
    $extraCount = 0
    if ($extraDir) {
        foreach ($entry in Get-CompositeEntries $extraDir) {
            $leaf = "layered_$(Split-Path -Leaf $entry.path)"
            [void]$tiles.Add([ordered]@{ x = $entry.x; y = $entry.y; file = "tiles/$leaf"
                sha256 = (Add-Tile $entry.path $leaf) })
            $extraCount++
        }
    }
    $manifest = [ordered]@{
        formatVersion = 1; packId = "$RegionId-$tag"; resourceVersion = $Version
        scene = 'World'; sceneId = 1
        source = [ordered]@{ static = 'https://web-static.kurobbs.com'; state = $state; tileSize = 1024; virtualMapSize = 850.0 }
        coordinateTransform = [ordered]@{ originX = $originX; originY = $originY; scale = $scale }
        tileBounds = $region.tileBounds
        tiles = @($tiles)
    }
    $path = Join-Path $packDir 'manifest.json'
    [IO.File]::WriteAllText($path, ($manifest | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
    return [pscustomobject]@{ path = $path; surfaceTiles = $surfaceRecords.Count; extraTiles = $extraCount }
}

$built = [ordered]@{}
foreach ($tag in $packs.Keys) {
    $info = Write-PackManifest $tag $packs[$tag].dir $packs[$tag].extra
    $features = Join-Path $work "$tag.features.yml"
    $report = Join-Path $work "$tag.report.json"
    if (Test-Path -LiteralPath $features) { Remove-Item -LiteralPath $features -Force }
    $stdout = & $builder --input $info.path --output $features --report $report 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Build failed for ${tag}: $($stdout -join ' ')" }
    $summary = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
    $built[$tag] = [pscustomobject]@{ manifest = $info.path; features = $features
        tiles = [int]$summary.tileCount; keypoints = [int]$summary.extractedKeypoints }
    Write-Host ("built {0,-9} tiles={1,4} (surface {2} + layered {3}) keypoints={4,7}" -f `
        $tag, $summary.tileCount, $info.surfaceTiles, $info.extraTiles, $summary.extractedKeypoints)
}

$queries = @(
    [pscustomobject]@{ name = 'real-surface-minimap'; path = (Join-Path $SourceRoot 'map-regions/references/jinzhou.png')
        anchorX = -96.0; anchorY = 1310.0; full = $true }
)
$frames = if ([string]::IsNullOrWhiteSpace($InGame)) { @() }
          else { @($InGame.Split(',') | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }) }
foreach ($frame in $frames) {
    $parts = $frame.Split('|')
    if ($parts.Count -ne 3) { throw "In-game query must be 'path|x|y': $frame" }
    if (-not (Test-Path -LiteralPath $parts[0])) { throw "In-game screenshot is missing: $($parts[0])" }
    $queries += [pscustomobject]@{ name = "ingame-$(Split-Path -Leaf $parts[0])"
        path = $parts[0]; anchorX = [double]$parts[1]; anchorY = [double]$parts[2]; full = $true }
}

$invariant = [Globalization.CultureInfo]::InvariantCulture
$rows = New-Object System.Collections.ArrayList
foreach ($tag in $built.Keys) {
    foreach ($query in $queries) {
        $report = Join-Path $work "verify-$tag-$($query.name).json"
        if (Test-Path -LiteralPath $report) { Remove-Item -LiteralPath $report -Force }
        $arguments = @('--input', $built[$tag].manifest, '--output', $built[$tag].features,
            '--report', $report, '--verify-only',
            '--verify-reference', $query.path,
            '--anchor-x', $query.anchorX.ToString($invariant), '--anchor-y', $query.anchorY.ToString($invariant))
        if ($query.full) { $arguments += '--reference-full-snapshot' }
        $null = & $builder @arguments 2>&1
        $exit = $LASTEXITCODE
        if (Test-Path -LiteralPath $report) {
            $verification = (Get-Content -LiteralPath $report -Raw | ConvertFrom-Json).referenceVerification
            [void]$rows.Add([pscustomobject]@{
                pack = $tag; query = $query.name; exit = $exit
                mapKeypoints = $verification.mapKeypoints; minimapKeypoints = $verification.minimapKeypoints
                goodMatches = $verification.goodMatches; nearAnchor = $verification.nearAnchorMatches
                errorPixels = [Math]::Round([double]$verification.errorPixels, 2); passed = $verification.passed })
        }
        else {
            [void]$rows.Add([pscustomobject]@{ pack = $tag; query = $query.name; exit = $exit
                mapKeypoints = '-'; minimapKeypoints = '-'; goodMatches = '-'; nearAnchor = '-'
                errorPixels = '-'; passed = 'NO REPORT' })
        }
    }
}

Write-Host ''
$rows | Format-Table -AutoSize | Out-String -Width 220 | Write-Host
$rows | Export-Csv -LiteralPath (Join-Path $work 'ab-results.csv') -NoTypeInformation -Encoding UTF8
Write-Host ("pack keypoints: " + (($built.Keys | ForEach-Object { "$_=$($built[$_].keypoints)" }) -join '  '))
