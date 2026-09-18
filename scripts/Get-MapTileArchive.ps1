[CmdletBinding()]
param(
    [string]$SourceRoot,
    # Named *Path so it cannot collide with the parsed $registry object: PowerShell
    # variable names are case-insensitive and a [string] parameter would coerce it.
    [string]$RegistryPath,
    [string]$ArchiveRoot,
    # Generation to download tiles from. Defaults to the generation the registry was
    # derived from. A different value is a substitution and must verify byte-identity
    # against the tile hashes recorded in the shipped packs.
    [string]$ResourceVersion,
    # Ask the public API for the generation it serves right now and use that.
    [switch]$ResolveCurrentVersion,
    [string[]]$RegionId = @(),
    [ValidateRange(1, 32)][int]$ThrottleLimit = 8,
    # The public tile set is irregular, so some tiles are legitimately absent: the
    # shipped packs record 20-45% absent inside their own rectangular windows, and a
    # bounding box over an irregular region covers more empty space than that. The
    # guard exists to catch a wrong URL pattern or generation (which yields ~100%),
    # so the hard limit is high and a lower ratio only warns.
    [ValidateRange(0.0, 1.0)][double]$MaxAbsentRatio = 0.75,
    [ValidateRange(0.0, 1.0)][double]$WarnAbsentRatio = 0.3,
    [switch]$FailOnMissing,
    [switch]$Check
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $SourceRoot) { $SourceRoot = Split-Path -Parent $PSScriptRoot }
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
if (-not $RegistryPath) { $RegistryPath = Join-Path $SourceRoot 'map-regions/regions.json' }
if (-not $ArchiveRoot) { $ArchiveRoot = Join-Path $SourceRoot 'map-regions/tiles' }
$RegistryPath = [IO.Path]::GetFullPath($RegistryPath)
$ArchiveRoot = [IO.Path]::GetFullPath($ArchiveRoot)

$kuroStaticHost = 'web-static.kurobbs.com'
$kuroApiHost = 'api.kurobbs.com'
$tileSize = 1024

if (-not (Test-Path -LiteralPath $RegistryPath)) { throw "Missing region registry: $RegistryPath" }
$registry = Get-Content -LiteralPath $RegistryPath -Raw -Encoding UTF8 | ConvertFrom-Json -AsHashtable
if ([int]$registry['formatVersion'] -ne 1) { throw 'Unsupported region registry format.' }
$recordedVersion = ([string]$registry['generatedFrom']['kuroResourceVersion']).ToUpperInvariant()
if ($recordedVersion -notmatch '^[A-Fa-f0-9]{32}$') { throw "Registry records an invalid resource version: $recordedVersion" }

function Get-CurrentKuroResourceVersion {
    $response = & curl.exe --fail --silent --show-error --location --proto '=https' --tlsv1.2 `
        --connect-timeout 15 --max-time 60 -X POST "https://$kuroApiHost/map/core/config/getMapResource" `
        -H 'content-type: application/json' -d '{}' 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Could not read the current Kuro map resource version: $response" }
    $parsed = $response | ConvertFrom-Json
    if ([int]$parsed.code -ne 200 -or ([string]$parsed.data) -notmatch '^[A-Fa-f0-9]{32}$') {
        throw "Kuro map resource response was invalid: $response"
    }
    return ([string]$parsed.data).ToUpperInvariant()
}

$tileVersion = $recordedVersion
if ($ResolveCurrentVersion) { $tileVersion = Get-CurrentKuroResourceVersion }
elseif ($ResourceVersion) {
    if ($ResourceVersion -notmatch '^[A-Fa-f0-9]{32}$') { throw "Resource version is not a 32-character hex value: $ResourceVersion" }
    $tileVersion = $ResourceVersion.ToUpperInvariant()
}
$substituted = $tileVersion -ne $recordedVersion

