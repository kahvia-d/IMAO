# Verifies the layered-tile composite offline before anything is published.
#
# Builds four Jinzhou packs from the same tile archive with the shipped feature builder
# (SURF(60,8,4,true,true) - the parameters the published packs were built with):
#
#   surface : the archived surface tiles (today's pack; the keypoint count must match the
#             shipped jinzhou pack exactly, which is the control)
#   k100    : the layered composite REPLACES the surface tile at layered coordinates
#   k035    : the same, with the surface dimmed to 0.35 (the measured in-game layered
#             large-map brightness is 0.197)
#   both    : the surface tiles PLUS the k100 composites at the same coordinates
#
# and locks three queries against each pack:
#
#   real-surface-minimap : map-regions/references/jinzhou.png, a real in-game minimap at
#                          tile (0,-1), far from every layered tile -> surface regression
#   cave-composite       : 1:1 crop of the cave in tile (4,-3) taken from the composite
#   cave-surface         : the same crop taken from the raw surface tile (the player
#                          standing above the cave)
#
# The two cave crops are geometric proxies, not real minimap renders: they are valid for
# comparing packs against each other, not as an absolute accuracy claim. Only an in-game
# minimap captured inside a layer settles that, which is what the runtime pack is for.
#
#   pwsh -File scripts\Test-LayeredTileComposite.ps1

[CmdletBinding()]
param(
    [string]$SourceRoot,
    [string]$RegionId = 'jinzhou',
    [string]$Version = 'B50F4135DCCC4D8DA87ED33CE95EA31D'
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
    surface = @{ dir = $surfaceDir; extra = $null }
    k100    = @{ dir = (Join-Path $compositeRoot 'k100'); extra = $null }
    k035    = @{ dir = (Join-Path $compositeRoot 'k035'); extra = $null }
    both    = @{ dir = $surfaceDir; extra = (Join-Path $compositeRoot 'k100') }
}

