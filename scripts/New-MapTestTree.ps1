[CmdletBinding()]
param(
    # Where the test build is assembled. Disposable and outside the source tree.
    [string]$RunRoot,
    # A built application to copy the binaries from. Its own Assets are never touched.
    [string]$BinaryRoot,
    # The staged Assets tree to clone. Defaults to the one beside the binaries, which carries
    # build-time inputs such as bundled-snapshot.json that the source tree does not have.
    [string]$AssetsRoot,
    # Comma-separated, because -File invocation cannot bind a string array.
    [Parameter(Mandatory = $true)][string]$PackRegionId,
    # Shipped pack directories to leave out of the run root, for a region whose replacement
    # covers ground the old packs also covered. Registering both would duplicate every
    # keypoint over that ground.
    [string]$ExcludePackDir,
    # Copy the binaries even when the run root already has them. They are locked while the
    # application is running, and only Assets change between rebuilds.
    [switch]$RefreshBinaries,
    [string]$SourceRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# A development output usually junctions Assets/ to the source tree, so installing rebuilt
# packs through it would rewrite LFS-tracked source files. This assembles a separate run
# root instead: binaries copied, untouched Assets entries linked, rebuilt packs real. The
# source tree and the existing build output are left exactly as they were.
if (-not $SourceRoot) { $SourceRoot = Split-Path -Parent $PSScriptRoot }
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
if (-not $RunRoot) { $RunRoot = Join-Path $SourceRoot 'out/map-test' }
if (-not $BinaryRoot) { $BinaryRoot = Join-Path $SourceRoot 'x64/Release' }
$RunRoot = [IO.Path]::GetFullPath($RunRoot)
$BinaryRoot = [IO.Path]::GetFullPath($BinaryRoot)
$assetsRoot = if ($AssetsRoot) { [IO.Path]::GetFullPath($AssetsRoot) } else { Join-Path $BinaryRoot 'Assets' }

if (-not (Test-Path -LiteralPath (Join-Path $BinaryRoot 'IMao-WinUI.exe'))) {
    throw "BinaryRoot does not contain IMao-WinUI.exe: $BinaryRoot"
}
if (-not (Test-Path -LiteralPath $assetsRoot)) { throw "AssetsRoot is missing: $assetsRoot" }
if ($RunRoot.StartsWith($assetsRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "RunRoot must not be inside Assets: $RunRoot"
}

function New-LinkedEntry([string]$LinkPath, [string]$TargetPath) {
    if (Test-Path -LiteralPath $LinkPath) { throw "Refusing to overwrite existing path: $LinkPath" }
    $target = Get-Item -LiteralPath $TargetPath -Force
    if ($target.PSIsContainer) { New-Item -ItemType Junction -Path $LinkPath -Target $target.FullName | Out-Null }
    else { New-Item -ItemType HardLink -Path $LinkPath -Target $target.FullName | Out-Null }
}

# 1. Binaries. Assets is skipped: it is rebuilt below, and in a dev output it is a junction.
[IO.Directory]::CreateDirectory($RunRoot) | Out-Null
# Idempotent: the binaries only change when the build output does, and re-copying them
# fails while the application is running from the run root.
$copiedBinaries = 0
$skippedBinaries = 0
foreach ($entry in @(Get-ChildItem -LiteralPath $BinaryRoot -Force)) {
    if ($entry.Name -eq 'Assets') { continue }
    $destination = Join-Path $RunRoot $entry.Name
    if (-not $RefreshBinaries) {
        if ($entry.PSIsContainer) { if (Test-Path -LiteralPath $destination) { ++$skippedBinaries; continue } }
        elseif ((Test-Path -LiteralPath $destination) -and (Get-Item -LiteralPath $destination).Length -eq $entry.Length) { ++$skippedBinaries; continue }
    }
    if ($entry.PSIsContainer) { Copy-Item -LiteralPath $entry.FullName -Destination $destination -Recurse -Force }
    else { Copy-Item -LiteralPath $entry.FullName -Destination $destination -Force }
    ++$copiedBinaries
}
Write-Host "  binaries: $copiedBinaries copied, $skippedBinaries already present"

# 2. Assets. Everything links to the source tree except the two levels that must be
#    replaceable: FeaturesDatas/KuroTilePacks and FeaturesDatas/kuro-tile-packs.json.
$runAssets = Join-Path $RunRoot 'Assets'
if (Test-Path -LiteralPath $runAssets) { Remove-Item -LiteralPath $runAssets -Recurse -Force }
[IO.Directory]::CreateDirectory($runAssets) | Out-Null
foreach ($entry in @(Get-ChildItem -LiteralPath $assetsRoot -Force)) {
    if ($entry.Name -eq 'FeaturesDatas') { continue }
    New-LinkedEntry (Join-Path $runAssets $entry.Name) $entry.FullName
}
$runFeatureDatas = Join-Path $runAssets 'FeaturesDatas'
[IO.Directory]::CreateDirectory($runFeatureDatas) | Out-Null
foreach ($entry in @(Get-ChildItem -LiteralPath (Join-Path $assetsRoot 'FeaturesDatas') -Force)) {
    if ($entry.Name -in @('KuroTilePacks', 'kuro-tile-packs.json')) { continue }
    New-LinkedEntry (Join-Path $runFeatureDatas $entry.Name) $entry.FullName
}

# 3. Packs: link the ones we are not replacing, install the rebuilt ones as real files.
$sourcePacks = Join-Path $assetsRoot 'FeaturesDatas/KuroTilePacks'
$runPacks = Join-Path $runFeatureDatas 'KuroTilePacks'
[IO.Directory]::CreateDirectory($runPacks) | Out-Null
$packRegions = @($PackRegionId -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
if ($packRegions.Count -eq 0) { throw 'PackRegionId is empty.' }
$wanted = [Collections.Generic.HashSet[string]]::new([string[]]$packRegions, [StringComparer]::OrdinalIgnoreCase)
$excluded = @($ExcludePackDir -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$replaced = [Collections.Generic.List[string]]::new()
foreach ($entry in @(Get-ChildItem -LiteralPath $sourcePacks -Force)) {
    if ($wanted.Contains($entry.Name) -or $excluded -contains $entry.Name) { continue }
    New-LinkedEntry (Join-Path $runPacks $entry.Name) $entry.FullName
}
foreach ($region in $packRegions) {
    $built = Join-Path $SourceRoot "out/map-regions/packs/$region"
    if (-not (Test-Path -LiteralPath (Join-Path $built 'manifest.json'))) { throw "No built pack for region $region at $built" }
    $verified = (Get-Content -LiteralPath (Join-Path $built 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json).referenceVerification.passed
    $shipped = @(Get-ChildItem -LiteralPath $sourcePacks -Directory | Where-Object { $_.Name -ieq $region })
    $targetName = if ($shipped.Count -eq 1) { $shipped[0].Name } else { $region }
    $target = Join-Path $runPacks $targetName
    [IO.Directory]::CreateDirectory($target) | Out-Null
    Copy-Item -Path (Join-Path $built '*') -Destination $target -Recurse -Force
    if ($shipped.Count -eq 1) {
        $reference = Join-Path $shipped[0].FullName 'reference-minimap.png'
        if (Test-Path -LiteralPath $reference -PathType Leaf) { Copy-Item -LiteralPath $reference -Destination $target -Force }
    }
    $replaced.Add($targetName)
    Write-Host ("Installed {0} -> {1} (referenceVerification.passed={2})" -f $region, $target, $verified)
}

# 4. The runtime does not read kuro-tile-packs.json when CoreHost is started with
#    --resource-snapshot, which it always is: LoadRegistered walks the snapshot's own
#    package list instead. A package whose directory is missing fails to load, and a
#    failed package aborts the entire resource load, so the core never becomes ready.
#    Every directory the snapshot names must therefore exist.
$snapshotPath = Join-Path $runAssets 'Updates/bundled-snapshot.json'
if (-not (Test-Path -LiteralPath $snapshotPath)) { throw "The run root has no bundled snapshot: $snapshotPath" }
$snapshot = Get-Content -LiteralPath $snapshotPath -Raw -Encoding UTF8 | ConvertFrom-Json
$missing = [Collections.Generic.List[string]]::new()
$snapshotPackDirs = [Collections.Generic.List[string]]::new()
foreach ($package in @($snapshot.packages)) {
    if ([string]::IsNullOrWhiteSpace([string]$package.directory)) { continue }
    if ([string]$package.kind -eq 'tile') { $snapshotPackDirs.Add(($package.directory -split '/')[-1]) }
    if (-not (Test-Path -LiteralPath (Join-Path $runAssets ([string]$package.directory)))) {
        $missing.Add("$($package.id) -> $($package.directory)")
    }
}
if ($missing.Count -gt 0) {
    # The rewrite below drops exactly these; the check after it must find nothing missing.
    Write-Host "  snapshot names $($missing.Count) not-installed directory/ies: $($missing -join ', ')"
}

# 5. Rewrite the snapshot's package list. CoreHost is always started with
#    --resource-snapshot, so LoadRegistered walks this list and never reads
#    kuro-tile-packs.json: a pack that is not named here never loads. New regions must be
#    added and packs whose directory was left out must be dropped. snapshotId is preserved
#    so the activation record already on disk still validates and no user data is cleared.
$kept = [Collections.Generic.List[object]]::new()
$named = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($package in @($snapshot.packages)) {
    if ([string]::IsNullOrWhiteSpace([string]$package.directory)) { continue }
    if (-not (Test-Path -LiteralPath (Join-Path $runAssets ([string]$package.directory)))) {
        Write-Host "  dropped from snapshot: $($package.id)"
        continue
    }
    $kept.Add($package)
    if ([string]$package.kind -eq 'tile') { [void]$named.Add(($package.directory -split '/')[-1]) }
}
$template = @($kept | Where-Object { [string]$_.kind -eq 'tile' })
if ($template.Count -eq 0) { throw 'The snapshot names no tile package to model a new entry on.' }
foreach ($entry in @(Get-ChildItem -LiteralPath $runPacks -Directory | Sort-Object Name)) {
    if ($named.Contains($entry.Name)) { continue }
    $packManifest = Get-Content -LiteralPath (Join-Path $entry.FullName 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    # Clone the shape of a known-good tile entry so every format the native reader
    # validates stays valid; only identity and location change.
    $added = $template[0] | Select-Object *
    $added.id = [string]$packManifest.packId
    $added.directory = "FeaturesDatas/KuroTilePacks/$($entry.Name)"
    $kept.Add($added)
    [void]$named.Add($entry.Name)
    Write-Host "  added to snapshot: $($packManifest.packId) -> $($entry.Name)"
}
$snapshot.packages = @($kept)
[IO.File]::WriteAllText($snapshotPath, (($snapshot | ConvertTo-Json -Depth 10) + [Environment]::NewLine), [Text.UTF8Encoding]::new($false))

$snapshotPackDirs.Clear()
foreach ($package in @($snapshot.packages)) {
    if ([string]$package.kind -eq 'tile') { $snapshotPackDirs.Add(($package.directory -split '/')[-1]) }
    if (-not (Test-Path -LiteralPath (Join-Path $runAssets ([string]$package.directory)))) {
        throw "The rewritten snapshot still names a missing directory: $($package.id) -> $($package.directory)"
    }
}

Write-Host ''
# 6. Registry, derived from the same list for the path where no snapshot is passed.
$registryPackNames = @($snapshotPackDirs | Sort-Object -Unique)
$registry = [ordered]@{
    formatVersion = 1
    packs = @($registryPackNames)
}
$registryPath = Join-Path $runFeatureDatas 'kuro-tile-packs.json'
[IO.File]::WriteAllText($registryPath, (($registry | ConvertTo-Json -Depth 4) + [Environment]::NewLine), [Text.UTF8Encoding]::new($false))

Write-Host "Test run root ready: $RunRoot" -ForegroundColor Green
Write-Host "  binaries copied from : $BinaryRoot (its Assets untouched)"
Write-Host "  rebuilt packs        : $($replaced -join ', ')"
Write-Host "  excluded old packs   : $($excluded -join ', ')"
Write-Host "  snapshot tile packs  : $($snapshotPackDirs -join ', ')   <- what the runtime actually loads"
Write-Host "  Assets cloned from   : $assetsRoot (untouched)"
Write-Host "Launch: $RunRoot\IMao-WinUI.exe"