Write-Host "Registry generation: $recordedVersion"
Write-Host "Tile generation:     $tileVersion"
if ($substituted) {
    Write-Warning "Tile generation differs from the generation the registry was derived from. The archive will be verified byte-for-byte against the tile hashes recorded in the shipped packs; a single mismatch aborts the run."
}

$selected = @($registry['regions'] | Where-Object { $null -ne $_['tileBounds'] })
if ($RegionId.Count -gt 0) {
    $known = @($registry['regions'] | ForEach-Object { [string]$_['id'] })
    foreach ($id in $RegionId) { if ($known -notcontains $id) { throw "Unknown region id: $id" } }
    $wanted = [Collections.Generic.HashSet[string]]::new([string[]]$RegionId, [StringComparer]::OrdinalIgnoreCase)
    $selected = @($selected | Where-Object { $wanted.Contains([string]$_['id']) })
    if ($selected.Count -eq 0) { throw 'No region matched the requested ids.' }
}

$unbuildable = @($selected | Where-Object { -not [bool]$_['buildable'] })
if ($unbuildable.Count -gt 0 -and $RegionId.Count -eq 0) {
    Write-Warning ("Skipping {0} region(s) whose tile window is not trustworthy: {1}" -f $unbuildable.Count,
        (($unbuildable | ForEach-Object { "$($_['id'])($($_['tileConfidence']))" }) -join ', '))
    $selected = @($selected | Where-Object { [bool]$_['buildable'] })
}

