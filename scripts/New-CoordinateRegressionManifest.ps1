[CmdletBinding()]
param(
    [string]$DiagnosticsRoot,
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($DiagnosticsRoot)) {
    $DiagnosticsRoot = Join-Path $repoRoot 'x64\Debug\Diagnostics'
}
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $repoRoot 'Tests\CoordinateRegression\manifest.json'
}

$knownLabels = @{
    '20260823-234837/4_ocr-coordinate-crop.png' = @{ expectedText = '-408,561,49'; sceneId = 1 }
    '20260823-234837/8_ocr-coordinate-crop.png' = @{ expectedText = '-408,561,49'; sceneId = 1 }
    '20260824-124646/34_ocr-coordinate-crop.png' = @{ expectedText = '-6901,-708,-22'; sceneId = 1 }
    '20260824-124646/38_ocr-coordinate-crop.png' = @{ expectedText = '-6905,-728,-22'; sceneId = 1 }
    '20260824-124646/44_ocr-coordinate-crop.png' = @{ expectedText = '-6904,-763,-22'; sceneId = 1 }
}

$samples = [System.Collections.Generic.List[object]]::new()
foreach ($session in Get-ChildItem -LiteralPath $DiagnosticsRoot -Directory | Sort-Object Name) {
    $eventsPath = Join-Path $session.FullName 'events.log'
    $crops = Get-ChildItem -LiteralPath $session.FullName -Filter '*_ocr-coordinate-crop.png' -File | Sort-Object {
        [int]($_.BaseName.Split('_')[0])
    }
    if ($crops.Count -eq 0) { continue }

    $ocrResults = @()
    if (Test-Path -LiteralPath $eventsPath) {
        $ocrResults = Select-String -Path $eventsPath -Pattern "\tocr-result\t" | ForEach-Object {
            $line = $_.Line
            $raw = $null
            $accepted = $line -match 'accepted=true'
            if ($line -match "result0='([^']*)'") { $raw = $Matches[1] }
            [pscustomobject]@{ Raw = $raw; Accepted = $accepted; Line = $line }
        }
    }

    for ($index = 0; $index -lt $crops.Count; $index++) {
        $relative = $session.Name + '/' + $crops[$index].Name
        $ocr = if ($index -lt $ocrResults.Count) { $ocrResults[$index] } else { $null }
        $known = $knownLabels[$relative]
        $numericObserved = $null -ne $ocr -and $ocr.Raw -match '[0-9]'
        $expected = if ($null -ne $known) { $known.expectedText } elseif ($null -ne $ocr -and $ocr.Accepted) { $ocr.Raw } else { $null }
        $confidence = if ($null -ne $known) { 'manually-verified' } elseif ($null -ne $expected) { 'historical-accepted' } elseif ($numericObserved) { 'ocr-observed-needs-review' } else { 'noise-or-hidden' }
        $sceneId = if ($null -ne $known) { $known.sceneId } else { $null }
        $samples.Add([ordered]@{
            id = $relative.Replace('/', '-')
            image = ('x64/Debug/Diagnostics/' + $relative)
            expectedText = $expected
            coordinateVisible = [bool]($null -ne $expected -or $numericObserved)
            sceneId = $sceneId
            labelConfidence = $confidence
        })
    }
}

$manifest = [ordered]@{
    formatVersion = 1
    generatedFrom = 'x64/Debug/Diagnostics'
    sampleCount = $samples.Count
    minimumLiveTarget = 100
    samples = $samples
}
$directory = Split-Path -Parent $OutputPath
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $OutputPath -Encoding utf8
Write-Host "Coordinate regression manifest: $OutputPath ($($samples.Count) samples)"
