# Sweep every Kuro tile pack with a SIFT parameter set, measuring the reference
# the way a rebuilt pack would actually be built: detect inside each 1024px tile
# (tools/KuroMapFeatureBuilder does exactly that), not once over the stitched
# raster. The whole-raster protocol understates the keypoint count, and the
# keypoint count is what decides the pack size.
#
# One report per pack plus a merged summary, so a size claim can be re-derived
# without re-running anything. The sweep is resumable: a pack whose report is
# already on disk is skipped with -SkipExisting, and a pack the harness cannot
# read is recorded as unmeasured instead of aborting the run.
[CmdletBinding()]
param(
    [string]$Config = 'sift-friend',
    [int]$Queries = 12,
    [ValidateSet('art', 'texture', 'answerable')]
    [string]$Gate = 'art',
    [string]$OutputRoot = 'measurements/sweep-per-tile',
    [string]$SummaryPath = 'measurements/sweep-sift-per-tile.json',
    [string[]]$Pack = @(),
    [switch]$SkipExisting
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
# cv2 writes decoder warnings to stderr; they are not failures and must not be
# turned into terminating errors.
$PSNativeCommandUseErrorActionPreference = $false

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$interpreter = Join-Path $repoRoot '.venv-featureprobe\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $interpreter)) { throw "Python interpreter is missing: $interpreter" }
# The harness is a sibling, so the pair works wherever it is checked out or copied to.
$harness = Join-Path $PSScriptRoot 'imao_vs_sift.py'
$outputDirectory = Join-Path $repoRoot $OutputRoot
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null

$registry = Get-Content -LiteralPath (Join-Path $repoRoot 'Assets\FeaturesDatas\kuro-tile-packs.json') -Raw | ConvertFrom-Json
$packs = if ($Pack.Count -gt 0) { $Pack } else { @($registry.packs) }
# PowerShell variable names are case-insensitive, so a local $config would alias
# the $Config parameter and break every iteration after the first.
$configKey = $Config

foreach ($name in $packs) {
    $reportPath = Join-Path $outputDirectory "$name.json"
    $failedPath = Join-Path $outputDirectory "$name.failed.json"
    if ($SkipExisting -and (Test-Path -LiteralPath $reportPath)) {
        Write-Host "== $name (cached)" -ForegroundColor DarkGray
        continue
    }
    if (Test-Path -LiteralPath $failedPath) { Remove-Item -LiteralPath $failedPath -Force }
    Write-Host "== $name" -ForegroundColor Cyan
    $errorLog = Join-Path $outputDirectory "$name.stderr.log"
    & $interpreter $harness --pack $name --queries $Queries --config $configKey --gate $Gate --json $reportPath 2> $errorLog
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $reportPath)) {
        $tail = if (Test-Path -LiteralPath $errorLog) {
            (@(Get-Content -LiteralPath $errorLog -Tail 12) -join [Environment]::NewLine)
        } else { '(no stderr captured)' }
        [IO.File]::WriteAllText($failedPath, (@{
                    pack       = $name
                    exitCode   = $LASTEXITCODE
                    config     = $configKey
                    reason     = $tail
                    recordedAt = [DateTime]::UtcNow.ToString('o')
                } | ConvertTo-Json -Depth 4) + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
        Write-Warning "$name could not be measured; recorded in $failedPath"
        continue
    }
}

