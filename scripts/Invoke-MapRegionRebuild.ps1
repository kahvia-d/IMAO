[CmdletBinding()]
param(
    [string]$SourceRoot,
    # Named *Path so it cannot collide with the parsed registry object.
    [string]$RegistryPath,
    # Tile archive generation directory. Defaults to the generation recorded in
    # map-regions/tiles/tiles.manifest.json.
    [string]$TileArchive,
    # Regenerated features are build products and are never written into the repository.
    [string]$OutputRoot,
    [string[]]$RegionId = @(),
    # Build the packs. Without it the script only validates the plan.
    [switch]$Apply,
    # Regions without a four-point calibration cannot be built; they are skipped with a
    # warning instead of failing the whole run.
    [switch]$SkipBlocked,
    # Directory holding one captured reference minimap per region, named <region>.png.
    # A region without one is built as an explicitly unverified coverage pack.
    [string]$ReferenceRoot,
    # Treat each reference file as a complete game screenshot and crop the minimap.
    [switch]$ReferenceFullSnapshot,
    # Reuse a shipped pack's surveyed anchor and its captured reference minimap. An anchor
    # read off the in-game coordinate display and a minimap captured at that spot are
    # observations no pipeline can derive; the reference check makes a wrong pairing fail
    # loudly rather than produce a plausible but uncalibrated pack.
    [switch]$UseShippedReference,
    # Copy each built pack into a runnable output tree's Assets so the runtime loads it.
    [string]$InstallRoot,
    [ValidateRange(1, 4096)][int]$MaxTiles = 1024
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $SourceRoot) { $SourceRoot = Split-Path -Parent $PSScriptRoot }
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
if (-not $RegistryPath) { $RegistryPath = Join-Path $SourceRoot 'map-regions/regions.json' }
if (-not $OutputRoot) { $OutputRoot = Join-Path $SourceRoot 'out/map-regions/packs' }
if (-not $ReferenceRoot) { $ReferenceRoot = Join-Path $SourceRoot 'map-regions/references' }
$RegistryPath = [IO.Path]::GetFullPath($RegistryPath)
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$ReferenceRoot = [IO.Path]::GetFullPath($ReferenceRoot)
$repoAssets = [IO.Path]::GetFullPath((Join-Path $SourceRoot 'Assets'))
if ($OutputRoot.StartsWith($repoAssets, [StringComparison]::OrdinalIgnoreCase)) {
    throw "OutputRoot must not be inside Assets/: regenerated features are build products, not source. Got $OutputRoot"
}

if (-not (Test-Path -LiteralPath $RegistryPath)) { throw "Missing region registry: $RegistryPath" }
$registry = Get-Content -LiteralPath $RegistryPath -Raw -Encoding UTF8 | ConvertFrom-Json -AsHashtable
if ([int]$registry['formatVersion'] -ne 1) { throw 'Unsupported region registry format.' }

if (-not $TileArchive) {
    $archiveManifestPath = Join-Path $SourceRoot 'map-regions/tiles/tiles.manifest.json'
    if (-not (Test-Path -LiteralPath $archiveManifestPath)) {
        throw "No tile archive found. Run scripts/Get-MapTileArchive.ps1 first (expected $archiveManifestPath)."
    }
    $archiveManifest = Get-Content -LiteralPath $archiveManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json -AsHashtable
    $TileArchive = Join-Path $SourceRoot ([string]$archiveManifest['archiveRoot'])
    $tileVersion = [string]$archiveManifest['tileResourceVersion']
    $substituted = [bool]$archiveManifest['substituted']
}
else {
    $TileArchive = [IO.Path]::GetFullPath($TileArchive)
    $archiveManifestPath = Join-Path $SourceRoot 'map-regions/tiles/tiles.manifest.json'
    $tileVersion = Split-Path -Leaf $TileArchive
    $substituted = $false
    if (Test-Path -LiteralPath $archiveManifestPath) {
        $archiveManifest = Get-Content -LiteralPath $archiveManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json -AsHashtable
        if ([string]$archiveManifest['tileResourceVersion'] -eq $tileVersion) { $substituted = [bool]$archiveManifest['substituted'] }
    }
}
if (-not (Test-Path -LiteralPath $TileArchive)) { throw "Tile archive directory does not exist: $TileArchive" }
if ($tileVersion -notmatch '^[A-Fa-f0-9]{32}$') { throw "Tile archive generation is not a 32-hex value: $tileVersion" }

