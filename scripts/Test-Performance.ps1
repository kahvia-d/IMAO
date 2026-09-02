[CmdletBinding()]
param(
    [string]$DiagnosticsRoot,
    [int]$FeatureLoadRuns = 5,
    [string]$CoordinateRegressionReport,
    [string]$VisualRegressionReport,
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($DiagnosticsRoot)) { $DiagnosticsRoot = Join-Path $repoRoot 'x64\Debug\Diagnostics' }
if ([string]::IsNullOrWhiteSpace($CoordinateRegressionReport)) { $CoordinateRegressionReport = Join-Path $repoRoot 'x64\Performance\coordinate-regression.json' }
if ([string]::IsNullOrWhiteSpace($VisualRegressionReport)) { $VisualRegressionReport = Join-Path $repoRoot 'x64\Performance\visual-regression.json' }
if ([string]::IsNullOrWhiteSpace($OutputPath)) { $OutputPath = Join-Path $repoRoot 'x64\Performance\optimization-report.json' }

function Get-Percentile([double[]]$Values, [double]$Percentile) {
    if ($Values.Count -eq 0) { return $null }
    $sorted = $Values | Sort-Object
    $index = [Math]::Ceiling($Percentile * $sorted.Count) - 1
    return [double]$sorted[[Math]::Max(0, [Math]::Min($index, $sorted.Count - 1))]
}

$converter = Join-Path $repoRoot 'x64\Release\IMaoFeatureConverter.exe'
$featureBinary = Join-Path $repoRoot 'Assets\FeaturesDatas\Map_features.imf'
$featureRuns = [System.Collections.Generic.List[object]]::new()
if (Test-Path -LiteralPath $converter) {
    for ($index = 0; $index -lt $FeatureLoadRuns; $index++) {
        $json = & $converter '--benchmark' $featureBinary
        if ($LASTEXITCODE -ne 0) { throw 'Feature-load benchmark failed.' }
        $featureRuns.Add(($json | ConvertFrom-Json))
    }
}

$resourceWait = [System.Collections.Generic.List[double]]::new()
$inference = [System.Collections.Generic.List[double]]::new()
$recovery = [System.Collections.Generic.List[double]]::new()
$visualGlobal = [System.Collections.Generic.List[double]]::new()
$visualCoarse = [System.Collections.Generic.List[double]]::new()
$visualVerification = [System.Collections.Generic.List[double]]::new()
$visualTracking = [System.Collections.Generic.List[double]]::new()
$ambiguousRejected = 0
$ocrWeighted = 0
$falseAccepted = 0
$falseAcceptanceChecks = 0
if (Test-Path -LiteralPath $DiagnosticsRoot) {
    foreach ($log in Get-ChildItem -LiteralPath $DiagnosticsRoot -Recurse -Filter events.log -File) {
        foreach ($line in Get-Content -LiteralPath $log.FullName) {
            if ($line -match "\tresource-wait\tdurationMs=([0-9.]+)") { $resourceWait.Add([double]$Matches[1]) }
            if ($line -match "\tcoordinate-recognition\t.*inferenceMs=([0-9.]+)") { $inference.Add([double]$Matches[1]) }
            if ($line -match "\tcoordinate-recovery\t.*durationMs=([0-9.]+)") { $recovery.Add([double]$Matches[1]) }
            if ($line -match "\tvisual-localization-result\t.*coarseMs=([0-9.]+).*verifyMs=([0-9.]+).*totalMs=([0-9.]+)") {
                $visualCoarse.Add([double]$Matches[1])
                $visualVerification.Add([double]$Matches[2])
                $visualGlobal.Add([double]$Matches[3])
                if ($line -match 'ambiguous=1') { $ambiguousRejected++ }
            }
            if ($line -match "\tvisual-local-tracking-time\tdurationMs=([0-9.]+)") { $visualTracking.Add([double]$Matches[1]) }
            if ($line -match "\tocr-weak-hint\t.*mappedHints=([1-9][0-9]*)") { $ocrWeighted++ }
            if ($line -match "\tcoordinate-final\t.*expected=(true|false)") {
                $falseAcceptanceChecks++
                if ($line -match 'accepted=true.*expected=false') { $falseAccepted++ }
            }
        }
    }
}

$visualRegression = $null
if (Test-Path -LiteralPath $VisualRegressionReport) {
    $visualRegression = Get-Content -LiteralPath $VisualRegressionReport -Raw | ConvertFrom-Json
}