# Phase 2 reads whatever reports exist, so a resumed run produces the same
# summary as a single full run.
$rows = @()
$unmeasured = @()
foreach ($file in Get-ChildItem -LiteralPath $outputDirectory -Filter '*.json' | Sort-Object Name) {
    if ($file.Name.EndsWith('.failed.json')) {
        $failed = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
        $unmeasured += [pscustomobject][ordered]@{
            pack     = [string]$failed.pack
            exitCode = [int]$failed.exitCode
            reason   = (([string]$failed.reason) -split "`n" | Select-Object -First 3) -join ' / '
        }
        continue
    }
    $name = [IO.Path]::GetFileNameWithoutExtension($file.Name)
    $report = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
    $entryProperty = $report.configs.PSObject.Properties[$configKey]
    if ($null -eq $entryProperty) {
        $unmeasured += [pscustomobject][ordered]@{
            pack = $name; exitCode = 0
            reason = "report has no config '$configKey'"
        }
        continue
    }
    $entry = $entryProperty.Value
    # @() must wrap the whole pipeline, not just its input: assigning a pipeline
    # that matches nothing yields $null, and under Set-StrictMode -Version Latest
    # $null.Count is a terminating error.
    $missing = @(@('keypoints', 'imfBytes', 'quantizedImfBytes', 'quantizedZlibBytes', 'detectSeconds') |
        Where-Object { $null -eq $entry.PSObject.Properties[$_] })
    if ($missing.Count -gt 0) {
        $unmeasured += [pscustomobject][ordered]@{
            pack = $name; exitCode = 0
            reason = "report is missing: $($missing -join ', ')"
        }
        continue
    }
    $shipped = [int]$report.imao.keypoints
    $detected = [int]$entry.keypoints
    # How many drawn queries the reference set cannot answer at all. A query
    # whose crop holds fewer reference keypoints than the pass criterion needs
    # (8) is unanswerable: no detector can score it, so those queries measure
    # how much of the map carries content, not how good the detector is.
    $referenceCounts = @()
    if ($null -ne $report.PSObject.Properties['queries']) {
        $referenceCounts = @($report.queries | ForEach-Object { [int]$_.referenceKeypoints })
    }
    $unanswerable = @($referenceCounts | Where-Object { $_ -lt 8 }).Count
    $referenceMin = if ($referenceCounts.Count -gt 0) {
        ($referenceCounts | Measure-Object -Minimum).Minimum
    } else { 0 }
    $referenceMedian = if ($referenceCounts.Count -gt 0) {
        $sorted = @($referenceCounts | Sort-Object)
        $sorted[[int][Math]::Floor($sorted.Count / 2)]
    } else { 0 }
    $rows += [pscustomobject][ordered]@{
        pack               = $name
        referenceMode      = [string]$report.referenceMode
        baseMapId          = [string]$report.baseMapId
        raster             = @([int]$report.layout.size[0], [int]$report.layout.size[1])
        tiles              = [int]$report.layout.tiles
        shippedKeypoints   = $shipped
        detectorKeypoints  = $detected
        ratio              = [Math]::Round($detected / [Math]::Max($shipped, 1), 4)
        shippedBytes       = [int]$report.imao.diskBytes
        rawImfBytes        = [int]$entry.imfBytes
        quantizedImfBytes  = [int]$entry.quantizedImfBytes
        quantizedZlibBytes = [int]$entry.quantizedZlibBytes
        detectSeconds      = [double]$entry.detectSeconds
        verified           = [int]$entry.verified
        queryCount         = [int]$entry.queries
        inliers            = @($entry.inliers)
        unanswerableQueries    = $unanswerable
        referenceKeypointsMin  = [int]$referenceMin
        referenceKeypointsMedian = [int]$referenceMedian
    }
}

$summary = [ordered]@{
    generatedBy   = 'measurements/accuracy-check/sweep_per_tile.ps1'
    config        = $configKey
    gate          = $Gate
    referenceMode = 'per-tile'
    packs         = $rows
    unmeasured    = $unmeasured
    totals        = [ordered]@{
        packs              = $rows.Count
        unmeasuredPacks    = $unmeasured.Count
        shippedKeypoints   = ($rows | Measure-Object shippedKeypoints -Sum).Sum
        detectorKeypoints  = ($rows | Measure-Object detectorKeypoints -Sum).Sum
        shippedBytes       = ($rows | Measure-Object shippedBytes -Sum).Sum
        rawImfBytes        = ($rows | Measure-Object rawImfBytes -Sum).Sum
        quantizedImfBytes  = ($rows | Measure-Object quantizedImfBytes -Sum).Sum
        quantizedZlibBytes = ($rows | Measure-Object quantizedZlibBytes -Sum).Sum
        detectSeconds      = [Math]::Round(($rows | Measure-Object detectSeconds -Sum).Sum, 1)
        verified           = ($rows | Measure-Object verified -Sum).Sum
        queryCount         = ($rows | Measure-Object queryCount -Sum).Sum
        unanswerableQueries = ($rows | Measure-Object unanswerableQueries -Sum).Sum
    }
}
$summaryPath = Join-Path $repoRoot $SummaryPath
[IO.File]::WriteAllText($summaryPath,
    ($summary | ConvertTo-Json -Depth 8) + [Environment]::NewLine,
    [Text.UTF8Encoding]::new($false))

Write-Host ''
$rows | Sort-Object shippedKeypoints | Format-Table pack, tiles, shippedKeypoints, detectorKeypoints,
    ratio, rawImfBytes, quantizedZlibBytes, verified, detectSeconds -AutoSize
Write-Host "queries the reference set cannot answer (crop holds fewer than 8 reference keypoints):"
$rows | Sort-Object unanswerableQueries -Descending | Format-Table pack, verified, queryCount,
    unanswerableQueries, referenceKeypointsMin, referenceKeypointsMedian -AutoSize
if ($unmeasured.Count -gt 0) {
    Write-Warning "unmeasured packs:"
    $unmeasured | Format-Table -AutoSize | Out-String | Write-Warning
}
$t = $summary.totals
if ($t.shippedKeypoints -gt 0) {
    Write-Host ("TOTAL measured {0} packs: shipped kp {1}  detector kp {2}  ratio {3:P1}" -f `
            $t.packs, $t.shippedKeypoints, $t.detectorKeypoints, ($t.detectorKeypoints / $t.shippedKeypoints))
    Write-Host ("TOTAL raw {0:N1} MB  uint8 {1:N1} MB  uint8+zlib {2:N1} MB  detect {3}s" -f `
            ($t.rawImfBytes / 1MB), ($t.quantizedImfBytes / 1MB), ($t.quantizedZlibBytes / 1MB), $t.detectSeconds)
}
Write-Host "wrote $summaryPath" -ForegroundColor Green
