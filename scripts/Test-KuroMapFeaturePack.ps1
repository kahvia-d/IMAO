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
$sceneStates = @{ World = 8; Tethys = 900; Fabricatorium = 905; Avinoleum = 903; Lahai = 906; LowerVault = 902; Darkplain = 909; TimeRiftRuins = 910; MengshuTianluo = 912 }
$sceneIds = @{ World = 1; Tethys = 2; Fabricatorium = 3; Avinoleum = 4; Lahai = 5; LowerVault = 6; Darkplain = 7; TimeRiftRuins = 8; MengshuTianluo = 9 }
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
# A pack must ship the quantized encoding. The float32 encoding costs 540 bytes per keypoint against
# about 110 for the quantized one, so a pack that regresses to it is several times larger for the same
# recognized locations - the converter writes the quantized format by default, which is exactly why a
# silent regression is possible: an old binary or a `--format v1` run would look like a normal pack.
# Version 1 is still readable at runtime, so nothing else would notice.
$binaryVersion = if ($null -ne $binaryManifest.PSObject.Properties['version']) { [int]$binaryManifest.version } else { 0 }
if ($binaryVersion -lt 2 -or [string]$binaryManifest.descriptorType -ne 'quantized-uint8' -or
    -not [bool]$binaryManifest.deflated) {
    throw ("Pack $($manifest.packId) ships the legacy float32 feature encoding (version=$binaryVersion " +
        "descriptorType=$($binaryManifest.descriptorType)). Rebuild it with the current converter, or " +
        "migrate the existing binary with IMaoFeatureMigrate.")
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
# The layered-floor sidecar index carries its own copy of the frame's coordinate transform, and that
# copy is what the runtime tests the player's position against (LayeredFloors::Contains -> the
# occupancy grid). A wrong one is invisible: identification keeps working, own-art still dominates,
# and containment simply never matches, so no floor is ever adopted. The shipped 隐海试验场 index sat
# in World's frame for three days that way. Checked here against the same resolver the builder uses,
# so a non-World index built with World's origin cannot ship again.
$layeredIndexPath = Join-Path $PackRoot 'layered-floors/floor-index.json'
$layeredFloorCount = 0
if (Test-Path -LiteralPath $layeredIndexPath) {
    . (Join-Path $PSScriptRoot 'SceneCoordinateTransform.ps1')
    $layeredIndex = Get-Content -LiteralPath $layeredIndexPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ([int]$layeredIndex.frame -ne [int]$manifest.source.state) {
        throw "Layered floor index frame $($layeredIndex.frame) does not match the pack's Kuro state $($manifest.source.state)."
    }
    $expectedTransform = Get-SceneCoordinateTransform -SourceRoot (Split-Path -Parent $PSScriptRoot) `
        -Frame ([int]$layeredIndex.frame) -Scene ([string]$manifest.scene)
    foreach ($field in 'originX', 'originY', 'scale') {
        $actual = [double]$layeredIndex.coordinateTransform.$field
        $wanted = [double]$expectedTransform.$field
        if ([Math]::Abs($actual - $wanted) -gt 0.001) {
            throw "Layered floor index $field is $actual but frame $($layeredIndex.frame) resolves to $wanted ($($expectedTransform.Source))."
        }
    }
    $layeredFloorCount = @($layeredIndex.floors).Count
    if ($layeredFloorCount -lt 1) { throw 'Layered floor index lists no floors.' }

    # Every floor states whether its map is open to the surface ("开放分层地图": an above-ground map
    # whose ground floor IS the surface, so standing on it suppresses nothing) or enclosed (a cave /
    # underground ruin / building interior, which hides everything that is not part of it). The
    # runtime reads an absent field as enclosed, which is exactly what used to hide 星炬学院's
    # collectibles - so a shipped index must state it, and a rebuilt one that dropped it is a failure
    # here rather than a player report. A value this checker does not know is refused for the same
    # reason. See Docs/LayeredMapOpenness_20260927.md.
    . (Join-Path $PSScriptRoot 'LayeredSurfaceAccess.ps1')
    foreach ($floor in @($layeredIndex.floors)) {
        $access = if ($null -ne $floor.PSObject.Properties['surfaceAccess']) { [string]$floor.surfaceAccess } else { '' }
        if ($access -ne $script:SurfaceAccessOpen -and $access -ne $script:SurfaceAccessEnclosed) {
            throw "Layered floor $($floor.floorId) has no valid surfaceAccess (got '$access'); run scripts/Set-LayeredSurfaceAccess.ps1."
        }
        # Open is a reviewed game fact, never something a rebuild should acquire on its own: an index
        # that starts calling a new map open would silently stop hiding its surface markers.
        if ($access -eq $script:SurfaceAccessOpen) {
            $region = Split-Path -Leaf $PackRoot
            if ($script:LayeredSurfaceAccessExpectedOpen -notcontains "$region/$($floor.floorId)") {
                throw "Layered floor $region/$($floor.floorId) is marked open but is not a recorded open map; review scripts/LayeredSurfaceAccess.ps1."
            }
        }
    }
    $openFloors = @($layeredIndex.floors | Where-Object { [string]$_.surfaceAccess -eq $script:SurfaceAccessOpen } |
        ForEach-Object { [string]$_.floorId })
    Write-Host ("  layered floors: {0} (open to the surface: {1})" -f $layeredFloorCount,
        $(if ($openFloors.Count -eq 0) { 'none' } else { $openFloors -join ', ' }))
}
Write-Host "Kuro tile feature pack valid: pack=$($manifest.packId) coordinates=$($appearances.Count) tileEntries=$(@($manifest.tiles).Count) layered=$layeredCount keypoints=$xmlCount resource=$($manifest.resourceVersion) featureSource=$featureSourceVerified layeredFloors=$layeredFloorCount" -ForegroundColor Green
