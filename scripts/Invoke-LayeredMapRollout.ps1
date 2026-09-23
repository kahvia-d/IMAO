# Rolls the layered-map ("分层地图") treatment out across every region that has layered maps.
#
# For each region: composite one image per floor (scripts/New-LayeredTileComposite.ps1) and
# build that region's per-floor descriptor index (scripts/New-LayeredFloorIndex.ps1). The pack
# rebuild that folds those appearances into the region pack is a separate step, because it also
# needs the region's anchor and reference minimap:
#
#   scripts/Sync-KuroMapFeaturePack.ps1 -LayeredCompositeDir out/map-regions/composite/<region>/k035
#
# Regions without layered maps (阿维纽林, 时隙废都) are reported and skipped, not treated as
# failures - country.json marks both haveLayer=false.
#
#   pwsh -File scripts/Invoke-LayeredMapRollout.ps1

[CmdletBinding()]
param(
    [string]$SourceRoot,
    # Comma-separated, because -File invocation cannot bind an array.
    [string]$RegionId = '',
    [ValidateSet('k100', 'k035')]
    [string]$Factor = 'k035',
    # Skip the per-floor feature extraction; only refresh the composited images.
    [switch]$CompositesOnly,
    [switch]$SkipExisting
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $SourceRoot) { $SourceRoot = Split-Path -Parent $PSScriptRoot }
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)

$registry = Get-Content -LiteralPath (Join-Path $SourceRoot 'map-regions/regions.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$regions = if ([string]::IsNullOrWhiteSpace($RegionId)) {
    @($registry.regions | ForEach-Object { $_.id })
}
else { @($RegionId.Split(',') | ForEach-Object { $_.Trim() } | Where-Object { $_ }) }
if ($regions.Count -eq 0) { throw 'No regions selected.' }

$compositeScript = Join-Path $PSScriptRoot 'New-LayeredTileComposite.ps1'
$indexScript = Join-Path $PSScriptRoot 'New-LayeredFloorIndex.ps1'
$baseFactor = if ($Factor -eq 'k100') { 1.0 } else { 0.35 }

$results = New-Object System.Collections.ArrayList
foreach ($region in $regions) {
    # The composite script appends <region>/k<factor> itself, so only the base root is passed.
    $compositeDir = Join-Path $SourceRoot "out/map-regions/composite/$region/$Factor"
    $compositeManifest = Join-Path (Split-Path -Parent $compositeDir) 'composite.manifest.json'
    $indexDir = Join-Path $SourceRoot "out/map-regions/packs/$region/layered-floors"
    $started = Get-Date
    Write-Host ''
    Write-Host "=== $region ===" -ForegroundColor Cyan
    try {
        # These are PowerShell scripts, so a failure throws instead of setting $LASTEXITCODE -
        # and under StrictMode merely reading $LASTEXITCODE after a script call is an error.
        if (-not ($SkipExisting -and (Test-Path -LiteralPath $compositeManifest))) {
            & $compositeScript -RegionId $region -BaseFactor $baseFactor
        }
        else { Write-Host '  composites already present; skipped' }

        $tileCount = @(Get-ChildItem -LiteralPath $compositeDir -File -Filter '*.png' -ErrorAction SilentlyContinue).Count
        if ($tileCount -eq 0) { throw 'no composite tiles were produced' }

        if (-not $CompositesOnly) {
            if (-not ($SkipExisting -and (Test-Path -LiteralPath (Join-Path $indexDir 'floor-index.json')))) {
                & $indexScript -RegionId $region -Factor $Factor
            }
            else { Write-Host '  floor index already present; skipped' }
        }
        $floors = 0
        $floorIndexPath = Join-Path $indexDir 'floor-index.json'
        if (Test-Path -LiteralPath $floorIndexPath) {
            $floors = @((Get-Content -LiteralPath $floorIndexPath -Raw -Encoding UTF8 | ConvertFrom-Json).floors).Count
        }
        [void]$results.Add([pscustomobject]@{ Region = $region; Status = 'ok'
            LayeredTiles = $tileCount; Floors = $floors
            Seconds = [Math]::Round(((Get-Date) - $started).TotalSeconds, 1) })
    }
    catch {
        $message = $_.Exception.Message
        $status = if ($message -match 'No layer|has no layered maps|nothing to composite') { 'no-layers' } else { 'failed' }
        Write-Warning "  $region -> $status : $message"
        [void]$results.Add([pscustomobject]@{ Region = $region; Status = $status
            LayeredTiles = $null; Floors = $null
            Seconds = [Math]::Round(((Get-Date) - $started).TotalSeconds, 1) })
    }
}

Write-Host ''
Write-Host '=== Layered rollout summary ==='
foreach ($result in $results) {
    Write-Host ("  {0,-14} {1,-10} layeredTiles={2,-5} floors={3,-4} {4}s" -f `
        $result.Region, $result.Status, ($result.LayeredTiles ?? '-'), ($result.Floors ?? '-'), $result.Seconds)
}
$ok = @($results | Where-Object Status -eq 'ok')
$floorTotal = 0
foreach ($entry in $ok) { if ($null -ne $entry.Floors) { $floorTotal += [int]$entry.Floors } }
Write-Host ("  buildable regions: {0}  layered floors: {1}" -f $ok.Count, $floorTotal)
