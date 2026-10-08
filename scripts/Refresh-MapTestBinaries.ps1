# Refreshes the maptest run root's binaries from the build output, WITHOUT rebuilding the tree.
#
# Why this exists instead of just re-running New-MapTestTree.ps1
# -------------------------------------------------------------
# New-MapTestTree.ps1 rebuilds `Assets/` from scratch: it links the shared tree and then REPLACES
# every pack named in -PackRegionId with the one under `out/map-regions/packs/<region>`. Those
# rebuilt packs are older than the layered-floor indices that live in the shared Assets tree, so
# re-running it installs a `floor-index.json` with no own-art mask over the good one - which silently
# turns the layered-floor fix off in exactly the tree you are testing it in. (It also cannot be run
# without -PackRegionId, because that parameter is mandatory.)
#
# This script copies only binaries, and only the ones that actually differ, so `Assets/` - including
# the deliberately absent Assets/Updates/baseline-files.json of the no-base test tree - is left as it
# is. The one exception is the layered-floor sidecar data (`<region>/layered-floors/`), which is
# synced from the shared tree below: that data is edited in the repo while the run root holds a COPY,
# so without the sync a pack fix lands in Assets and the tree under test keeps the broken version -
# which is what happened to 隐海试验场's coordinate transform on 2026-09-26.
#
#   pwsh -File scripts\Refresh-MapTestBinaries.ps1              # copy, then report
#   pwsh -File scripts\Refresh-MapTestBinaries.ps1 -DryRun      # report only
#
# Launch the result with (administrator, game running; any client aspect ratio, the HUD crops follow
# the game's own scale and anchors):
#   out\map-test\IMao-WinUI.exe

[CmdletBinding()]
param(
    [string]$SourceRoot,
    [string]$RunRoot,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $SourceRoot) { $SourceRoot = Split-Path -Parent $PSScriptRoot }
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
if (-not $RunRoot) { $RunRoot = Join-Path $SourceRoot 'out/map-test' }
$binaryRoot = Join-Path $SourceRoot 'x64/Release'
$packsRoot = Join-Path $RunRoot 'Assets/FeaturesDatas/KuroTilePacks'
$sharedPacksRoot = Join-Path $SourceRoot 'Assets/FeaturesDatas/KuroTilePacks'

if (-not (Test-Path -LiteralPath $RunRoot)) { throw "run root does not exist: $RunRoot" }
if (-not (Test-Path -LiteralPath $binaryRoot)) { throw "build output does not exist: $binaryRoot" }

# A framework-dependent build must never reach this tree. `IMao-WinUI.csproj` sets neither
# <SelfContained> nor <RuntimeIdentifier>, so a plain `dotnet build -r win-x64` - the command you reach
# for just to check that a change compiles - writes a runtimeconfig.json whose only framework entry is
# Microsoft.NETCore.App. Copying it (plus the trimmed deps.json) over the run root replaces the
# self-contained pair and the client dies at startup with "You must install or update .NET". That has
# happened four times, each time because a throwaway compile-check build landed in x64/Release - the
# fourth on 2026-09-26 came from a build run only to verify a one-line change, which then got copied
# into the run root by this very script. So refuse the source outright instead of trusting the copies.
$runtimeConfigPath = Join-Path $binaryRoot 'IMao-WinUI.runtimeconfig.json'
if (-not (Test-Path -LiteralPath $runtimeConfigPath)) { throw "no runtimeconfig.json in $binaryRoot; build IMao-WinUI first" }
$runtimeOptions = (Get-Content -LiteralPath $runtimeConfigPath -Raw -Encoding UTF8 | ConvertFrom-Json).runtimeOptions
if (-not ($runtimeOptions.PSObject.Properties.Name -contains 'includedFrameworks')) {
    throw ("$runtimeConfigPath is framework-dependent (it names the Microsoft.NETCore.App framework and not " +
        "includedFrameworks), so the build output cannot start on a machine without .NET installed - and copying it " +
        "here would break the run root. Rebuild the shell self-contained first: " +
        "dotnet build IMao-WinUI\IMao-WinUI.csproj -c Release -r win-x64 --self-contained true -p:Platform=x64 -t:Rebuild")
}

$running = @(Get-Process -Name 'IMao-CoreHost', 'IMao-WinUI' -ErrorAction SilentlyContinue)
if ($running.Count -gt 0 -and -not $DryRun) {
    throw ("the test tree is running (pid {0}); close it first, its binaries are locked" -f ($running.Id -join ', '))
}

