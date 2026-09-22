[CmdletBinding()]
param(
    [string]$PackRoot = (Join-Path (Split-Path -Parent $PSScriptRoot) 'Assets\FeaturesDatas\KuroTilePacks\tethys'),
    [switch]$AllowUnverified
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$manifestPath = Join-Path $PackRoot 'manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath)) { throw "Missing pack manifest: $manifestPath" }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$sceneStates = @{ World = 8; Tethys = 900; Fabricatorium = 905; Avinoleum = 903; Lahai = 906; LowerVault = 902; Darkplain = 909; TimeRiftRuins = 910 }
$sceneIds = @{ World = 1; Tethys = 2; Fabricatorium = 3; Avinoleum = 4; Lahai = 5; LowerVault = 6; Darkplain = 7; TimeRiftRuins = 8 }
if ($manifest.formatVersion -ne 1 -or -not $sceneStates.ContainsKey([string]$manifest.scene) -or [string]::IsNullOrWhiteSpace([string]$manifest.packId)) {
    throw 'Pack manifest format, scene, or identifier is invalid.'
}
$manifestSceneId = if ($null -ne $manifest.PSObject.Properties['sceneId']) { [int]$manifest.sceneId } else { 1 }
if ($manifestSceneId -ne [int]$sceneIds[[string]$manifest.scene] -or
    [int]$manifest.source.state -ne [int]$sceneStates[[string]$manifest.scene]) {
    throw 'Pack manifest scene ID or Kuro state is invalid.'
}
if ($null -ne $manifest.PSObject.Properties['coordinateTransform'] -and [double]$manifest.coordinateTransform.scale -le 0) {
    throw 'Pack manifest coordinate transform is missing or invalid.'
}
if (@($manifest.tiles).Count -lt 1) { throw 'Pack manifest contains no source tiles.' }
if ($null -eq $manifest.referenceVerification) {
    throw 'Reference-minimap verification is missing.'
}
$isReferencePassed = [bool]$manifest.referenceVerification.passed -and
    $null -ne $manifest.referenceVerification.errorPixels -and [double]$manifest.referenceVerification.errorPixels -le 8.0
if (-not $isReferencePassed) {
    if (-not $AllowUnverified -or -not [bool]$manifest.referenceVerification.skipped) {
        throw 'Reference-minimap verification did not pass the 8-pixel tolerance.'
    }
    Write-Warning "Feature pack is intentionally unverified: $($manifest.packId)"
}
# One coordinate may legitimately carry several source tiles: the surface tile and, for a
# layered-map ("分层") region, one composite per floor (scripts/New-LayeredTileComposite.ps1
# plus -LayeredCompositeDir on Sync-KuroMapFeaturePack.ps1). What must never happen is the
# same coordinate listed twice with the SAME bytes - that is a duplicated entry, not an
# extra appearance. Distinct-coordinate tiles keep the original guarantee.
$entries = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$appearances = @{}
foreach ($tile in @($manifest.tiles)) {
    $id = "$($tile.x),$($tile.y)"
    $sha = ([string]$tile.sha256).ToLowerInvariant()
    if ([string]::IsNullOrWhiteSpace($sha) -or $sha.Length -ne 64) { throw "Invalid tile entry: $id" }
    if (-not $entries.Add("$id|$sha")) { throw "Duplicate tile entry (same coordinate and bytes): $id" }
    if (-not $appearances.ContainsKey($id)) { $appearances[$id] = 0 }
    $appearances[$id]++
}
$layeredCount = if ($null -ne $manifest.PSObject.Properties['layeredTileCount']) { [int]$manifest.layeredTileCount } else { 0 }
$extraAppearances = (@($appearances.Values) | Measure-Object -Sum).Sum - $appearances.Count
if ($extraAppearances -ne $layeredCount) {
    throw "Manifest lists $extraAppearances extra tile appearances but layeredTileCount is $layeredCount."
}
$featurePath = Join-Path $PackRoot ([string]$manifest.features.file)
$binaryPath = Join-Path $PackRoot 'features.imf'
$binaryManifestPath = Join-Path $PackRoot 'features.imf.manifest.json'
if (-not (Test-Path -LiteralPath $binaryPath) -or -not (Test-Path -LiteralPath $binaryManifestPath)) {
    throw 'Missing binary feature resource. Run scripts\Build-VisualIndex.ps1.'
}
$binaryManifest = Get-Content -LiteralPath $binaryManifestPath -Raw | ConvertFrom-Json
if ([string]$binaryManifest.format -ne 'IMAOFT01' -or
    [int]$binaryManifest.descriptorColumns -ne 128 -or
    [int]$binaryManifest.keypointCount -lt 12) {
    throw 'Binary feature manifest header is invalid.'
}
# The runtime reads only the binary. The source XML/YAML is a build-time input that is
# deliberately not shipped (it is ~75% of a pack and CoreHost never opens it), so its
# provenance is checked through the hash the binary manifest records instead.
$recordedXmlHash = ([string]$manifest.features.sha256).ToLowerInvariant()
$binaryXmlHash = ([string]$binaryManifest.sourceXmlSha256).ToLowerInvariant()
if ([string]::IsNullOrWhiteSpace($recordedXmlHash) -or $binaryXmlHash -ne $recordedXmlHash) {
    throw 'Binary feature manifest was not built from the XML source recorded in the pack manifest.'
}
if ([int]$binaryManifest.keypointCount -ne [int]$manifest.features.keypointCount) {
    throw 'Binary feature manifest keypoint count does not match the pack manifest.'
}
$xmlCount = [int]$binaryManifest.keypointCount
$featureSourceVerified = 'binary-manifest'
if (Test-Path -LiteralPath $featurePath) {
    $actualHash = (Get-FileHash -LiteralPath $featurePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $recordedXmlHash) { throw 'Feature XML SHA-256 mismatch.' }
    $header = (Get-Content -LiteralPath $featurePath -TotalCount 16) -join [Environment]::NewLine
    if ($header -notmatch '<opencv_storage>' -or $header -notmatch '<num_keypoints>(\d+)</num_keypoints>') { throw 'Feature XML header is invalid.' }
    $xmlCount = [int]$Matches[1]
    if ($xmlCount -ne [int]$manifest.features.keypointCount -or $xmlCount -lt 12) { throw 'Feature XML keypoint count does not match manifest.' }
    if ([int]$binaryManifest.keypointCount -ne $xmlCount) { throw 'Binary feature manifest does not match the verified XML source.' }
    $featureSourceVerified = 'source-xml'
}
Write-Host "Kuro tile feature pack valid: pack=$($manifest.packId) coordinates=$($appearances.Count) tileEntries=$(@($manifest.tiles).Count) layered=$layeredCount keypoints=$xmlCount resource=$($manifest.resourceVersion) featureSource=$featureSourceVerified" -ForegroundColor Green
