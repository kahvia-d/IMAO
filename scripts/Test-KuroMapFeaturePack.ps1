[CmdletBinding()]
param(
    [string]$PackRoot = (Join-Path (Split-Path -Parent $PSScriptRoot) 'Assets\FeaturesDatas\KuroTilePacks\Dreamzhou')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$manifestPath = Join-Path $PackRoot 'manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath)) { throw "Missing pack manifest: $manifestPath" }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.formatVersion -ne 1 -or $manifest.scene -ne 'World' -or [string]::IsNullOrWhiteSpace([string]$manifest.packId)) {
    throw 'Pack manifest format, scene, or identifier is invalid.'
}
if (@($manifest.tiles).Count -lt 1) { throw 'Pack manifest contains no source tiles.' }
if ($null -eq $manifest.referenceVerification -or -not [bool]$manifest.referenceVerification.passed -or [double]$manifest.referenceVerification.errorPixels -gt 8.0) {
    throw 'Reference-minimap verification is missing or did not pass the 8-pixel tolerance.'
}
$ids = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($tile in @($manifest.tiles)) {
    $id = "$($tile.x),$($tile.y)"
    if (-not $ids.Add($id) -or [string]::IsNullOrWhiteSpace([string]$tile.sha256)) { throw "Invalid tile entry: $id" }
}
$featurePath = Join-Path $PackRoot ([string]$manifest.features.file)
if (-not (Test-Path -LiteralPath $featurePath)) { throw "Missing feature XML: $featurePath" }
$actualHash = (Get-FileHash -LiteralPath $featurePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualHash -ne [string]$manifest.features.sha256) { throw 'Feature XML SHA-256 mismatch.' }
$header = (Get-Content -LiteralPath $featurePath -TotalCount 16) -join [Environment]::NewLine
if ($header -notmatch '<opencv_storage>' -or $header -notmatch '<num_keypoints>(\d+)</num_keypoints>') { throw 'Feature XML header is invalid.' }
$xmlCount = [int]$Matches[1]
if ($xmlCount -ne [int]$manifest.features.keypointCount -or $xmlCount -lt 12) { throw 'Feature XML keypoint count does not match manifest.' }
Write-Host "Kuro tile feature pack valid: pack=$($manifest.packId) tiles=$($ids.Count) keypoints=$xmlCount resource=$($manifest.resourceVersion)" -ForegroundColor Green