# Only top-level FILES: directories (and therefore Assets/) are never touched.
$copied = New-Object System.Collections.ArrayList
foreach ($entry in @(Get-ChildItem -LiteralPath $binaryRoot -Force -File)) {
    $destination = Join-Path $RunRoot $entry.Name
    if (-not (Test-Path -LiteralPath $destination)) { continue }
    $existing = Get-Item -LiteralPath $destination -Force
    if ($entry.LastWriteTimeUtc -le $existing.LastWriteTimeUtc -and $entry.Length -eq $existing.Length) { continue }
    if (-not $DryRun) { Copy-Item -LiteralPath $entry.FullName -Destination $destination -Force }
    [void]$copied.Add([pscustomobject]@{
            Name = $entry.Name
            From = $existing.LastWriteTime.ToString('MM-dd HH:mm:ss')
            To = $entry.LastWriteTime.ToString('MM-dd HH:mm:ss')
            Size = ('{0} -> {1}' -f $existing.Length, $entry.Length)
        })
}

if ($copied.Count -eq 0) { Write-Host 'Nothing to refresh: the run root already matches the build output.' }
else {
    Write-Host ('{0} {1} file(s):' -f $(if ($DryRun) { 'would refresh' } else { 'refreshed' }), $copied.Count)
    $copied | Format-Table -AutoSize
}

# The region shards name one shared vocabulary by hash instead of each carrying their own 2.00 MB
# copy. That file lives under Assets/, which the loop above deliberately never touches, so a run
# root built before it existed holds shards it cannot read - and refreshing only the binaries would
# leave it that way, with the new CoreHost reporting "carries no vocabulary". Place it whenever a
# shard in this root actually needs it, then confirm it is the same one the build produced.
function Test-ShardNamesSharedVocabulary([string]$ShardPath) {
    if (-not (Test-Path -LiteralPath $ShardPath -PathType Leaf)) { return $false }
    $stream = [IO.File]::OpenRead($ShardPath)
    try {
        if ($stream.Length -lt 192) { return $false }
        $header = New-Object byte[] 192
        if ($stream.Read($header, 0, 192) -ne 192) { return $false }
    }
    finally { $stream.Dispose() }
    # vocabularyPayloadLength is the first uint64 after magic(8) + twelve uint32 header fields.
    return ([BitConverter]::ToUInt64($header, 56) -eq 0)
}
$vocabularyName = 'Map_visual_vocabulary.imx'
$stagedVocabulary = Join-Path $binaryRoot "Assets/FeaturesDatas/$vocabularyName"
$runVocabulary = Join-Path $RunRoot "Assets/FeaturesDatas/$vocabularyName"
$needsVocabulary = $false
foreach ($region in @(Get-ChildItem -LiteralPath $packsRoot -Directory -ErrorAction SilentlyContinue)) {
    if (Test-ShardNamesSharedVocabulary (Join-Path $region.FullName 'visual-index.imx')) {
        $needsVocabulary = $true
        break
    }
}
if ($needsVocabulary) {
    if (-not (Test-Path -LiteralPath $stagedVocabulary -PathType Leaf)) {
        throw ("Shards in this run root name a shared vocabulary but the build output has none: " +
            "$stagedVocabulary. Build IMao-WinUI first, or the packs cannot be read at all.")
    }
    $current = Test-Path -LiteralPath $runVocabulary -PathType Leaf
    $matches = $current -and (Get-Item -LiteralPath $runVocabulary).Length -eq (Get-Item -LiteralPath $stagedVocabulary).Length -and
        (Get-FileHash -LiteralPath $runVocabulary -Algorithm SHA256).Hash -eq (Get-FileHash -LiteralPath $stagedVocabulary -Algorithm SHA256).Hash
    if ($matches) {
        Write-Host 'Shared visual vocabulary already matches the build output.'
    }
    else {
        if (-not $DryRun) { Copy-Item -LiteralPath $stagedVocabulary -Destination $runVocabulary -Force }
        Write-Host ('Shared visual vocabulary {0} the run root.' -f
            $(if ($DryRun) { 'would be placed into' } else { 'placed into' }))
    }
}

