[CmdletBinding()]
param(
    # Where the no-baseline test build is assembled. Disposable and outside the source tree.
    [string]$RunRoot,
    # A built application to clone the binaries from. Its own Assets are never touched.
    [string]$BinaryRoot,
    # The staged Assets tree to clone. Defaults to the one beside the binaries, which carries
    # build-time inputs such as bundled-snapshot.json that the source tree does not have.
    [string]$AssetsRoot,
    # Emit a strict format 2 candidate snapshot beside the run root so the native preflight
    # can be run against the same tree at <RunRoot>.candidate.json.
    [switch]$WriteCandidateSnapshot,
    [string]$SourceRoot
)

# Assembles the "no base atlas" build that answers the open question from
# Docs/MapRegionRefactor-Handoff.md section 4: can the 150.3 MB legacy base library
# (Map_features.imf + Map_visual_index.imx) be dropped now that every region ships as its
# own pack, without changing locating behaviour in game?
#
# The run root is a faithful clone of the installed tree with exactly these differences:
#   * Assets/FeaturesDatas/Map_features.imf      removed
#   * Assets/FeaturesDatas/Map_visual_index.imx  removed
#   * Assets/Updates/baseline-files.json         NOT written
#   * the bundled snapshot no longer names a map-features package
#
# The second difference is deliberate rather than incidental: ResourceSnapshotContext.cpp
# only enforces "the baseline manifest lists the features OR a map-features package exists"
# when baseline-files.json is present, so omitting the file bypasses that rule without
# touching validation code. Neither removed file is named by the bundled snapshot, so
# dropping them needs no package-list edit; the map-features clause is handled anyway so
# the tree stays correct if one is ever added.
#
# Everything else is a hard link (files) or a directory junction (directories) into the
# installed tree, so the clone costs no disk space and the installed tree is never written.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

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

# Counting the tree that is about to be linked from, so an accidental delete through a link
# cannot pass unnoticed. Only file counts are compared; the clone never writes to the source.
function Get-SourceFileCount {
    $count = 0
    foreach ($directory in @($assetsRoot, (Join-Path $assetsRoot 'FeaturesDatas'), (Join-Path $assetsRoot 'KuroMap'))) {
        if (Test-Path -LiteralPath $directory) {
            $count += @(Get-ChildItem -LiteralPath $directory -Recurse -File -Force -ErrorAction SilentlyContinue).Count
        }
    }
    return $count
}
$sourceFilesBefore = Get-SourceFileCount

# Removed from the run root's FeaturesDatas: the whole point of this build.
$removedFeatureFiles = @('Map_features.imf', 'Map_visual_index.imx')
# Inside Updates: omitting this one is what keeps the strict snapshot validator out of the way.
$baselineManifestName = 'baseline-files.json'
# The bundled snapshot is rewritten in place, so it must be a real file rather than a link.
$bundledSnapshotName = 'bundled-snapshot.json'

function New-LinkedEntry([string]$LinkPath, [string]$TargetPath) {
    if (Test-Path -LiteralPath $LinkPath) { throw "Refusing to overwrite existing path: $LinkPath" }
    $target = Get-Item -LiteralPath $TargetPath -Force
    if ($target.PSIsContainer) { New-Item -ItemType Junction -Path $LinkPath -Target $target.FullName | Out-Null }
    else { New-Item -ItemType HardLink -Path $LinkPath -Target $target.FullName | Out-Null }
}

# 1. Binaries. Assets is rebuilt below, so it is skipped here.
[IO.Directory]::CreateDirectory($RunRoot) | Out-Null
$copiedBinaries = 0
$reusedBinaries = 0
foreach ($entry in @(Get-ChildItem -LiteralPath $BinaryRoot -Force)) {
    if ($entry.Name -eq 'Assets') { continue }
    $destination = Join-Path $RunRoot $entry.Name
    if ($entry.PSIsContainer) {
        if (Test-Path -LiteralPath $destination) { ++$reusedBinaries; continue }
        Copy-Item -LiteralPath $entry.FullName -Destination $destination -Recurse -Force
    }
    else {
        # Byte comparison is what actually matters here; the mtime changes on every relink.
        if ((Test-Path -LiteralPath $destination) -and
            (Get-Item -LiteralPath $destination).Length -eq $entry.Length -and
            [Linq.Enumerable]::SequenceEqual(
                [IO.File]::ReadAllBytes($entry.FullName), [IO.File]::ReadAllBytes($destination))) {
            ++$reusedBinaries
            continue
        }
        Copy-Item -LiteralPath $entry.FullName -Destination $destination -Force
    }
    ++$copiedBinaries
}
Write-Host "  binaries: $copiedBinaries copied, $reusedBinaries already current"