function Write-PackManifest([string]$tag, [string]$dir, [string]$extraDir) {
    # The builder resolves tile paths relative to the manifest's own directory and
    # rejects "..", so each pack gets a directory with hard-linked tiles. Hard links are
    # only ever read here; nothing writes through them.
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
    if ($extraDir) {
        # The same coordinates a second time, carrying the layered appearance. Every
        # listed tile is mapped into the pack, so a duplicate (x,y) holds both textures.
        foreach ($record in $surfaceRecords) {
            $leaf = Split-Path -Leaf $record.file
            $source = Join-Path $extraDir $leaf
            if (-not (Test-Path -LiteralPath $source)) { continue }
            if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -eq
                (Get-FileHash -LiteralPath (Join-Path $dir $leaf) -Algorithm SHA256).Hash) { continue }
            [void]$tiles.Add([ordered]@{ x = [int]$record.x; y = [int]$record.y; file = "tiles/layer_$leaf"
                sha256 = (Add-Tile $source "layer_$leaf") })
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
    return $path
}

$built = [ordered]@{}
foreach ($tag in $packs.Keys) {
    $manifestPath = Write-PackManifest $tag $packs[$tag].dir $packs[$tag].extra
    $features = Join-Path $work "$tag.features.yml"
    $report = Join-Path $work "$tag.report.json"
    if (Test-Path -LiteralPath $features) { Remove-Item -LiteralPath $features -Force }
    $stdout = & $builder --input $manifestPath --output $features --report $report 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Build failed for ${tag}: $($stdout -join ' ')" }
    $summary = Get-Content -LiteralPath $report -Raw | ConvertFrom-Json
    $built[$tag] = [pscustomobject]@{ manifest = $manifestPath; features = $features
        tiles = [int]$summary.tileCount; keypoints = [int]$summary.extractedKeypoints }
    Write-Host ("built {0,-8} tiles={1,4} keypoints={2,7}" -f $tag, $summary.tileCount, $summary.extractedKeypoints)
}

# Cave query centre: the opaque centroid of one real overlay, so the crop sits on cave
# texture rather than wherever the tile centre happens to be.
$stateDir = Join-Path $SourceRoot "map-regions/layers/$Version/$state"
$overlay = Get-ChildItem -LiteralPath $stateDir -Recurse -File -Filter '4_-3.png' | Select-Object -First 1
if ($null -eq $overlay) { throw "No overlay tile for (4,-3) under $stateDir." }
$bmp = [System.Drawing.Bitmap]::FromFile($overlay.FullName)
$sumX = 0.0; $sumY = 0.0; $count = 0
for ($y = 0; $y -lt 1024; $y += 2) {
    for ($x = 0; $x -lt 1024; $x += 2) { if ($bmp.GetPixel($x, $y).A -gt 200) { $sumX += $x; $sumY += $y; $count++ } }
}
$bmp.Dispose()
if ($count -eq 0) { throw "Overlay $($overlay.FullName) has no opaque pixels." }
$cx = [int]($sumX / $count); $cy = [int]($sumY / $count)
$half = 97
$crop = New-Object System.Drawing.Rectangle ([Math]::Max(0, $cx - $half)), ([Math]::Max(0, $cy - $half)), 194, 194
Write-Host "cave query: tile (4,-3) pixel ($cx,$cy), $count opaque samples"

foreach ($pair in @(@('cave-composite', (Join-Path $compositeRoot 'k100')), @('cave-surface', $surfaceDir))) {
    $leaf = "${state}_4_-3.png"
    $src = [System.Drawing.Bitmap]::FromFile((Join-Path $pair[1] $leaf))
    $dst = New-Object System.Drawing.Bitmap 194, 194, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($dst)
    $graphics.DrawImage($src, (New-Object System.Drawing.Rectangle 0, 0, 194, 194), $crop, [System.Drawing.GraphicsUnit]::Pixel)
    $graphics.Dispose(); $src.Dispose()
    $dst.Save((Join-Path $work "$($pair[0]).png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $dst.Dispose()
}
# Crop centre -> game coordinates, the inverse of the builder's KuroTilePointToAppMap.
$kuroX = 4 * 1024 + $crop.X + $half
$kuroY = -3 * 1024 - ($crop.Y + $half)
$gameX = ($kuroX - 1024) * 850.0 / 1024.0
$gameY = -$kuroY * 850.0 / 1024.0

$queries = @(
    [pscustomobject]@{ name = 'real-surface-minimap'; path = (Join-Path $SourceRoot 'map-regions/references/jinzhou.png')
        anchorX = -96.0; anchorY = 1310.0; full = $true }
    [pscustomobject]@{ name = 'cave-composite'; path = (Join-Path $work 'cave-composite.png')
        anchorX = $gameX; anchorY = $gameY; full = $false }
    [pscustomobject]@{ name = 'cave-surface'; path = (Join-Path $work 'cave-surface.png')
        anchorX = $gameX; anchorY = $gameY; full = $false }
)

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
        $verification = (Get-Content -LiteralPath $report -Raw | ConvertFrom-Json).referenceVerification
        [void]$rows.Add([pscustomobject]@{
            pack = $tag; query = $query.name; exit = $exit
            mapKeypoints = $verification.mapKeypoints; minimapKeypoints = $verification.minimapKeypoints
            goodMatches = $verification.goodMatches; nearAnchor = $verification.nearAnchorMatches
            errorPixels = [Math]::Round([double]$verification.errorPixels, 2); passed = $verification.passed })
    }
}

Write-Host ''
$rows | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
$rows | Export-Csv -LiteralPath (Join-Path $work 'ab-results.csv') -NoTypeInformation -Encoding UTF8
Write-Host ("pack keypoints: " + (($built.Keys | ForEach-Object { "$_=$($built[$_].keypoints)" }) -join '  '))
