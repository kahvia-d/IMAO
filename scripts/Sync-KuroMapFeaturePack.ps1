[CmdletBinding(DefaultParameterSetName = 'Check')]
param(
    [Parameter(ParameterSetName = 'Check')]
    [switch]$Check,
    [Parameter(Mandatory = $true, ParameterSetName = 'Apply')]
    [switch]$Apply,
    [ValidateSet('Dreamzhou')]
    [string]$PackId = 'Dreamzhou',
    [double]$AnchorWorldX = -6725,
    [double]$AnchorWorldY = -919,
    [ValidateRange(1, 4)]
    [int]$TileRadius = 2,
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

function Assert-KuroUri([string]$Url, [string[]]$AllowedHosts) {
    $uri = [Uri]$Url
    if ($uri.Scheme -ne 'https' -or $AllowedHosts -notcontains $uri.Host) {
        throw "Refusing non-public Kuro map URI: $Url"
    }
}

function Invoke-KuroDownload([string]$Url, [string]$Destination) {
    Assert-KuroUri $Url @($kuroStaticHost)
    & curl.exe --fail --silent --show-error --location --proto '=https' --tlsv1.2 --connect-timeout 15 --max-time 60 $Url --output $Destination
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $Destination)) { throw "Download failed: $Url" }
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
    $tiles = [Collections.Generic.List[object]]::new()

    for ($tileX = $centerTileX - $TileRadius; $tileX -le $centerTileX + $TileRadius; ++$tileX) {
        for ($tileY = $centerTileY - $TileRadius; $tileY -le $centerTileY + $TileRadius; ++$tileY) {
            $fileName = "8_${tileX}_${tileY}.png"
            $destination = Join-Path $tileDir $fileName
            $url = "https://$kuroStaticHost/mcmap/tiles/$resourceVersion/8/$fileName"
            Invoke-KuroDownload $url $destination
            Assert-MapTilePng $destination
            $tiles.Add([ordered]@{
                x = $tileX; y = $tileY; file = "tiles/$fileName"
                sha256 = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
            })
        }
    }

    $tileManifest = [ordered]@{
        formatVersion = 1; packId = $PackId; scene = 'World'; resourceVersion = $resourceVersion
        source = [ordered]@{ static = "https://$kuroStaticHost"; state = 8; tileSize = $tileSize; virtualMapSize = $kuroVirtualMapSize }
        anchorWorldCoordinate = [ordered]@{ x = $AnchorWorldX; y = $AnchorWorldY }
        tileRadius = $TileRadius; tiles = @($tiles)
    }
    $tileManifestPath = Join-Path $rawDir 'tiles.json'
    Write-Utf8Json $tileManifest $tileManifestPath
    $featurePath = Join-Path $generatedDir 'features.yml'
    $builderReportPath = Join-Path $generatedDir 'builder-report.json'
    $referencePath = Join-Path $repoRoot 'Assets\FeaturesDatas\DreamzhouCandidate\reference--6725--919.png'
    if (-not (Test-Path -LiteralPath $referencePath)) { throw "Missing Dreamzhou reference minimap: $referencePath" }
    & (Join-Path $PSScriptRoot 'Build-KuroMapFeaturePack.ps1') -TileManifest $tileManifestPath -Output $featurePath -Report $builderReportPath -VerifyReference $referencePath -AnchorWorldX $AnchorWorldX -AnchorWorldY $AnchorWorldY -PaddleLib $PaddleLib -OpenCvDir $OpenCvDir
    if ($LASTEXITCODE -ne 0) { throw "Feature-pack builder failed with exit code $LASTEXITCODE." }

    $builderReport = Get-Content -LiteralPath $builderReportPath -Raw | ConvertFrom-Json
    if ($builderReport.selectedKeypoints -lt 12 -or [string]::IsNullOrWhiteSpace([string]$builderReport.featuresSha256) -or $null -eq $builderReport.referenceVerification -or -not [bool]$builderReport.referenceVerification.passed) {
        throw 'Feature-pack builder report did not satisfy minimum validation.'
    }
    $packManifest = [ordered]@{
        formatVersion = 1; packId = "$($PackId.ToLowerInvariant())-kurotiles"; scene = 'World'; resourceVersion = $resourceVersion
        generatedAtUtc = [DateTime]::UtcNow.ToString('o')
        source = $tileManifest.source; anchorWorldCoordinate = $tileManifest.anchorWorldCoordinate; tileRadius = $TileRadius
        tiles = @($tiles); coordinateBounds = $builderReport.coordinateBounds
        referenceVerification = $builderReport.referenceVerification
        features = [ordered]@{ file = 'features.yml'; sha256 = [string]$builderReport.featuresSha256; keypointCount = [int]$builderReport.selectedKeypoints; extractedKeypointCount = [int]$builderReport.extractedKeypoints }
    }
    $generatedManifestPath = Join-Path $generatedDir 'manifest.json'
    Write-Utf8Json $packManifest $generatedManifestPath
    $reportLines = @(
        '# Kuro map feature-pack report', '',
        "- Pack: $($packManifest.packId)",
        "- Kuro resource version: $resourceVersion",
        "- Scene: World (state 8)",
        "- Anchor game coordinate: $AnchorWorldX, $AnchorWorldY",
        "- Tile center: $centerTileX, $centerTileY; radius: $TileRadius",
        "- Tiles validated: $($tiles.Count)",
        "- SURF keypoints extracted: $($builderReport.extractedKeypoints)",
        "- SURF keypoints retained: $($builderReport.selectedKeypoints)",
        "- Reference-map verification: passed=$($builderReport.referenceVerification.passed); matches=$($builderReport.referenceVerification.nearAnchorMatches); error=$($builderReport.referenceVerification.errorPixels) pixels",
        "- Feature SHA-256: $($builderReport.featuresSha256)", '',
        'The tiles were downloaded from Kuro public static assets. This pack adds World SURF coverage only; it does not change map-point data or claim support for other state IDs.'
    )
    [IO.File]::WriteAllLines((Join-Path $generatedDir 'report.md'), $reportLines, [Text.UTF8Encoding]::new($false))

    $target = Join-Path $repoRoot "Assets\FeaturesDatas\KuroTilePacks\$PackId"
    $existingHash = if (Test-Path -LiteralPath (Join-Path $target 'features.yml')) { (Get-FileHash -LiteralPath (Join-Path $target 'features.yml') -Algorithm SHA256).Hash.ToLowerInvariant() } else { '' }
    $status = if ($existingHash -eq $packManifest.features.sha256) { 'unchanged' } elseif ([string]::IsNullOrWhiteSpace($existingHash)) { 'new' } else { 'changed' }
    Write-Host "Kuro tile feature pack $($PSCmdlet.ParameterSetName): pack=$PackId resource=$resourceVersion tiles=$($tiles.Count) selected=$($builderReport.selectedKeypoints) status=$status"
    Write-Host "Anchor=$AnchorWorldX,$AnchorWorldY -> raw tile center=$centerTileX,$centerTileY"

    if ($Apply) {
        New-Item -ItemType Directory -Force -Path $target | Out-Null
        Copy-Item -LiteralPath $featurePath, $generatedManifestPath, (Join-Path $generatedDir 'report.md') -Destination $target -Force
        Write-Host "Applied feature pack: $target" -ForegroundColor Green
    }
}
finally {
    if (Test-Path -LiteralPath $tempRoot) { Remove-Item -LiteralPath $tempRoot -Recurse -Force }
}
