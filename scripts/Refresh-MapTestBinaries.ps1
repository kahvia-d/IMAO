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
# the mask in every KuroTilePacks/<region>/layered-floors/floor-index.json and the deliberately
# absent Assets/Updates/baseline-files.json of the no-base test tree - is left exactly as it is.
#
#   pwsh -File scripts\Refresh-MapTestBinaries.ps1              # copy, then report
#   pwsh -File scripts\Refresh-MapTestBinaries.ps1 -DryRun      # report only
#
# Launch the result with (administrator, game running, 16:9):
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

if (-not (Test-Path -LiteralPath $RunRoot)) { throw "run root does not exist: $RunRoot" }
if (-not (Test-Path -LiteralPath $binaryRoot)) { throw "build output does not exist: $binaryRoot" }

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

# The runtime reaches the layered indices through <run root>/Assets/FeaturesDatas/KuroTilePacks, so
# resolve them from there and confirm every floor carries a mask of exactly the length Load demands.
# A mask that has drifted is dropped by DecodeOwnMask, which quietly returns the tree to the old
# behaviour - worth eyeballing before a test run rather than after.
Write-Host ''
Write-Host 'layered-floor indices reachable from the run root:'
$packsRoot = Join-Path $RunRoot 'Assets/FeaturesDatas/KuroTilePacks'
$totalFloors = 0
$totalMasked = 0
$totalExact = 0
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
    $totalFloors += $floors.Count
    $totalMasked += $masked
    $totalExact += $exact
    Write-Host ('  {0,-14} floors={1,2}  with mask={2,2}  exact length={3,2}{4}' -f `
            $region.Name, $floors.Count, $masked, $exact,
        $(if ($exact -eq $floors.Count) { '' } else { '   <-- CHECK' }))
}
Write-Host ('  {0,-14} floors={1,2}  with mask={2,2}  exact length={3,2}' -f 'TOTAL', $totalFloors, $totalMasked, $totalExact)
if ($totalExact -ne $totalFloors) {
    Write-Warning 'some floors have no usable own-art mask; those floors will behave as they did before the fix.'
}
Write-Host ''
Write-Host 'Launch with: out\map-test\IMao-WinUI.exe   (administrator, game running, 16:9)'