# The layered-floor sidecars are DATA, not build output: they are edited in the shared Assets tree and
# the run root holds a copy of it, so a file newer there means the tree under test is stale. Only the
# regions already present in the run root are touched, and only under layered-floors/ - nothing here
# can install a pack, and a region the run root deliberately does not have stays absent.
$synced = New-Object System.Collections.ArrayList
foreach ($region in @(Get-ChildItem -LiteralPath $packsRoot -Directory -ErrorAction SilentlyContinue | Sort-Object Name)) {
    $sharedRegion = Join-Path $sharedPacksRoot $region.Name
    if (-not (Test-Path -LiteralPath $sharedRegion)) { continue }
    foreach ($file in @(Get-ChildItem -LiteralPath (Join-Path $sharedRegion 'layered-floors') -Recurse -File -ErrorAction SilentlyContinue)) {
        $relative = $file.FullName.Substring($sharedRegion.Length).TrimStart('\', '/')
        $destination = Join-Path $region.FullName $relative
        $previousLength = 0
        if (Test-Path -LiteralPath $destination) {
            $existing = Get-Item -LiteralPath $destination -Force
            if ($file.LastWriteTimeUtc -le $existing.LastWriteTimeUtc -and $file.Length -eq $existing.Length) { continue }
            $previousLength = $existing.Length
        }
        $parent = Split-Path -Parent $destination
        if (-not (Test-Path -LiteralPath $parent)) { if (-not $DryRun) { New-Item -ItemType Directory -Force -Path $parent | Out-Null } }
        if (-not $DryRun) { Copy-Item -LiteralPath $file.FullName -Destination $destination -Force }
        [void]$synced.Add([pscustomobject]@{
                Region = $region.Name
                File = $relative
                Size = ('{0} -> {1}' -f $previousLength, $file.Length)
            })
    }
}
if ($synced.Count -eq 0) { Write-Host 'Layered-floor sidecar data already matches the shared Assets tree.' }
else {
    Write-Host ('{0} layered-floor data file(s) from the shared Assets tree:' -f $(if ($DryRun) { 'would sync' } else { 'synced' }), $synced.Count)
    $synced | Format-Table -AutoSize
}

# The runtime reaches the layered indices through <run root>/Assets/FeaturesDatas/KuroTilePacks, so
# resolve them from there and confirm every floor carries a mask of exactly the length Load demands.
# A mask that has drifted is dropped by DecodeOwnMask, which quietly returns the tree to the old
# behaviour - worth eyeballing before a test run rather than after. The frame column is the other
# silent one: an index whose transform is in the wrong frame identifies floors perfectly and can
# never adopt one, because nothing the player stands on is ever inside its footprint.
. (Join-Path $PSScriptRoot 'SceneCoordinateTransform.ps1')
Write-Host ''
Write-Host 'layered-floor indices reachable from the run root:'
$totalFloors = 0
$totalMasked = 0
$totalExact = 0
$totalIndexes = 0
$totalFramed = 0
foreach ($region in @(Get-ChildItem -LiteralPath $packsRoot -Directory -ErrorAction SilentlyContinue | Sort-Object Name)) {
    $indexPath = Join-Path $region.FullName 'layered-floors/floor-index.json'
    if (-not (Test-Path -LiteralPath $indexPath)) { continue }
    $index = Get-Content -LiteralPath $indexPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $floors = @($index.floors)
    $masked = 0
    $exact = 0
    foreach ($floor in $floors) {
        if (-not ($floor.PSObject.Properties.Name -contains 'ownMask') -or -not $floor.ownMask) { continue }
        ++$masked
        if ($floor.ownMask.Length -eq [Math]::Ceiling([int]$floor.ownMaskKeypoints / 4.0)) { ++$exact }
    }
    $frameNote = ''
    try {
        $expected = Get-SceneCoordinateTransform -SourceRoot $SourceRoot -Frame ([int]$index.frame)
        $matchesFrame = ([Math]::Abs([double]$index.coordinateTransform.originX - $expected.OriginX) -le 0.001) -and
            ([Math]::Abs([double]$index.coordinateTransform.originY - $expected.OriginY) -le 0.001) -and
            ([Math]::Abs([double]$index.coordinateTransform.scale - $expected.Scale) -le 0.001)
        if ($matchesFrame) { ++$totalFramed; $frameNote = 'ok' }
        else {
            $frameNote = ('WRONG FRAME -> want ({0},{1})' -f $expected.OriginX, $expected.OriginY)
        }
    }
    catch { $frameNote = "UNRESOLVED: $($_.Exception.Message)" }
    $totalFloors += $floors.Count
    $totalMasked += $masked
    $totalExact += $exact
    ++$totalIndexes
    Write-Host ('  {0,-14} frame={1,-4} floors={2,2}  with mask={3,2}  exact length={4,2}  transform={5}{6}' -f `
            $region.Name, $index.frame, $floors.Count, $masked, $exact, $frameNote,
        $(if ($exact -eq $floors.Count) { '' } else { '   <-- CHECK' }))
}
Write-Host ('  {0,-14} {1,11} floors={2,2}  with mask={3,2}  exact length={4,2}  transforms={5}/{6}' -f `
        'TOTAL', '', $totalFloors, $totalMasked, $totalExact, $totalFramed, $totalIndexes)
if ($totalExact -ne $totalFloors) {
    Write-Warning 'some floors have no usable own-art mask; those floors will behave as they did before the fix.'
}
if ($totalFramed -ne $totalIndexes) {
    Write-Warning 'a layered-floor index is in the wrong coordinate frame; no player position can ever be inside its footprint.'
}
Write-Host ''
# The pair that decides whether the tree starts at all: the run root must hold a self-contained
# runtimeconfig plus the managed runtime it names. Checked here so a bad refresh is reported by this
# script instead of by the Windows event log after somebody double-clicks the client.
$runConfigPath = Join-Path $RunRoot 'IMao-WinUI.runtimeconfig.json'
$runOptions = (Get-Content -LiteralPath $runConfigPath -Raw -Encoding UTF8 | ConvertFrom-Json).runtimeOptions
$runSelfContained = $runOptions.PSObject.Properties.Name -contains 'includedFrameworks'
$runtimeFiles = @('hostpolicy.dll', 'coreclr.dll', 'System.Private.CoreLib.dll')
$missingRuntime = @($runtimeFiles | Where-Object { -not (Test-Path -LiteralPath (Join-Path $RunRoot $_)) })
if (-not $runSelfContained -or $missingRuntime.Count -gt 0) {
    Write-Warning ('the run root will not start: self-contained={0}, missing runtime files: {1}' -f `
            $runSelfContained, $(if ($missingRuntime.Count -gt 0) { $missingRuntime -join ', ' } else { 'none' }))
}
else {
    Write-Host ('run root runtime: self-contained ({0} frameworks), hostpolicy/coreclr/CoreLib present' -f `
            $runOptions.includedFrameworks.Count)
}

# A pack that is installed but not named by the snapshot is a pack the runtime will never load,
# because KuroTileFeaturePack::LoadRegistered walks snapshot.packages and never reads
# kuro-tile-packs.json. That is invisible from every other angle - the directory is there, the
# registry names it, the layered indices resolve - so it is checked here, where it is cheap.
# 2026-09-30: a staging run rewrote the shared Assets/Updates/bundled-snapshot.json through the
# junction out/map-test/Assets/Updates, 梦枢天罗's pack fell out of the snapshot, and the client
# localized nothing at all while looking perfectly healthy.
$runSnapshotPath = Join-Path $RunRoot 'Assets/Updates/bundled-snapshot.json'
$snapshotTileDirs = @()
if (Test-Path -LiteralPath $runSnapshotPath) {
    $snapshot = Get-Content -LiteralPath $runSnapshotPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $snapshotTileDirs = @($snapshot.packages | Where-Object { [string]$_.kind -eq 'tile' } |
        ForEach-Object { ($_.directory -split '/')[-1] } | Sort-Object -Unique)
    $installedDirs = @(Get-ChildItem -LiteralPath $packsRoot -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { $_.Name } | Sort-Object -Unique)
    $notLoaded = @($installedDirs | Where-Object { $snapshotTileDirs -notcontains $_ })
    Write-Host ''
    Write-Host ('snapshot tile packs ({0}): {1}' -f $snapshotTileDirs.Count, ($snapshotTileDirs -join ', '))
    if ($notLoaded.Count -gt 0) {
        Write-Warning ('installed but NOT in the snapshot, so the runtime will never load: {0}' -f ($notLoaded -join ', '))
        Write-Warning ('  fix: pwsh -File scripts\New-MapTestTree.ps1 -PackRegionId {0} -IsolateTrialState -ApproveScene <scene>' -f ($notLoaded -join ','))
    }
    $updates = Get-Item -LiteralPath (Join-Path $RunRoot 'Assets/Updates') -Force
    # A link is only a problem when THIS tree loads something staging would not keep: then the next
    # staging run replaces the snapshot through the link and the pack disappears. A tree whose packs
    # all come from the published registry is fine either way - after 2026-09-30's release the trial
    # no longer needs isolation, and warning about the link there was pure noise.
    $stagedRegistry = Join-Path $BinaryRoot 'Assets/FeaturesDatas/kuro-tile-packs.json'
    $stagedPacks = if (Test-Path -LiteralPath $stagedRegistry) {
        @((Get-Content -LiteralPath $stagedRegistry -Raw -Encoding UTF8 | ConvertFrom-Json).packs)
    } else { @() }
    $keptByStaging = @($snapshotTileDirs | Where-Object { $stagedPacks -contains $_ })
    if ($updates.LinkType -and $keptByStaging.Count -lt $snapshotTileDirs.Count) {
        $dropped = @($snapshotTileDirs | Where-Object { $stagedPacks -notcontains $_ })
        Write-Warning ('Assets/Updates is a {0} to the build output, and this tree loads packs the staged registry does not name ({1}): the next staging run replaces the snapshot through that link and they stop loading. Re-run New-MapTestTree.ps1 -IsolateTrialState to make this tree own it.' -f $updates.LinkType, ($dropped -join ', '))
    }
}
else { Write-Warning "the run root has no bundled snapshot: $runSnapshotPath" }
Write-Host 'Launch with: out\map-test\IMao-WinUI.exe   (administrator, game running; any client aspect ratio)'