$coordinateRegression = $null
if (Test-Path -LiteralPath $CoordinateRegressionReport) {
    $coordinateRegression = Get-Content -LiteralPath $CoordinateRegressionReport -Raw | ConvertFrom-Json
    foreach ($sample in $coordinateRegression.samples) {
        if ($null -ne $sample.claheInferenceMilliseconds) { $inference.Add([double]$sample.claheInferenceMilliseconds) }
        if ($null -ne $sample.topHatInferenceMilliseconds) { $inference.Add([double]$sample.topHatInferenceMilliseconds) }
    }
}

$featureTimes = @($featureRuns | ForEach-Object { [double]$_.featureLoadMilliseconds })
$memoryValues = @($featureRuns | ForEach-Object { [double]$_.additionalWorkingSetBytes })
$report = [ordered]@{
    generatedAt = [DateTimeOffset]::Now.ToString('o')
    featureLoad = [ordered]@{
        runs = $featureRuns.Count
        p50Milliseconds = Get-Percentile $featureTimes 0.50
        p95Milliseconds = Get-Percentile $featureTimes 0.95
        maximumAdditionalWorkingSetBytes = if ($memoryValues.Count) { ($memoryValues | Measure-Object -Maximum).Maximum } else { $null }
        targetP95Milliseconds = 2000
        targetAdditionalWorkingSetBytes = 188743680
    }
    repeatedStartWait = [ordered]@{
        samples = $resourceWait.Count
        p95Milliseconds = Get-Percentile $resourceWait.ToArray() 0.95
        targetP95Milliseconds = 200
    }
    coordinateRecognition = [ordered]@{
        samples = $inference.Count
        p50Milliseconds = Get-Percentile $inference.ToArray() 0.50
        p95Milliseconds = Get-Percentile $inference.ToArray() 0.95
        targetP95Milliseconds = 100
        regressionMapPositionSuccessRate = if ($null -ne $coordinateRegression) { $coordinateRegression.labeledSuccessRate } else { $null }
        regressionExact3dSuccessRate = if ($null -ne $coordinateRegression) { $coordinateRegression.labeledExact3dSuccessRate } else { $null }
    }
    visualLocalization = [ordered]@{
        regressionSamples = if ($null -ne $visualRegression) { $visualRegression.processed } else { $null }
        labeledSamples = if ($null -ne $visualRegression) { $visualRegression.labeled } else { $null }
        strongPrecision = if ($null -ne $visualRegression) { $visualRegression.strongPrecision } else { $null }
        falseAccepted = if ($null -ne $visualRegression) { $visualRegression.falseAccepted } else { $null }
        indexAndFeaturesLoadMilliseconds = if ($null -ne $visualRegression) { $visualRegression.resourceLoadMilliseconds } else { $null }
        additionalWorkingSetBytes = if ($null -ne $visualRegression) { $visualRegression.additionalWorkingSetBytes } else { $null }
        coarseP95Milliseconds = if ($null -ne $visualRegression) { $visualRegression.coarseP95Milliseconds } else { Get-Percentile $visualCoarse.ToArray() 0.95 }
        verificationP95Milliseconds = if ($null -ne $visualRegression) { $visualRegression.verificationP95Milliseconds } else { Get-Percentile $visualVerification.ToArray() 0.95 }
        globalP95Milliseconds = if ($null -ne $visualRegression) { $visualRegression.globalP95Milliseconds } else { Get-Percentile $visualGlobal.ToArray() 0.95 }
        acceptedGlobalP95Milliseconds = if ($null -ne $visualRegression) { $visualRegression.acceptedGlobalP95Milliseconds } else { Get-Percentile $visualGlobal.ToArray() 0.95 }
        localTrackingP95Milliseconds = if ($null -ne $visualRegression) { $visualRegression.localTrackingP95Milliseconds } else { Get-Percentile $visualTracking.ToArray() 0.95 }
        ambiguousRejected = if ($null -ne $visualRegression) { $visualRegression.ambiguousRejected } else { $ambiguousRejected }
        ocrWeightedBatches = $ocrWeighted
        targetStrongPrecision = 0.95
        targetFalseAccepted = 0
        targetAcceptedGlobalP95Milliseconds = 250
        targetLocalTrackingP95Milliseconds = 50
        targetLoadP95Milliseconds = 2000
        targetAdditionalWorkingSetBytes = 188743680
    }
    recovery = [ordered]@{
        samples = $recovery.Count
        p95Milliseconds = Get-Percentile $recovery.ToArray() 0.95
        targetP95Milliseconds = 500
    }
    falseAcceptedCoordinates = [ordered]@{
        samples = $falseAcceptanceChecks
        count = if ($falseAcceptanceChecks -gt 0) { $falseAccepted } else { $null }
        target = 0
    }
}

$directory = Split-Path -Parent $OutputPath
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $OutputPath -Encoding utf8
$report | ConvertTo-Json -Depth 6
