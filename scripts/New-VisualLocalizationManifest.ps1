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
    $OutputPath = Join-Path $repoRoot 'Tests\VisualLocalization\manifest.json'
}

$origins = @{
    1 = @(2474.0, 1957.0)
    2 = @(8593.0, 1382.0)
    3 = @(7437.0, 13783.0)
    4 = @(2433.0, 9030.0)
    5 = @(21662.0, 13138.0)
    6 = @(0.0, 0.0)
    7 = @(0.0, 0.0)
    8 = @(0.0, 0.0)
}

function New-Expected([int]$SceneId, [double]$MapX, [double]$MapY, [double]$Tolerance, [string]$Source) {
    return [ordered]@{
        sceneId = $SceneId
        mapX = $MapX
        mapY = $MapY
        tolerance = $Tolerance
        source = $Source
    }
}

$samples = [System.Collections.Generic.List[object]]::new()
$sessions = Get-ChildItem -LiteralPath $DiagnosticsRoot -Directory | Sort-Object Name
foreach ($session in $sessions) {
    $logPath = Join-Path $session.FullName 'events.log'
    if (-not (Test-Path -LiteralPath $logPath)) { continue }
    $lines = @(Get-Content -LiteralPath $logPath)
    $lastExpected = $null
    $minimapEvidenceReady = $false
    for ($lineIndex = 0; $lineIndex -lt $lines.Count; $lineIndex++) {
        $line = $lines[$lineIndex]
        if ($line -match 'minimap-bootstrap-ready\tscene=(\d+) playerMap=([-0-9.]+),([-0-9.]+)') {
            $lastExpected = New-Expected ([int]$Matches[1]) ([double]$Matches[2]) ([double]$Matches[3]) 48.0 'legacy-visual-verified'
        }
        elseif ($line -match 'game-state\tminimap=(\d+) minimapMatches=(\d+) map=(\d+)') {
            $minimapEvidenceReady = [int]$Matches[1] -eq 1 -and [int]$Matches[2] -ge 4 -and [int]$Matches[3] -eq 0
            if (-not $minimapEvidenceReady) {
                # A map-open/transition interval may include a teleport. Do not
                # attach the pre-transition tracking sample to a later crop.
                $lastExpected = $null
            }
        }
        elseif ($minimapEvidenceReady -and
            $line -match 'minimap-marker-sample\tscene=(\d+).*playerROC=([-0-9.]+),([-0-9.]+)') {
            $sceneId = [int]$Matches[1]
            if ($origins.ContainsKey($sceneId)) {
                $origin = $origins[$sceneId]
                $mapX = [double]$Matches[2] + $origin[0]
                $mapY = $origin[1] - [double]$Matches[3]
                $lastExpected = New-Expected $sceneId $mapX $mapY 48.0 'continuity-sample'
            }
        }

        if ($line -notmatch "`timage`t(?<file>[^ ]+_minimap-(?<kind>bootstrap|continuity)-crop\.png)") { continue }
        $fileName = $Matches['file']
        $kind = $Matches['kind']
        $imagePath = Join-Path $session.FullName $fileName
        $expected = if ($kind -eq 'continuity') { $lastExpected } else { $null }
        if ($kind -eq 'bootstrap') {
            for ($lookAhead = $lineIndex + 1; $lookAhead -lt $lines.Count; $lookAhead++) {
                $candidateLine = $lines[$lookAhead]
                if ($candidateLine -match "`timage`t[^ ]+_minimap-bootstrap-crop\.png") { break }
                if ($candidateLine -match 'minimap-bootstrap-failed') { break }
                if ($candidateLine -match 'minimap-bootstrap-ready\tscene=(\d+) playerMap=([-0-9.]+),([-0-9.]+)') {
                    $expected = New-Expected ([int]$Matches[1]) ([double]$Matches[2]) ([double]$Matches[3]) 32.0 'legacy-visual-verified'
                    break
                }
            }
        }
        $relativePath = [IO.Path]::GetRelativePath($repoRoot, $imagePath).Replace('\', '/')
        $samples.Add([ordered]@{
            id = "$($session.Name)-$([IO.Path]::GetFileNameWithoutExtension($fileName))"
            session = $session.Name
            kind = $kind
            image = $relativePath
            expected = $expected
        })
    }
}

# Curated references are independently anchored live minimap samples. Keeping
# every registered pack in the normal regression manifest catches a stale or
# omitted optional shard before it reaches a Debug/Release package.
$candidateRegistryPath = Join-Path $repoRoot 'Assets\FeaturesDatas\candidate-packs.json'
if (Test-Path -LiteralPath $candidateRegistryPath) {
    $candidateRegistry = Get-Content -LiteralPath $candidateRegistryPath -Raw | ConvertFrom-Json
    if ([int]$candidateRegistry.formatVersion -ne 1) { throw 'Unsupported candidate-pack registry format.' }
    foreach ($candidateDirectory in @($candidateRegistry.packs)) {
        $candidateManifestPath = Join-Path $repoRoot (Join-Path 'Assets\FeaturesDatas' ([string]$candidateDirectory))
        $candidateManifestPath = Join-Path $candidateManifestPath 'manifest.json'
        if (-not (Test-Path -LiteralPath $candidateManifestPath)) { throw "Missing candidate manifest: $candidateManifestPath" }
        $candidateManifest = Get-Content -LiteralPath $candidateManifestPath -Raw | ConvertFrom-Json
        $candidateSceneId = if ($null -ne $candidateManifest.PSObject.Properties['sceneId']) {
            [int]$candidateManifest.sceneId
        } else {
            switch ([string]$candidateManifest.scene) {
                'World' { 1 }
                'Tethys' { 2 }
                'Fabricatorium' { 3 }
                'Avinoleum' { 4 }
                'Lahai' { 5 }
                'LowerVault' { 6 }
                'Darkplain' { 7 }
                'TimeRiftRuins' { 8 }
                default { throw "Unsupported candidate scene: $candidateDirectory" }
            }
        }
        if (-not $origins.ContainsKey($candidateSceneId)) { throw "Unsupported candidate scene: $candidateDirectory" }
        $candidateReferences = if ([int]$candidateManifest.formatVersion -eq 1) {
            @([pscustomobject]@{
                anchorWorldCoordinate = $candidateManifest.anchorWorldCoordinate
                reference = $candidateManifest.reference
            })
        } else {
            @($candidateManifest.references)
        }
        foreach ($reference in $candidateReferences) {
            $worldX = [double]$reference.anchorWorldCoordinate.x
            $worldY = [double]$reference.anchorWorldCoordinate.y
            $anchorId = '{0}-{1}' -f
                $worldX.ToString('0.###', [Globalization.CultureInfo]::InvariantCulture),
                $worldY.ToString('0.###', [Globalization.CultureInfo]::InvariantCulture)
            $imagePath = Join-Path (Split-Path -Parent $candidateManifestPath) ([string]$reference.reference.image)
            if (-not (Test-Path -LiteralPath $imagePath)) { throw "Missing curated visual reference: $imagePath" }
            $samples.Add([ordered]@{
                id = "candidate-$($candidateManifest.packId)-$anchorId"
                session = [string]$candidateManifest.packId
                kind = 'curated-positive'
                image = [IO.Path]::GetRelativePath($repoRoot, $imagePath).Replace('\', '/')
                expected = New-Expected $candidateSceneId ($worldX * 1.205 + $origins[$candidateSceneId][0]) `
                    ($worldY * 1.205 + $origins[$candidateSceneId][1]) 12.0 'curated-visual-verified'
            })
        }
    }
}

$syntheticSources = @($samples | Where-Object {
    $_.kind -eq 'bootstrap' -and $null -ne $_.expected -and $_.expected.mapX -lt -5500.0
} | Select-Object -First 5)
$syntheticTransforms = @(
    [ordered]@{ name = 'rotate37'; values = [ordered]@{ rotationDegrees = 37.0 } },
    [ordered]@{ name = 'brightness'; values = [ordered]@{ brightnessAlpha = 0.72; brightnessBeta = 18.0 } },
    [ordered]@{ name = 'blur'; values = [ordered]@{ blurKernel = 5 } },
    [ordered]@{ name = 'scale092'; values = [ordered]@{ scale = 0.92 } },
    [ordered]@{ name = 'center-occlusion'; values = [ordered]@{ centerOcclusionRadius = 24 } }
)
foreach ($source in $syntheticSources) {
    foreach ($transform in $syntheticTransforms) {
        $samples.Add([ordered]@{
            id = "$($source.id)-synthetic-$($transform.name)"
            session = "$($source.session)-synthetic-$($transform.name)"
            kind = 'synthetic-positive'
            image = $source.image
            expected = $source.expected
            transform = $transform.values
        })
    }
}
if ($syntheticSources.Count -gt 0) {
    foreach ($negative in @(
        [ordered]@{ name = 'blank'; values = [ordered]@{ blank = $true } },
        [ordered]@{ name = 'noise'; values = [ordered]@{ noise = $true } }
    )) {
        $samples.Add([ordered]@{
            id = "synthetic-negative-$($negative.name)"
            session = "synthetic-negative-$($negative.name)"
            kind = 'synthetic-negative'
            image = $syntheticSources[0].image
            expected = $null
            mustReject = $true
            transform = $negative.values
        })
    }
}

$manifest = [ordered]@{
    schemaVersion = 1
    generatedFrom = [IO.Path]::GetRelativePath($repoRoot, $DiagnosticsRoot).Replace('\', '/')
    sampleCount = $samples.Count
    bootstrapCount = @($samples | Where-Object kind -eq 'bootstrap').Count
    continuityCount = @($samples | Where-Object kind -eq 'continuity').Count
    syntheticCount = @($samples | Where-Object kind -like 'synthetic-*').Count
    labeledCount = @($samples | Where-Object { $null -ne $_.expected }).Count
    samples = $samples
}
$outputDirectory = Split-Path -Parent $OutputPath
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$manifest | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $OutputPath -Encoding utf8
[pscustomobject]$manifest | Select-Object sampleCount, bootstrapCount, continuityCount, labeledCount | Format-List