Write-Host "Registry:     $RegistryPath"
Write-Host "Tile archive: $TileArchive (generation $tileVersion)"
if ($substituted) {
    Write-Warning 'This archive substitutes a different tile generation than the registry was derived from. It was verified byte-for-byte against the shipped packs; see tiles.manifest.json.'
}

$selected = @($registry['regions'])
if ($RegionId.Count -gt 0) {
    $known = @($selected | ForEach-Object { [string]$_['id'] })
    foreach ($id in $RegionId) { if ($known -notcontains $id) { throw "Unknown region id: $id" } }
    $wanted = [Collections.Generic.HashSet[string]]::new([string[]]$RegionId, [StringComparer]::OrdinalIgnoreCase)
    $selected = @($selected | Where-Object { $wanted.Contains([string]$_['id']) })
}
if ($selected.Count -eq 0) { throw 'No region selected.' }

$buildable = [Collections.Generic.List[object]]::new()
$skipped = [Collections.Generic.List[object]]::new()
foreach ($record in $selected) {
    if (-not [bool]$record['buildable'] -or $null -eq $record['tileBounds']) { $skipped.Add($record) } else { $buildable.Add($record) }
}
if ($skipped.Count -gt 0) {
    foreach ($record in $skipped) {
        Write-Warning ("Skipping {0} ({1}): {2}" -f $record['id'], $record['tileConfidence'],
            'no trustworthy tile window; calibrate the frame first')
    }
    if (-not $SkipBlocked -and $buildable.Count -eq 0) { throw 'Every selected region is blocked on calibration.' }
}

