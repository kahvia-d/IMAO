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
if ($manifest.formatVersion -ne 1) { throw 'Unsupported candidate manifest format.' }
if ([string]$manifest.scene -ne 'World') { throw 'Dreamzhou candidate must use the World scene.' }
if ([double]$manifest.anchorWorldCoordinate.x -ne -6725 -or [double]$manifest.anchorWorldCoordinate.y -ne -919) {
    throw 'Unexpected Dreamzhou candidate anchor.'
}

$imageName = [string]$manifest.reference.image
if ([string]::IsNullOrWhiteSpace($imageName) -or [IO.Path]::GetFileName($imageName) -ne $imageName) {
    throw 'Candidate reference image must be a single safe file name.'
}
$imagePath = Join-Path $packDirectory $imageName
if (-not (Test-Path -LiteralPath $imagePath)) { throw "Missing candidate reference image: $imagePath" }

$actualHash = (Get-FileHash -LiteralPath $imagePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualHash -ne ([string]$manifest.reference.sha256).ToLowerInvariant()) {
    throw 'Candidate reference image SHA-256 mismatch.'
}

Add-Type -AssemblyName System.Drawing
$image = [Drawing.Image]::FromFile($imagePath)
try {
    if ($image.Width -ne [int]$manifest.reference.width -or $image.Height -ne [int]$manifest.reference.height) {
        throw "Candidate reference dimensions mismatch: got $($image.Width)x$($image.Height)."
    }
}
finally {
    $image.Dispose()
}

$innerRadius = [int]$manifest.mask.innerRadius
$outerRadius = [int]$manifest.mask.outerRadius
if ($innerRadius -le 0 -or $outerRadius -le $innerRadius -or $outerRadius -gt ([int]$manifest.reference.width / 2)) {
    throw 'Candidate mask radii are invalid.'
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

Write-Host "Dreamzhou candidate assets passed: $($manifest.packId), anchor $($manifest.anchorWorldCoordinate.x),$($manifest.anchorWorldCoordinate.y)."