# One file per (state, tile). Two regions that need the same tile share it, which is
# why the archive path is keyed by tile coordinate and not by region.
$targets = [Collections.Generic.List[object]]::new()
foreach ($regionRecord in $selected) {
    $state = [int]$regionRecord['frame']
    $bounds = $regionRecord['tileBounds']
    $tileCount = 0
    for ($x = [int]$bounds['minX']; $x -le [int]$bounds['maxX']; $x++) {
        for ($y = [int]$bounds['minY']; $y -le [int]$bounds['maxY']; $y++) {
            $name = "${state}_${x}_${y}.png"
            $targets.Add([pscustomobject]@{
                Region = [string]$regionRecord['id']
                State = $state
                X = $x
                Y = $y
                Name = $name
                Path = Join-Path $ArchiveRoot "$tileVersion/$state/$name"
                Url = "https://$kuroStaticHost/mcmap/tiles/$tileVersion/$state/$name"
            })
            ++$tileCount
        }
    }
    Write-Host ("  {0,-14} {1,-14} state={2,-4} x {3}..{4} y {5}..{6} -> {7} tiles" -f `
        $regionRecord['id'], $regionRecord['name'], $state, $bounds['minX'], $bounds['maxX'], $bounds['minY'], $bounds['maxY'], $tileCount)
}
$unique = @($targets | Group-Object Path | ForEach-Object { $_.Group[0] })
Write-Host "Regions: $($selected.Count)  tile requests: $($targets.Count)  unique tiles: $($unique.Count)"
if ($unique.Count -eq 0) { throw 'No tiles were selected; refusing to report an empty archive as success.' }

function Test-MapTilePng([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    if ([IO.FileInfo]::new($Path).Length -lt 24) { return $false }
    $stream = [IO.File]::OpenRead($Path)
    try {
        $header = [byte[]]::new(24)
        if ($stream.Read($header, 0, 24) -ne 24) { return $false }
    }
    finally { $stream.Dispose() }
    $signature = @(0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A)
    for ($i = 0; $i -lt 8; $i++) { if ($header[$i] -ne $signature[$i]) { return $false } }
    $width = ([int]$header[16] -shl 24) -bor ([int]$header[17] -shl 16) -bor ([int]$header[18] -shl 8) -bor [int]$header[19]
    $height = ([int]$header[20] -shl 24) -bor ([int]$header[21] -shl 16) -bor ([int]$header[22] -shl 8) -bor [int]$header[23]
    return $width -eq $tileSize -and $height -eq $tileSize
}

$pending = [Collections.Generic.List[object]]::new()
$present = 0
foreach ($target in $unique) {
    if (Test-MapTilePng $target.Path) { ++$present } else { $pending.Add($target) }
}
Write-Host "Cached: $present  to fetch: $($pending.Count)"
if ($Check) { Write-Host 'Check only; nothing was downloaded.'; return }

$absent = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$failed = [Collections.Generic.List[string]]::new()
if ($pending.Count -gt 0) {
    $results = $pending | ForEach-Object -Parallel {
        $target = $_
        [IO.Directory]::CreateDirectory((Split-Path -Parent $target.Path)) | Out-Null
        $partial = $target.Path + '.part'
        if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Force }
        $output = & curl.exe --fail --silent --show-error --location --proto '=https' --tlsv1.2 `
            --connect-timeout 15 --max-time 90 --retry 2 --retry-delay 2 `
            $target.Url --output $partial 2>&1
        $code = $LASTEXITCODE
        if ($code -eq 0 -and (Test-Path -LiteralPath $partial)) {
            Move-Item -LiteralPath $partial -Destination $target.Path -Force
            return [pscustomobject]@{ Name = $target.Name; Status = 'ok'; Detail = '' }
        }
        if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Force }
        # curl exit 22 is --fail's HTTP error; only a literal 404 is an absent tile.
        if ($code -eq 22 -and ($output -match '(?i)\b404\b')) {
            return [pscustomobject]@{ Name = $target.Name; Status = 'absent'; Detail = '' }
        }
        return [pscustomobject]@{ Name = $target.Name; Status = 'failed'; Detail = "exit=$code $output" }
    } -ThrottleLimit $ThrottleLimit
    foreach ($result in $results) {
        switch ($result.Status) {
            'absent' { [void]$absent.Add($result.Name) }
            'failed' { $failed.Add("$($result.Name): $($result.Detail)") }
        }
    }
}

if ($failed.Count -gt 0) {
    $failed | Select-Object -First 10 | ForEach-Object { Write-Warning $_ }
    throw "$($failed.Count) tile downloads failed with transport or server errors."
}

$absentRatio = $absent.Count / [double]$unique.Count
if ($absent.Count -eq $unique.Count) {
    throw "Every one of the $($unique.Count) requested tiles is absent upstream. The URL pattern or the tile generation is wrong; refusing to report an empty archive as success."
}
if ($absentRatio -gt $MaxAbsentRatio) {
    throw "$($absent.Count) of $($unique.Count) tiles are absent upstream ($([Math]::Round($absentRatio * 100, 1))%), above the allowed $([Math]::Round($MaxAbsentRatio * 100, 1))%. The URL pattern or the tile generation is probably wrong."
}
if ($absentRatio -gt $WarnAbsentRatio) {
    Write-Warning "$($absent.Count) of $($unique.Count) tiles ($([Math]::Round($absentRatio * 100, 1))%) are absent upstream. Rectangular windows over an irregular map shape include empty space; this is expected but should be reviewed per region."
}
if ($FailOnMissing -and $absent.Count -gt 0) { throw "$($absent.Count) tiles are absent upstream." }

# ---------------------------------------------------------------------------
# Generation substitution evidence
#
# The shipped packs record the SHA-256 of every tile they were built from. When the
# archive is fetched from a generation other than the registry's, those recorded
# hashes are the only available proof that the map imagery did not change.
# ---------------------------------------------------------------------------
$verification = [ordered]@{ performed = $false; compared = 0; mismatched = 0; samples = @() }
if ($substituted -and -not $Check) {
    $recorded = @{}
    foreach ($manifestFile in @(Get-ChildItem -LiteralPath (Join-Path $SourceRoot 'Assets/FeaturesDatas/KuroTilePacks') -Filter 'manifest.json' -Recurse -File)) {
        $pack = Get-Content -LiteralPath $manifestFile.FullName -Raw -Encoding UTF8 | ConvertFrom-Json -AsHashtable
        $state = [int]$pack['source']['state']
        foreach ($tile in @($pack['tiles'])) {
            $key = "$state|$([int]$tile['x'])|$([int]$tile['y'])"
            if (-not $recorded.ContainsKey($key)) { $recorded[$key] = ([string]$tile['sha256']).ToLowerInvariant() }
        }
    }
    $verification.performed = $true
    foreach ($target in $unique) {
        $key = "$($target.State)|$($target.X)|$($target.Y)"
        if (-not $recorded.ContainsKey($key)) { continue }
        if ($absent.Contains($target.Name)) { continue }
        $actual = (Get-FileHash -LiteralPath $target.Path -Algorithm SHA256).Hash.ToLowerInvariant()
        ++$verification.compared
        if ($actual -ne $recorded[$key]) {
            ++$verification.mismatched
            if ($verification.samples.Count -lt 5) { $verification.samples += "$key expected=$($recorded[$key]) actual=$actual" }
        }
    }
    Write-Host "Generation substitution: compared $($verification.compared) tiles against the shipped packs, $($verification.mismatched) mismatched."
    if ($verification.compared -eq 0) {
        throw 'A substituted tile generation could not be verified against any recorded tile hash; refusing to archive unverified tiles.'
    }
    if ($verification.mismatched -gt 0) {
        foreach ($sample in $verification.samples) { Write-Warning $sample }
        throw "The substituted tile generation differs from the recorded tiles in $($verification.mismatched) of $($verification.compared) comparisons. The map imagery changed; regenerate the region registry and recalibrate before archiving."
    }
}

# ---------------------------------------------------------------------------
$entries = [Collections.Generic.List[object]]::new()
$byRegion = @{}
foreach ($target in $targets) {
    if (-not $byRegion.ContainsKey($target.Region)) { $byRegion[$target.Region] = [Collections.Generic.List[object]]::new() }
    $isAbsent = $absent.Contains($target.Name)
    $entry = [ordered]@{
        x = $target.X; y = $target.Y; file = "tiles/$($target.Name)"
        sha256 = if ($isAbsent) { $null } else { (Get-FileHash -LiteralPath $target.Path -Algorithm SHA256).Hash.ToLowerInvariant() }
        absent = $isAbsent
    }
    $byRegion[$target.Region].Add($entry)
    $entries.Add([pscustomobject]@{ Region = $target.Region; Entry = $entry })
}

$manifest = [ordered]@{
    formatVersion = 1
    registryResourceVersion = $recordedVersion
    tileResourceVersion = $tileVersion
    substituted = $substituted
    archiveRoot = "map-regions/tiles/$tileVersion"
    generatedAtUtc = [DateTime]::UtcNow.ToString('o')
    tileSize = $tileSize
    verification = $verification
    regions = [ordered]@{}
}
foreach ($id in ($byRegion.Keys | Sort-Object)) {
    $list = $byRegion[$id]
    $manifest.regions[$id] = [ordered]@{
        tiles = @($list | ForEach-Object { $_ })
        present = @($list | Where-Object { -not $_['absent'] }).Count
        absent = @($list | Where-Object { $_['absent'] }).Count
    }
}
$manifestPath = Join-Path $ArchiveRoot 'tiles.manifest.json'
[IO.File]::WriteAllText($manifestPath, (($manifest | ConvertTo-Json -Depth 8) + [Environment]::NewLine), [Text.UTF8Encoding]::new($false))

$totalPresent = @($entries | Where-Object { -not $_.Entry['absent'] }).Count
Write-Host "Archived $totalPresent tiles under $ArchiveRoot/$tileVersion" -ForegroundColor Green
Write-Host "Wrote $manifestPath" -ForegroundColor Green
Write-Host "Absent upstream: $($absent.Count) of $($unique.Count)"