Write-Host ''
Write-Host ("{0,-14} {1,-12} {2,-6} {3,-6} {4}" -f 'region', 'scene', 'frame', 'tiles', 'window')
foreach ($record in $buildable) {
    $bounds = $record['tileBounds']
    Write-Host ("{0,-14} {1,-12} {2,-6} {3,-6} x {4}..{5} y {6}..{7}" -f `
        $record['id'], $record['scene'], $record['frame'], $bounds['count'], $bounds['minX'], $bounds['maxX'], $bounds['minY'], $bounds['maxY'])
}
Write-Host ''
Write-Host "Buildable: $($buildable.Count)  skipped: $($skipped.Count)  mode: $(if ($Apply) { 'APPLY' } else { 'CHECK' })"

if (-not $Apply) {
    Write-Host 'Validation only. Re-run with -Apply to build the packs.' -ForegroundColor Yellow
    return
}

New-Item -ItemType Directory -Force $OutputRoot | Out-Null
$syncScript = Join-Path $PSScriptRoot 'Sync-KuroMapFeaturePack.ps1'

# The feature binary and the per-pack visual-index shard are produced by two native
# tools. scripts/Build-VisualIndex.ps1 also rewrites the baseline index inside Assets/,
# which a region rebuild must never do, so only the two targets are built here.
function Get-FeatureTools {
    $outputDirectory = Join-Path $SourceRoot 'x64/Release'
    $converter = Join-Path $outputDirectory 'IMaoFeatureConverter.exe'
    $indexBuilder = Join-Path $outputDirectory 'IMaoVisualIndexBuilder.exe'
    if ((Test-Path -LiteralPath $converter) -and (Test-Path -LiteralPath $indexBuilder)) {
        return [pscustomobject]@{ Converter = $converter; IndexBuilder = $indexBuilder }
    }
    Write-Host 'Building IMaoFeatureConverter and IMaoVisualIndexBuilder...'
    if ([string]::IsNullOrWhiteSpace($env:IMAO_PADDLE_LIB) -or [string]::IsNullOrWhiteSpace($env:IMAO_OPENCV_DIR)) {
        throw 'Set IMAO_PADDLE_LIB and IMAO_OPENCV_DIR (dot-source scripts/Enter-DevEnvironment.ps1) before building the feature tools.'
    }
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    $cmake = & $vswhere -latest -products * -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' | Select-Object -First 1
    $vcvars = & $vswhere -latest -products * -find 'VC\Auxiliary\Build\vcvars64.bat' | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($cmake) -or [string]::IsNullOrWhiteSpace($vcvars)) {
        throw 'Visual Studio CMake or the x64 developer environment is unavailable.'
    }
    $configure = 'call "' + $vcvars + '" >nul && "' + $cmake + '" --fresh --preset windows-x64-release "-DPADDLE_LIB=' + $env:IMAO_PADDLE_LIB + '" "-DOPENCV_DIR=' + $env:IMAO_OPENCV_DIR + '"'
    & "$env:SystemRoot\System32\cmd.exe" /d /c $configure
    if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
    $build = 'call "' + $vcvars + '" >nul && "' + $cmake + '" --build out\build\windows-x64-release --target IMaoFeatureConverter IMaoVisualIndexBuilder'
    & "$env:SystemRoot\System32\cmd.exe" /d /c $build
    if ($LASTEXITCODE -ne 0) { throw 'Feature tool build failed.' }
    $opencvRuntime = Get-ChildItem -Path $env:IMAO_OPENCV_DIR -Recurse -Filter 'opencv_world*.dll' -File | Sort-Object FullName | Select-Object -First 1
    if ($null -eq $opencvRuntime) { throw 'Unable to find opencv_world*.dll.' }
    Copy-Item -LiteralPath $opencvRuntime.FullName -Destination $outputDirectory -Force
    if (-not (Test-Path -LiteralPath $indexBuilder)) { throw "Feature tools were not produced at $outputDirectory." }
    return [pscustomobject]@{ Converter = $converter; IndexBuilder = $indexBuilder }
}

function Complete-PackRegion([string]$packDirectory, $tools) {
    $featureXml = Join-Path $packDirectory 'features.yml'
    if (-not (Test-Path -LiteralPath $featureXml)) { throw "Pack has no features.yml: $packDirectory" }
    & $tools.Converter $featureXml (Join-Path $packDirectory 'features.imf') (Join-Path $packDirectory 'features.imf.manifest.json')
    if ($LASTEXITCODE -ne 0) { throw 'Feature binary conversion failed.' }
    # --pack-only uses the baseline vocabulary but never rewrites the baseline itself.
    $assetsRoot = Join-Path $SourceRoot 'Assets'
    & $tools.IndexBuilder '--pack-only' $assetsRoot $packDirectory '--allow-unverified'
    if ($LASTEXITCODE -ne 0) { throw 'Per-pack visual index generation failed.' }
}

$tools = $null
$results = [Collections.Generic.List[object]]::new()

# Shipped packs are matched to a region by directory name. They are the only source of
# surveyed anchors and captured minimaps.
$shippedRoot = Join-Path $SourceRoot 'Assets/FeaturesDatas/KuroTilePacks'
$shippedByRegion = @{}
foreach ($shippedDirectory in @(Get-ChildItem -LiteralPath $shippedRoot -Directory -ErrorAction SilentlyContinue)) {
    $shippedByRegion[$shippedDirectory.Name.ToLowerInvariant()] = $shippedDirectory
}
if ($UseShippedReference) {
    Write-Host "Reusing shipped anchors and reference minimaps from $shippedRoot"
}
foreach ($record in $buildable) {
    $bounds = $record['tileBounds']
    $anchor = $record['anchor']
    $currentRegion = [string]$record['id']
    $anchorX = [double]$anchor['x']
    $anchorY = [double]$anchor['y']
    $referencePath = Join-Path $ReferenceRoot "$currentRegion.png"
    $shipped = if ($shippedByRegion.ContainsKey($currentRegion)) { $shippedByRegion[$currentRegion] } else { $null }
    $shippedReference = if ($null -ne $shipped) { Join-Path $shipped.FullName 'reference-minimap.png' } else { $null }
    if ($UseShippedReference -and $null -ne $shipped -and (Test-Path -LiteralPath $shippedReference -PathType Leaf)) {
        $shippedManifest = Get-Content -LiteralPath (Join-Path $shipped.FullName 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json -AsHashtable
        $anchorX = [double]$shippedManifest['anchorWorldCoordinate']['x']
        $anchorY = [double]$shippedManifest['anchorWorldCoordinate']['y']
        $referencePath = $shippedReference
    }
    $arguments = @{
        Apply            = $true
        PackId           = $currentRegion
        Scene            = [string]$record['scene']
        State            = [int]$record['frame']
        AnchorWorldX     = $anchorX
        AnchorWorldY     = $anchorY
        TileMinX         = [int]$bounds['minX']
        TileMaxX         = [int]$bounds['maxX']
        TileMinY         = [int]$bounds['minY']
        TileMaxY         = [int]$bounds['maxY']
        MaxTiles         = $MaxTiles
        TileArchive      = $TileArchive
        ResourceVersion  = $tileVersion
        OutputRoot       = $OutputRoot
    }
    # A region with a captured reference minimap is built as a verified pack; without one
    # the build must explicitly mark the pack unverified rather than imply accuracy.
    if (Test-Path -LiteralPath $referencePath -PathType Leaf) {
        $arguments.ReferencePath = $referencePath
        if ($ReferenceFullSnapshot) { $arguments.ReferenceFullSnapshot = $true }
        Write-Host "  reference minimap: $referencePath"
        Write-Host "  anchor: ($anchorX, $anchorY)"
    }
    else {
        $arguments.SkipReferenceVerification = $true
        Write-Warning "  no reference minimap at $referencePath; building an explicitly unverified coverage pack"
    }
    Write-Host ''
    Write-Host "=== $($record['id']) ($($record['name'])) — $($record['scene']) state $($record['frame']) ===" -ForegroundColor Cyan
    $started = Get-Date
    try {
        & $syncScript @arguments
        if ($LASTEXITCODE -ne 0 -and $null -ne $LASTEXITCODE) { throw "Sync script exited with $LASTEXITCODE." }
        $packDirectory = Join-Path $OutputRoot ([string]$record['id'])
        if ($null -eq $tools) { $tools = Get-FeatureTools }
        Write-Host '  converting feature binary and building the pack visual index...'
        Complete-PackRegion $packDirectory $tools
        $manifestPath = Join-Path $packDirectory 'manifest.json'
        $keypoints = $null
        if (Test-Path -LiteralPath $manifestPath) {
            $pack = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json -AsHashtable
            $keypoints = [int]$pack['features']['keypointCount']
        }
        $results.Add([pscustomobject]@{ Region = [string]$record['id']; Status = 'ok'; Keypoints = $keypoints
            Seconds = [Math]::Round(((Get-Date) - $started).TotalSeconds, 1) })
    }
    catch {
        Write-Warning "$($record['id']) failed: $($_.Exception.Message)"
        $results.Add([pscustomobject]@{ Region = [string]$record['id']; Status = 'failed'; Keypoints = $null
            Seconds = [Math]::Round(((Get-Date) - $started).TotalSeconds, 1) })
        if (-not $SkipBlocked) { throw }
    }
}

Write-Host ''
Write-Host '=== Rebuild summary ==='
foreach ($result in $results) {
    Write-Host ("  {0,-14} {1,-8} keypoints={2,-8} {3}s" -f $result.Region, $result.Status, ($result.Keypoints ?? '—'), $result.Seconds)
}
$failed = @($results | Where-Object { $_.Status -ne 'ok' })
$ok = @($results | Where-Object { $_.Status -eq 'ok' })
Write-Host "Built $($ok.Count) pack(s), $($failed.Count) failed." -ForegroundColor $(if ($failed.Count) { 'Red' } else { 'Green' })
Write-Host "Packs are in $OutputRoot. Regenerated features are build products: publish them as resource packages, do not commit them."

if ($InstallRoot -and $ok.Count -gt 0) {
    $InstallRoot = [IO.Path]::GetFullPath($InstallRoot)
    $installPacks = Join-Path $InstallRoot 'Assets/FeaturesDatas/KuroTilePacks'
    if (-not (Test-Path -LiteralPath $installPacks)) {
        throw "InstallRoot has no staged Assets/FeaturesDatas/KuroTilePacks: $InstallRoot. Build or stage the application first."
    }
    $registryPath = Join-Path $InstallRoot 'Assets/FeaturesDatas/kuro-tile-packs.json'
    if (-not (Test-Path -LiteralPath $registryPath)) { throw "InstallRoot has no tile-pack registry: $registryPath" }
    $registryDocument = Get-Content -LiteralPath $registryPath -Raw -Encoding UTF8 | ConvertFrom-Json -AsHashtable
    if ([int]$registryDocument['formatVersion'] -ne 1 -or $null -eq $registryDocument['packs']) { throw 'Installed tile-pack registry format is invalid.' }
    $installed = [Collections.Generic.List[string]]::new()
    foreach ($result in $ok) {
        $currentRegion = [string]$result.Region
        $source = Join-Path $OutputRoot $currentRegion
        if (-not (Test-Path -LiteralPath (Join-Path $source 'manifest.json'))) { throw "Built pack is missing its manifest: $source" }
        # Replace the shipped directory in place. A second pack for the same scene would be
        # merged alongside the first and duplicate every keypoint.
        $targetName = if ($shippedByRegion.ContainsKey($currentRegion)) { $shippedByRegion[$currentRegion].Name } else { $currentRegion }
        $target = Join-Path $installPacks $targetName
        if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force }
        [IO.Directory]::CreateDirectory($target) | Out-Null
        Copy-Item -Path (Join-Path $source '*') -Destination $target -Recurse -Force
        # Keep the captured reference beside the pack it verified: it is provenance the
        # source tree does not otherwise carry for a rebuilt pack.
        $referencePath = Join-Path $ReferenceRoot "$currentRegion.png"
        if ($UseShippedReference -and $shippedByRegion.ContainsKey($currentRegion)) {
            $shippedReference = Join-Path $shippedByRegion[$currentRegion].FullName 'reference-minimap.png'
            if (Test-Path -LiteralPath $shippedReference -PathType Leaf) { Copy-Item -LiteralPath $shippedReference -Destination $target -Force }
        }
        elseif (Test-Path -LiteralPath $referencePath -PathType Leaf) { Copy-Item -LiteralPath $referencePath -Destination $target -Force }
        if ($registryDocument['packs'] -notcontains $targetName) { $registryDocument['packs'] += $targetName }
        $installed.Add($targetName)
        Write-Host "Installed $currentRegion -> $target" -ForegroundColor Green
    }
    [IO.File]::WriteAllText($registryPath, (($registryDocument | ConvertTo-Json -Depth 8) + [Environment]::NewLine), [Text.UTF8Encoding]::new($false))
    Write-Host "Registered: $($installed -join ', ')"
    Write-Host "Run the application from $InstallRoot to test in game."
}
else {
    Write-Host 'Next: re-verify each pack with scripts/Test-KuroMapFeaturePack.ps1, or pass -InstallRoot to install into a runnable tree.'
}
