[CmdletBinding(DefaultParameterSetName = 'Check')]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('LowerVault', 'Darkplain', 'TimeRiftRuins')]
    [string]$Scene,
    [Parameter(Mandatory = $true)]
    [string]$GameEvidencePath,
    [Parameter(ParameterSetName = 'Check')]
    [switch]$Check,
    [Parameter(Mandatory = $true, ParameterSetName = 'Apply')]
    [switch]$Apply,
    # Local runtime trial based on original captures. This is explicitly not
    # the continuous-tracking/overlay acceptance required for a full release.
    [switch]$LocalCaptureTrial
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$stateIds = @{ LowerVault = 902; Darkplain = 909; TimeRiftRuins = 910 }
$sceneIds = @{ LowerVault = 6; Darkplain = 7; TimeRiftRuins = 8 }
$calibrationPath = Join-Path $repoRoot 'Assets\KuroMap\scene-calibrations.json'
$validationPath = Join-Path $repoRoot 'Assets\KuroMap\scene-validation.json'
$packRoot = Join-Path $repoRoot "Assets\FeaturesDatas\KuroTilePacks\$Scene"

function Write-Utf8Json([object]$Value, [string]$Path) {
    [IO.File]::WriteAllText($Path, (($Value | ConvertTo-Json -Depth 32) + [Environment]::NewLine), [Text.UTF8Encoding]::new($false))
}

if (-not (Test-Path -LiteralPath $calibrationPath)) { throw 'Scene calibration registry is missing.' }
$calibrations = Get-Content -LiteralPath $calibrationPath -Raw | ConvertFrom-Json -AsHashtable
$calibration = $calibrations['scenes'][$Scene]
if ($null -eq $calibration -or -not [bool]$calibration['passed'] -or [int]$calibration['sampleCount'] -ne 4 -or [double]$calibration['maxErrorPixels'] -gt 8.0) {
    throw "Scene $Scene has not passed its four-point calibration."
}

# This also checks the XML hash, binary feature pack and reference-minimap
# tolerance.  It never changes the pack.
& (Join-Path $PSScriptRoot 'Test-KuroMapFeaturePack.ps1') -PackRoot $packRoot

if (-not (Test-Path -LiteralPath $GameEvidencePath)) { throw "Game validation evidence is missing: $GameEvidencePath" }
$evidence = Get-Content -LiteralPath $GameEvidencePath -Raw | ConvertFrom-Json
if ($evidence.formatVersion -ne 1 -or [string]$evidence.scene -ne $Scene -or [int]$evidence.state -ne $stateIds[$Scene] -or
    [int]$evidence.calibrationPairCount -ne 4) {
    throw 'Game validation evidence has an invalid format, scene, state, or sample count.'
}
if ($LocalCaptureTrial) {
    if ($evidence.evidenceKind -ne 'original-capture-replay' -or @($evidence.pairs).Count -ne 4) {
        throw 'Local trial requires four original capture pairs.'
    }
    foreach ($pair in $evidence.pairs) {
        foreach ($capture in @($pair.minimap, $pair.viewport)) {
            if (!(Test-Path -LiteralPath $capture.path) -or
                (Get-FileHash -LiteralPath $capture.path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $capture.sha256) {
                throw 'Original capture hash mismatch.'
            }
        }
    }
    foreach ($kind in @('map', 'minimap')) {
        $path = [string]$evidence.($kind + 'Report')
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne
            [string]$evidence.($kind + 'ReportSha256')) {
            throw 'Capture replay report hash mismatch.'
        }
    }
    $maps = Get-Content -LiteralPath $evidence.mapReport -Raw | ConvertFrom-Json
    $mini = Get-Content -LiteralPath $evidence.minimapReport -Raw | ConvertFrom-Json
    if (@($maps.samples).Count -ne 4 -or $maps.failed -ne 0 -or
        @($maps.samples | Where-Object { !$_.accepted -or !$_.correct -or $_.sceneId -ne $sceneIds[$Scene] }).Count -ne 0 -or
        $mini.processed -ne 4 -or $mini.falseAccepted -ne 0 -or
        $mini.rawStrongCorrect -lt 1 -or $mini.rawStrongCorrect -ne $mini.rawStrong) {
        throw 'Local capture replay does not support enabling this scene.'
    }
} else {
foreach ($check in @('minimapLocalization', 'continuousTracking', 'gameMapMarkers', 'minimapMarkers')) {
    if ($null -eq $evidence.checks -or -not [bool]$evidence.checks.$check) { throw "Game validation check failed or missing: $check" }
}
if ($null -eq $evidence.visualRegression -or [int]$evidence.visualRegression.sampleCount -ne 113 -or
    [int]$evidence.visualRegression.currentFinalAccepted -lt [int]$evidence.visualRegression.baselineFinalAccepted -or
    [int]$evidence.visualRegression.currentFalseAccepted -gt [int]$evidence.visualRegression.baselineFalseAccepted -or
    [int]$evidence.visualRegression.wrongSceneAccepted -ne 0 -or [int]$evidence.visualRegression.distantWrongAccepted -ne 0) {
    throw 'Visual-regression evidence does not meet the required baseline or false-acceptance criteria.'
}
}

$evidenceHash = (Get-FileHash -LiteralPath $GameEvidencePath -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "Release approval $($PSCmdlet.ParameterSetName): scene=$Scene calibration=$([Math]::Round([double]$calibration['maxErrorPixels'], 3))px evidence=$evidenceHash"
if ($Apply) {
    if (-not (Test-Path -LiteralPath $validationPath)) { throw 'Scene validation registry is missing.' }
    $validation = Get-Content -LiteralPath $validationPath -Raw | ConvertFrom-Json -AsHashtable
    if ($validation['formatVersion'] -ne 1 -or $null -eq $validation['scenes']) { throw 'Scene validation registry format is invalid.' }
    $validation['scenes'][$Scene] = [ordered]@{
        approved = $true; state = $stateIds[$Scene]; approvedAtUtc = [DateTime]::UtcNow.ToString('o')
        calibrationMaxErrorPixels = [double]$calibration['maxErrorPixels']; gameEvidenceSha256 = $evidenceHash
        validationScope = if ($LocalCaptureTrial) { 'local-capture-trial' } else { 'full-game-validation' }
    }
    Write-Utf8Json $validation $validationPath
    if ($LocalCaptureTrial) {
        Write-Host "Enabled $Scene for a local capture-based trial; continuous tracking and live overlays remain unverified." -ForegroundColor Yellow
    } else {
        Write-Host "Released $Scene for runtime item filtering and visual localization." -ForegroundColor Green
    }
}