# 2. Assets. Each level is cloned separately: two of them drop an entry, and the FeaturesDatas,
#    Updates and KuroTilePacks levels are rebuilt as real directories instead of junctions so
#    that no path the runtime writes can ever point back into the installed tree.
function Copy-LinkedLevel([string]$Source, [string]$Destination, [string[]]$Skip = @()) {
    # A level that is filled in below must not stay a junction to the installed tree, or the
    # files written into it would land in the installation instead.
    if (Test-Path -LiteralPath $Destination) {
        $existing = Get-Item -LiteralPath $Destination -Force
        if ($existing.LinkType) { [IO.Directory]::Delete($existing.FullName, $false) }
    }
    [IO.Directory]::CreateDirectory($Destination) | Out-Null
    $linked = 0
    $leftOut = [Collections.Generic.List[string]]::new()
    foreach ($entry in @(Get-ChildItem -LiteralPath $Source -Force)) {
        if ($entry.Name -in $Skip) { $leftOut.Add($entry.Name); continue }
        New-LinkedEntry (Join-Path $Destination $entry.Name) $entry.FullName
        ++$linked
    }
    Write-Host ("  {0}: {1} linked, {2} left out{3}" -f
        $Destination.Substring($RunRoot.Length).TrimStart('\'), $linked, $leftOut.Count,
        $(if ($leftOut.Count) { " ($($leftOut -join ', '))" } else { '' }))
}

# Remove-Item -Recurse deletes a junction's contents in place instead of unlinking it, and
# Get-ChildItem -Recurse follows junctions, so a plain recursive delete of this tree would
# empty the installed Assets it was built from. Every reparse point is deleted as a link.
function Remove-LinkedTree([string]$Path) {
    foreach ($entry in @(Get-ChildItem -LiteralPath $Path -Force)) {
        if ($entry.LinkType) {
            if ($entry.PSIsContainer) { [IO.Directory]::Delete($entry.FullName, $false) }
            else { Remove-Item -LiteralPath $entry.FullName -Force }
            continue
        }
        if ($entry.PSIsContainer) { Remove-LinkedTree $entry.FullName }
        else { Remove-Item -LiteralPath $entry.FullName -Force }
    }
    Remove-Item -LiteralPath $Path -Force
}

$runAssets = Join-Path $RunRoot 'Assets'
if (Test-Path -LiteralPath $runAssets) { Remove-LinkedTree $runAssets }
Copy-LinkedLevel -Source $assetsRoot -Destination $runAssets `
    -Skip @('FeaturesDatas', 'Updates')
Copy-LinkedLevel -Source (Join-Path $assetsRoot 'FeaturesDatas') `
    -Destination (Join-Path $runAssets 'FeaturesDatas') `
    -Skip $removedFeatureFiles
Copy-LinkedLevel -Source (Join-Path $assetsRoot 'Updates') `
    -Destination (Join-Path $runAssets 'Updates') `
    -Skip @($baselineManifestName)
Copy-LinkedLevel -Source (Join-Path $assetsRoot 'FeaturesDatas/KuroTilePacks') `
    -Destination (Join-Path $runAssets 'FeaturesDatas/KuroTilePacks')

# The bundled snapshot is the one file the runtime rewrites in place, so it must be a real
# copy rather than a hard link into the installed tree.
$bundledSnapshot = Join-Path $runAssets "Updates/$bundledSnapshotName"
if (-not (Test-Path -LiteralPath $bundledSnapshot)) { throw "The run root has no bundled snapshot: $bundledSnapshot" }
if ((Get-Item -LiteralPath $bundledSnapshot).LinkType) {
    $content = [IO.File]::ReadAllText($bundledSnapshot)
    Remove-Item -LiteralPath $bundledSnapshot -Force
    [IO.File]::WriteAllText($bundledSnapshot, $content, [Text.UTF8Encoding]::new($false))
}

# 3. Assert the tree really is the no-base-atlas layout before anything else trusts it.
foreach ($name in $removedFeatureFiles) {
    $path = Join-Path $runAssets "FeaturesDatas/$name"
    if (Test-Path -LiteralPath $path) { throw "Removal failed, still present: $path" }
}
if (Test-Path -LiteralPath (Join-Path $runAssets "Updates/$baselineManifestName")) {
    throw "baseline-files.json must not exist in a no-base-atlas run root"
}
# candidate-packs.json is deliberately absent: no curated candidate pack ships any more.
foreach ($required in @('FeaturesDatas/IconTask_Features.yml', 'FeaturesDatas/IconWavePlateCrystal_Features.yml',
    'KuroMap/scene-validation.json', 'Updates/bundled-snapshot.json')) {
    if (-not (Test-Path -LiteralPath (Join-Path $runAssets $required))) {
        throw "The no-base-atlas run root is missing a required non-package file: $required"
    }
}

# 4. Rewrite the bundled snapshot's package list: drop the map-features entries whose
#    directory is now gone and clear the root so MapFeatureRoot() falls back to
#    BaselineRoot()/FeaturesDatas, which is where the removed files used to live.
$snapshotPath = Join-Path $runAssets "Updates/$bundledSnapshotName"
$snapshot = Get-Content -LiteralPath $snapshotPath -Raw -Encoding UTF8 | ConvertFrom-Json
$kept = [Collections.Generic.List[object]]::new()
$dropped = [Collections.Generic.List[string]]::new()
foreach ($package in @($snapshot.packages)) {
    if ([string]$package.kind -eq 'map-features') { $dropped.Add([string]$package.id); continue }
    if (-not (Test-Path -LiteralPath (Join-Path $runAssets ([string]$package.directory)))) {
        throw "The bundled snapshot names a missing directory: $($package.id) -> $($package.directory)"
    }
    $kept.Add($package)
}
$snapshot.packages = @($kept)
if ($snapshot.PSObject.Properties.Name -contains 'mapFeatureRoot') { $snapshot.mapFeatureRoot = '' }
[IO.File]::WriteAllText($snapshotPath, (($snapshot | ConvertTo-Json -Depth 10) + [Environment]::NewLine),
    [Text.UTF8Encoding]::new($false))
$tilePackCount = @($snapshot.packages | Where-Object { $_.kind -eq 'tile' }).Count
if ($tilePackCount -eq 0) { throw 'The rewritten bundled snapshot names no tile package.' }
Write-Host ("  bundled snapshot: {0} packages kept ({1} tile), map-features dropped{2}" -f
    @($snapshot.packages).Count, $tilePackCount, $(if ($dropped.Count) { ": $($dropped -join ', ')" } else { ' (none were named)' }))

# 5. Optional strict candidate snapshot. The bundled descriptor is format 1 and validates
#    in non-strict mode only, so the native preflight (which is what actually loads the
#    resources and reports visualReady/viewportReady) needs a format 2 descriptor that
#    passes ResourceSnapshotValidation::Validate with bundled=false.
$candidatePath = $null
if ($WriteCandidateSnapshot) {
    $version = (Get-Content -LiteralPath (Join-Path $BinaryRoot 'build-info.json') -Raw -Encoding UTF8 | ConvertFrom-Json).appVersion
    $packages = [Collections.Generic.List[object]]::new()
    foreach ($package in @($snapshot.packages)) {
        $directory = Join-Path $runAssets ([string]$package.directory)
        $files = [Collections.Generic.List[object]]::new()
        foreach ($file in @(Get-ChildItem -LiteralPath $directory -Recurse -File -Force | Sort-Object FullName)) {
            $relative = $file.FullName.Substring($directory.Length).TrimStart('\').Replace('\', '/')
            $files.Add([ordered]@{
                path = $relative
                size = $file.Length
                sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            })
        }
        $packages.Add([ordered]@{
            id = [string]$package.id
            version = [string]$package.version
            kind = [string]$package.kind
            directory = $directory
            sha256 = (Get-FileHash -LiteralPath (Join-Path $directory 'manifest.json') -Algorithm SHA256).Hash.ToLowerInvariant()
            files = @($files)
        })
    }
    $candidate = [ordered]@{
        formatVersion = 2
        snapshotId = "no-baseline-preflight-$(Get-Date -Format yyyyMMddHHmmss)"
        sequence = 0
        baselineId = [string]$snapshot.baselineId
        minAppVersion = [string]$version
        baselineRoot = $runAssets
        mapDataRoot = Join-Path $runAssets 'KuroMap'
        mapIconRoot = ''
        mapFeatureRoot = ''
        bundled = $false
        packages = @($packages)
    }
    $candidatePath = "$RunRoot.candidate.json"
    [IO.File]::WriteAllText($candidatePath, (($candidate | ConvertTo-Json -Depth 10) + [Environment]::NewLine),
        [Text.UTF8Encoding]::new($false))
    Write-Host ("  strict candidate snapshot: {0} ({1} packages, {2} files hashed)" -f
        $candidatePath, $packages.Count, (@($packages | ForEach-Object { @($_.files).Count }) | Measure-Object -Sum).Sum)
}

Write-Host ''
$sourceFilesAfter = Get-SourceFileCount
if ($sourceFilesAfter -ne $sourceFilesBefore) {
    throw "The cloned Assets tree lost files: $sourceFilesBefore -> $sourceFilesAfter"
}
Write-Host "No-base-atlas test run root ready: $RunRoot" -ForegroundColor Green
Write-Host "  binaries cloned from : $BinaryRoot (its Assets untouched)"
Write-Host "  Assets cloned from   : $assetsRoot (hard links, $sourceFilesAfter files intact)"
Write-Host "  base atlas removed   : $($removedFeatureFiles -join ', ')"
Write-Host "  $baselineManifestName : absent by design (skips the features-must-be-listed rule)"
Write-Host "  tile packages named  : $tilePackCount"
if ($candidatePath) { Write-Host "  preflight snapshot   : $candidatePath" }
Write-Host "Launch: $RunRoot\IMao-WinUI.exe  (run as administrator, game open, 16:9)"
