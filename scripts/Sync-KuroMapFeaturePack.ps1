[CmdletBinding(DefaultParameterSetName = 'Check')]
param(
    [Parameter(ParameterSetName = 'Check')]
    [switch]$Check,
    [Parameter(Mandatory = $true, ParameterSetName = 'Apply')]
    [switch]$Apply,
    [string]$PackId = 'Dreamzhou',
    [ValidateSet('World', 'Tethys', 'Fabricatorium', 'Avinoleum', 'Lahai', 'LowerVault', 'Darkplain', 'TimeRiftRuins')]
    [string]$Scene = 'World',
    [int]$State = 8,
    [double]$AnchorWorldX = -6725,
    [double]$AnchorWorldY = -919,
    [double]$TransformOriginX = 2474,
    [double]$TransformOriginY = 1957,
    [double]$TransformScale = 1.205,
    [string]$ReferencePath = '',
    # Generates a hash-checked tile/feature package without a real minimap
    # reference. This is useful for broad coverage before field collection,
    # but manifests stay explicitly unverified and the normal test script
    # rejects them unless -AllowUnverified is specified.
    [switch]$SkipReferenceVerification,
    # Public map bounds are irregular. When enabled, only a confirmed HTTP 404
    # is treated as an absent tile; transport failures and other HTTP errors
    # still fail the build.
    [switch]$AllowMissingTiles,
    [ValidateRange(1, 4)]
    [int]$TileRadius = 2,
    [Nullable[int]]$TileMinX = $null,
    [Nullable[int]]$TileMaxX = $null,
    [Nullable[int]]$TileMinY = $null,
    [Nullable[int]]$TileMaxY = $null,
    [string]$PaddleLib = $env:IMAO_PADDLE_LIB,
    [string]$OpenCvDir = $env:IMAO_OPENCV_DIR
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$kuroApiHost = 'api.kurobbs.com'
$kuroStaticHost = 'web-static.kurobbs.com'
$tileSize = 1024
$kuroVirtualMapSize = 850.0
$sceneIds = @{ World = 1; Tethys = 2; Fabricatorium = 3; Avinoleum = 4; Lahai = 5; LowerVault = 6; Darkplain = 7; TimeRiftRuins = 8 }
$expectedStates = @{ World = 8; Tethys = 900; Fabricatorium = 905; Avinoleum = 903; Lahai = 906; LowerVault = 902; Darkplain = 909; TimeRiftRuins = 910 }
if ($State -ne $expectedStates[$Scene]) { throw "State $State does not belong to scene $Scene." }
if ([string]::IsNullOrWhiteSpace($PackId) -or $PackId -notmatch '^[A-Za-z0-9_-]+$') { throw 'PackId must be an ASCII directory name.' }
if ($TransformScale -le 0 -or [double]::IsNaN($TransformScale) -or [double]::IsInfinity($TransformScale)) { throw 'TransformScale must be finite and positive.' }

function Assert-KuroUri([string]$Url, [string[]]$AllowedHosts) {
    $uri = [Uri]$Url
    if ($uri.Scheme -ne 'https' -or $AllowedHosts -notcontains $uri.Host) {
        throw "Refusing non-public Kuro map URI: $Url"
    }
}

function Invoke-KuroDownload([string]$Url, [string]$Destination) {
    Assert-KuroUri $Url @($kuroStaticHost)
    $curlOutput = & curl.exe --fail --silent --show-error --location --proto '=https' --tlsv1.2 --connect-timeout 15 --max-time 60 $Url --output $Destination 2>&1
    $exitCode = $LASTEXITCODE
    if ($exitCode -eq 0 -and (Test-Path -LiteralPath $Destination)) { return $true }
    if ($AllowMissingTiles -and $exitCode -eq 22 -and ($curlOutput -match '(?i)\b404\b')) {
        if (Test-Path -LiteralPath $Destination) { [IO.File]::Delete($Destination) }
        Write-Verbose "Kuro map tile is absent (404): $Url"
        return $false
    }
    throw "Download failed: $Url $curlOutput"
}

function Get-KuroResourceVersion([string]$Destination) {
    $url = "https://$kuroApiHost/map/core/config/getMapResource"
    Assert-KuroUri $url @($kuroApiHost)
    & curl.exe --fail --silent --show-error --location --proto '=https' --tlsv1.2 --connect-timeout 15 --max-time 60 -X POST $url -H 'content-type: application/json' -d '{}' --output $Destination
    if ($LASTEXITCODE -ne 0) { throw 'Kuro map resource request failed.' }
    $response = Get-Content -LiteralPath $Destination -Raw | ConvertFrom-Json
    if ($response.code -ne 200 -or [string]::IsNullOrWhiteSpace([string]$response.data)) { throw 'Kuro map resource response was invalid.' }
    $value = [string]$response.data
    if ($value -notmatch '^[A-Fa-f0-9]{32}$') { throw 'Kuro map resource version was invalid.' }
    return $value.ToUpperInvariant()
}

function Assert-MapTilePng([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 24 -or $bytes[0] -ne 0x89 -or $bytes[1] -ne 0x50 -or $bytes[2] -ne 0x4E -or $bytes[3] -ne 0x47 -or $bytes[4] -ne 0x0D -or $bytes[5] -ne 0x0A -or $bytes[6] -ne 0x1A -or $bytes[7] -ne 0x0A) {
        throw "Tile is not a PNG: $Path"
    }
    $width = (([int]$bytes[16] -shl 24) -bor ([int]$bytes[17] -shl 16) -bor ([int]$bytes[18] -shl 8) -bor [int]$bytes[19])
    $height = (([int]$bytes[20] -shl 24) -bor ([int]$bytes[21] -shl 16) -bor ([int]$bytes[22] -shl 8) -bor [int]$bytes[23])
    if ($width -ne $tileSize -or $height -ne $tileSize) { throw "Tile dimensions are not 1024x1024: $Path" }
}

function Write-Utf8Json([object]$Value, [string]$Path) {
    $parent = Split-Path -Parent $Path
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    [IO.File]::WriteAllText($Path, (($Value | ConvertTo-Json -Depth 16) + [Environment]::NewLine), [Text.UTF8Encoding]::new($false))
}

function Get-LocalTileY([double]$KuroY) {
    # The raw URL Y axis is the inverse of Leaflet's screen Y axis. A tile covers
    # y*1024 - pixelY, so the correct tile index uses ceiling instead of floor.
    return [int][Math]::Ceiling(($KuroY / $tileSize) - 0.0000001)
}

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('imao-kuro-feature-pack-' + [guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null
    $rawDir = Join-Path $tempRoot 'raw'
    $tileDir = Join-Path $rawDir 'tiles'
    $generatedDir = Join-Path $tempRoot 'generated'
    New-Item -ItemType Directory -Force -Path $tileDir, $generatedDir | Out-Null

    $resourceVersion = Get-KuroResourceVersion (Join-Path $rawDir 'resource.json')
    $kuroX = $AnchorWorldX * $tileSize / $kuroVirtualMapSize + $tileSize
    $kuroY = -$AnchorWorldY * $tileSize / $kuroVirtualMapSize
    $centerTileX = [int][Math]::Floor($kuroX / $tileSize)
    $centerTileY = Get-LocalTileY $kuroY
    $specifiedBounds = @(@($TileMinX, $TileMaxX, $TileMinY, $TileMaxY) |
        Where-Object { $null -ne $_ }).Count
    if ($specifiedBounds -ne 0 -and $specifiedBounds -ne 4) {
        throw 'TileMinX, TileMaxX, TileMinY, and TileMaxY must be specified together.'
    }
    $usesExplicitBounds = $specifiedBounds -eq 4
    $minimumTileX = if ($usesExplicitBounds) { [int]$TileMinX } else { $centerTileX - $TileRadius }
    $maximumTileX = if ($usesExplicitBounds) { [int]$TileMaxX } else { $centerTileX + $TileRadius }
    $minimumTileY = if ($usesExplicitBounds) { [int]$TileMinY } else { $centerTileY - $TileRadius }
    $maximumTileY = if ($usesExplicitBounds) { [int]$TileMaxY } else { $centerTileY + $TileRadius }
    if ($minimumTileX -gt $maximumTileX -or $minimumTileY -gt $maximumTileY) {
        throw 'Explicit tile bounds are inverted.'
    }
    if ($centerTileX -lt $minimumTileX -or $centerTileX -gt $maximumTileX -or
        $centerTileY -lt $minimumTileY -or $centerTileY -gt $maximumTileY) {
        throw 'Explicit tile bounds must contain the reference anchor tile.'
    }
    $requestedTileCount = ($maximumTileX - $minimumTileX + 1) * ($maximumTileY - $minimumTileY + 1)
    if ($requestedTileCount -lt 1 -or $requestedTileCount -gt 256) {
        throw "Requested tile bounds contain an unsupported number of tiles: $requestedTileCount"
    }
    $tiles = [Collections.Generic.List[object]]::new()
    $missingTileCount = 0

    for ($tileX = $minimumTileX; $tileX -le $maximumTileX; ++$tileX) {
        for ($tileY = $minimumTileY; $tileY -le $maximumTileY; ++$tileY) {
            $fileName = "${State}_${tileX}_${tileY}.png"
            $destination = Join-Path $tileDir $fileName
            $url = "https://$kuroStaticHost/mcmap/tiles/$resourceVersion/$State/$fileName"
            if (-not (Invoke-KuroDownload $url $destination)) {
                ++$missingTileCount
                continue
            }
            Assert-MapTilePng $destination
            $tiles.Add([ordered]@{
                x = $tileX; y = $tileY; file = "tiles/$fileName"
                sha256 = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
            })
        }
    }
    if ($tiles.Count -eq 0) { throw 'No source tiles were available in the requested bounds.' }

    $tileManifest = [ordered]@{
        formatVersion = 1; packId = $PackId; scene = $Scene; sceneId = $sceneIds[$Scene]; resourceVersion = $resourceVersion
        source = [ordered]@{ static = "https://$kuroStaticHost"; state = $State; tileSize = $tileSize; virtualMapSize = $kuroVirtualMapSize }
        coordinateTransform = [ordered]@{ originX = $TransformOriginX; originY = $TransformOriginY; scale = $TransformScale }
        anchorWorldCoordinate = [ordered]@{ x = $AnchorWorldX; y = $AnchorWorldY }
        tileRadius = if ($usesExplicitBounds) { $null } else { $TileRadius }
        tileBounds = [ordered]@{ minX = $minimumTileX; maxX = $maximumTileX; minY = $minimumTileY; maxY = $maximumTileY }
        tiles = @($tiles); missingTileCount = $missingTileCount
    }
    $tileManifestPath = Join-Path $rawDir 'tiles.json'
    Write-Utf8Json $tileManifest $tileManifestPath
    $featurePath = Join-Path $generatedDir 'features.yml'
    $builderReportPath = Join-Path $generatedDir 'builder-report.json'
    if ([string]::IsNullOrWhiteSpace($ReferencePath) -and $PackId -eq 'Dreamzhou' -and -not $SkipReferenceVerification) {
        $ReferencePath = Join-Path $repoRoot 'Assets\FeaturesDatas\DreamzhouCandidate\reference--6725--919.png'
    }
    $referencePath = if ($SkipReferenceVerification) { '' } else { $ReferencePath }
    if (-not $SkipReferenceVerification -and -not (Test-Path -LiteralPath $referencePath)) {
        throw "Missing reference minimap: $referencePath"
    }
    $builderArguments = @{
        TileManifest = $tileManifestPath; Output = $featurePath; Report = $builderReportPath
        PaddleLib = $PaddleLib; OpenCvDir = $OpenCvDir
    }
    if (-not $SkipReferenceVerification) {
        $builderArguments.VerifyReference = $referencePath
        $builderArguments.AnchorWorldX = $AnchorWorldX
        $builderArguments.AnchorWorldY = $AnchorWorldY
    }
    & (Join-Path $PSScriptRoot 'Build-KuroMapFeaturePack.ps1') @builderArguments
    if ($LASTEXITCODE -ne 0) { throw "Feature-pack builder failed with exit code $LASTEXITCODE." }

    $builderReport = Get-Content -LiteralPath $builderReportPath -Raw | ConvertFrom-Json
    if ($builderReport.selectedKeypoints -lt 12 -or [string]::IsNullOrWhiteSpace([string]$builderReport.featuresSha256) -or
        (-not $SkipReferenceVerification -and ($null -eq $builderReport.referenceVerification -or -not [bool]$builderReport.referenceVerification.passed))) {
        throw 'Feature-pack builder report did not satisfy minimum validation.'
    }
    $referenceVerification = if ($SkipReferenceVerification) {
        [ordered]@{
            skipped = $true; passed = $false; errorPixels = $null
            reason = 'No game minimap reference was supplied; coverage package is not field-verified.'
        }
    }
    else {
        $builderReport.referenceVerification
    }
    $packManifest = [ordered]@{
        formatVersion = 1; packId = "$($PackId.ToLowerInvariant())-kurotiles"; scene = $Scene; sceneId = $sceneIds[$Scene]; resourceVersion = $resourceVersion
        generatedAtUtc = [DateTime]::UtcNow.ToString('o')
        source = $tileManifest.source; coordinateTransform = $tileManifest.coordinateTransform; anchorWorldCoordinate = $tileManifest.anchorWorldCoordinate
        tileRadius = $tileManifest.tileRadius; tileBounds = $tileManifest.tileBounds
        tiles = @($tiles); missingTileCount = $missingTileCount; coordinateBounds = $builderReport.coordinateBounds
        referenceVerification = $referenceVerification
        features = [ordered]@{ file = 'features.yml'; sha256 = [string]$builderReport.featuresSha256; keypointCount = [int]$builderReport.selectedKeypoints; extractedKeypointCount = [int]$builderReport.extractedKeypoints }
    }
    $generatedManifestPath = Join-Path $generatedDir 'manifest.json'
    Write-Utf8Json $packManifest $generatedManifestPath
    $reportLines = @(
        '# Kuro map feature-pack report', '',
        "- Pack: $($packManifest.packId)",
        "- Kuro resource version: $resourceVersion",
        "- Scene: $Scene (state $State)",
        "- Anchor game coordinate: $AnchorWorldX, $AnchorWorldY",
        "- Tile center: $centerTileX, $centerTileY; bounds: x=$minimumTileX..$maximumTileX, y=$minimumTileY..$maximumTileY",
        "- Tiles validated: $($tiles.Count)",
        "- Tiles absent from the public source: $missingTileCount",
        "- SURF keypoints extracted: $($builderReport.extractedKeypoints)",
        "- SURF keypoints retained: $($builderReport.selectedKeypoints)",
        "- Reference-map verification: passed=$($referenceVerification.passed); skipped=$($referenceVerification.skipped); error=$($referenceVerification.errorPixels) pixels",
        "- Feature SHA-256: $($builderReport.featuresSha256)", '',
        'The tiles were downloaded from Kuro public static assets. An unverified coverage pack must be checked against a real minimap before accuracy is claimed.'
    )
    [IO.File]::WriteAllLines((Join-Path $generatedDir 'report.md'), $reportLines, [Text.UTF8Encoding]::new($false))

    $target = Join-Path $repoRoot "Assets\FeaturesDatas\KuroTilePacks\$PackId"
    $existingHash = if (Test-Path -LiteralPath (Join-Path $target 'features.yml')) { (Get-FileHash -LiteralPath (Join-Path $target 'features.yml') -Algorithm SHA256).Hash.ToLowerInvariant() } else { '' }
    $status = if ($existingHash -eq $packManifest.features.sha256) { 'unchanged' } elseif ([string]::IsNullOrWhiteSpace($existingHash)) { 'new' } else { 'changed' }
    Write-Host "Kuro tile feature pack $($PSCmdlet.ParameterSetName): pack=$PackId resource=$resourceVersion tiles=$($tiles.Count) selected=$($builderReport.selectedKeypoints) status=$status"
    Write-Host "Anchor=$AnchorWorldX,$AnchorWorldY -> raw tile center=$centerTileX,$centerTileY bounds=x:$minimumTileX..$maximumTileX y:$minimumTileY..$maximumTileY"

    if ($Apply) {
        New-Item -ItemType Directory -Force -Path $target | Out-Null
        Copy-Item -LiteralPath $featurePath, $generatedManifestPath, (Join-Path $generatedDir 'report.md') -Destination $target -Force
        Write-Host "Applied feature pack: $target" -ForegroundColor Green
    }
}
finally {
    if (Test-Path -LiteralPath $tempRoot) { Remove-Item -LiteralPath $tempRoot -Recurse -Force }
}
