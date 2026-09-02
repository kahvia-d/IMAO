[CmdletBinding()]
param(
    [string]$FeatureDataRoot = (Join-Path (Split-Path -Parent $PSScriptRoot) 'Assets\FeaturesDatas'),
    [string]$DiagnosticsSession
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$packDirectory = Join-Path $FeatureDataRoot 'DreamzhouCandidate'
$manifestPath = Join-Path $packDirectory 'manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath)) { throw "Missing candidate manifest: $manifestPath" }

$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.formatVersion -notin 1, 2) { throw 'Unsupported candidate manifest format.' }
if ([string]$manifest.scene -ne 'World') { throw 'Dreamzhou candidate must use the World scene.' }
$references = if ($manifest.formatVersion -eq 1) {
    @([pscustomobject]@{
        anchorWorldCoordinate = $manifest.anchorWorldCoordinate
        reference = $manifest.reference
        mask = $manifest.mask
    })
} else {
    @($manifest.references)
}
if ($references.Count -lt 1) { throw 'Candidate reference list is empty.' }

Add-Type -AssemblyName System.Drawing
$anchorIds = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($entry in $references) {
    $anchorX = [double]$entry.anchorWorldCoordinate.x
    $anchorY = [double]$entry.anchorWorldCoordinate.y
    $anchorXIsFinite = -not [double]::IsNaN($anchorX) -and -not [double]::IsInfinity($anchorX)
    $anchorYIsFinite = -not [double]::IsNaN($anchorY) -and -not [double]::IsInfinity($anchorY)
    if (-not $anchorXIsFinite -or -not $anchorYIsFinite -or
        -not $anchorIds.Add("$anchorX,$anchorY")) {
        throw 'Candidate reference anchor is invalid or duplicated.'
    }
    $imageName = [string]$entry.reference.image
    if ([string]::IsNullOrWhiteSpace($imageName) -or [IO.Path]::GetFileName($imageName) -ne $imageName) {
        throw 'Candidate reference image must be a single safe file name.'
    }
    $imagePath = Join-Path $packDirectory $imageName
    if (-not (Test-Path -LiteralPath $imagePath)) { throw "Missing candidate reference image: $imagePath" }

    $actualHash = (Get-FileHash -LiteralPath $imagePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne ([string]$entry.reference.sha256).ToLowerInvariant()) {
        throw "Candidate reference image SHA-256 mismatch: $imageName"
    }

    $image = [Drawing.Image]::FromFile($imagePath)
    try {
        if ($image.Width -ne [int]$entry.reference.width -or $image.Height -ne [int]$entry.reference.height) {
            throw "Candidate reference dimensions mismatch for ${imageName}: got $($image.Width)x$($image.Height)."
        }
    }
    finally {
        $image.Dispose()
    }

    $entryMaskProperty = $entry.PSObject.Properties['mask']
    $mask = if ($null -ne $entryMaskProperty) { $entryMaskProperty.Value } else { $manifest.mask }
    $innerRadius = [int]$mask.innerRadius
    $outerRadius = [int]$mask.outerRadius
    if ($innerRadius -le 0 -or $outerRadius -le $innerRadius -or
        $outerRadius -gt ([int]$entry.reference.width / 2)) {
        throw "Candidate mask radii are invalid: $imageName"
    }
}
if (-not $anchorIds.Contains('-6725,-919') -or -not $anchorIds.Contains('-8519,-292')) {
    throw 'Dreamzhou curated reference anchors are incomplete.'
}

if (-not [string]::IsNullOrWhiteSpace($DiagnosticsSession)) {
    $eventsPath = Join-Path $DiagnosticsSession 'events.log'
    if (-not (Test-Path -LiteralPath $eventsPath)) { throw "Missing diagnostics event log: $eventsPath" }
    $result = Get-Content -LiteralPath $eventsPath | Select-String -Pattern '^.*\tcandidate-feature-pack\t' | Select-Object -Last 1
    if ($null -eq $result) { throw 'No runtime candidate-pack verification was found in the diagnostics session.' }
    if ($result.Line -notmatch 'loaded=1' -or $result.Line -notmatch 'selfAccepted=1') {
        throw "Runtime candidate self-match was not accepted: $($result.Line)"
    }
}

Write-Host "Dreamzhou candidate assets passed: $($manifest.packId), references=$($references.Count)."
