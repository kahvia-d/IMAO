[CmdletBinding()]
param(
    [string]$SourceRoot,
    [string]$RegistryPath,
    # Comma-separated region ids, because -File invocation cannot bind an array.
    [string]$RegionId = '',
    [string]$ArchiveRoot,
    # Generation to probe. Defaults to the one the region registry was derived from;
    # -ResolveCurrentVersion asks the public API instead.
    [string]$ResourceVersion,
    [switch]$ResolveCurrentVersion,
    # How far outside the region's point window to look, in tiles. The point window is
    # derived from a possibly unproven origin, so the imagery may sit next to it.
    [ValidateRange(1, 32)][int]$SearchMargin = 8,
    [ValidateRange(1, 32)][int]$ThrottleLimit = 12,
    # A tile at or above this many bytes counts as map imagery; anything smaller that the
    # host still serves is a transparent placeholder. The published gap is wide (placeholders
    # are ~22 KB, the smallest real tile seen is ~225 KB), and the probe prints the size
    # histogram so the threshold can be checked against the data instead of assumed.
    [ValidateRange(1024, 10485760)][int]$ImageryMinBytes = 100000,
    # Also fetch the imagery tiles into the archive. Without it the run only measures.
    [switch]$Download,
    # Report without writing the evidence file.
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
$footprintRoot = Join-Path $SourceRoot 'map-regions/footprints'

$kuroStaticHost = 'web-static.kurobbs.com'
$kuroApiHost = 'api.kurobbs.com'

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

$generation = $recordedVersion
if ($ResolveCurrentVersion) { $generation = Get-CurrentKuroResourceVersion }
elseif ($ResourceVersion) {
    if ($ResourceVersion -notmatch '^[A-Fa-f0-9]{32}$') { throw "Resource version is not a 32-character hex value: $ResourceVersion" }
    $generation = $ResourceVersion.ToUpperInvariant()
}

$regionIds = @($RegionId -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
if ($regionIds.Count -eq 0) { throw 'Name at least one region id with -RegionId; measuring every region would probe the whole upstream tile space.' }
$known = @($registry['regions'] | ForEach-Object { [string]$_['id'] })
foreach ($id in $regionIds) { if ($known -notcontains $id) { throw "Unknown region id: $id" } }

Write-Host "Registry generation: $recordedVersion"
Write-Host "Probe generation:    $generation"
if ($generation -ne $recordedVersion) {
    Write-Warning "Probing generation $generation, not the registry generation $recordedVersion. Sync the registry first, or the measurement will be recorded as stale."
}

$written = 0
foreach ($id in $regionIds) {
    $record = $registry['regions'] | Where-Object { [string]$_['id'] -eq $id }
    $state = [int]$record['frame']
    $scene = [string]$record['scene']
    $search = $record['tileBounds']
    $basis = 'registry tile window'
    if ($null -eq $search) {
        $search = $record['untrustedPointWindow']
        $basis = 'point window under the unproven compiled origin'
    }
    if ($null -eq $search) { throw "Region $id has neither a tile window nor a point window; nothing to bound the search with." }

    $minX = [int]$search['minX'] - $SearchMargin
    $maxX = [int]$search['maxX'] + $SearchMargin
    $minY = [int]$search['minY'] - $SearchMargin
    $maxY = [int]$search['maxY'] + $SearchMargin
    $cells = ($maxX - $minX + 1) * ($maxY - $minY + 1)
    Write-Host ("Region {0,-16} state={1,-4} searching x {2}..{3} y {4}..{5} ({6} tiles, basis: {7})" -f $id, $state, $minX, $maxX, $minY, $maxY, $cells, $basis)

    $targets = [Collections.Generic.List[object]]::new()
    for ($x = $minX; $x -le $maxX; $x++) {
        for ($y = $minY; $y -le $maxY; $y++) {
            $name = "${state}_${x}_${y}.png"
            $targets.Add([pscustomobject]@{
                X = $x; Y = $y; Name = $name
                Url = "https://$kuroStaticHost/mcmap/tiles/$generation/$state/$name"
            })
        }
    }

    # Upstream answers 200 with a fully transparent placeholder over a rectangle that is
    # larger than the map, so "served" and "has imagery" are different questions and only
    # the byte count separates them. The parse is inlined because -Parallel runspaces do
    # not inherit the caller's functions.
    $probes = $targets | ForEach-Object -Parallel {
        $header = & curl.exe --silent --show-error --location --proto '=https' --tlsv1.2 `
            --connect-timeout 15 --max-time 60 --retry 3 --retry-delay 2 -o NUL -D - -I $_.Url 2>&1
        $code = 0
        $bytes = 0
        $etag = ''
        foreach ($line in @($header)) {
            if ($line -match '^HTTP/\S+\s+(\d{3})') { $code = [int]$Matches[1]; continue }
            if ($line -match '(?i)^content-length:\s*(\d+)') { $bytes = [int64]$Matches[1]; continue }
            if ($line -match '(?i)^etag:\s*"?([^"\r\n]+)"?') { $etag = $Matches[1].Trim(); continue }
        }
        [pscustomobject]@{ X = $_.X; Y = $_.Y; Name = $_.Name; Url = $_.Url; Code = $code; Bytes = $bytes; ETag = $etag }
    } -ThrottleLimit $ThrottleLimit

    $failed = @($probes | Where-Object { $_.Code -ne 200 -and $_.Code -ne 404 })
    if ($failed.Count -gt 0) {
        $failed | Select-Object -First 5 | ForEach-Object { Write-Warning "  $($_.Name): HTTP $($_.Code)" }
        throw "$($failed.Count) tile probes returned neither 200 nor 404; the generation or URL pattern is wrong."
    }
    $present = @($probes | Where-Object { $_.Code -eq 200 })
    if ($present.Count -eq 0) {
        throw "None of the $($probes.Count) probed tiles is served for state $state at generation $generation. The URL pattern, the generation or the search box is wrong; refusing to record an empty footprint."
    }

    $imagery = @($present | Where-Object { $_.Bytes -ge $ImageryMinBytes })
    $placeholder = @($present | Where-Object { $_.Bytes -lt $ImageryMinBytes })
    Write-Host ("  served: {0}  imagery: {1} (>= {2} bytes)  placeholder: {3}" -f $present.Count, $imagery.Count, $ImageryMinBytes, $placeholder.Count)
    $histogram = $present | Group-Object Bytes | Sort-Object { [int64]$_.Name } | Select-Object -Last 12
    Write-Host ("  size histogram (largest 12): " + (($histogram | ForEach-Object { "$($_.Name)x$($_.Count)" }) -join ' '))
    # A present tile sitting right next to the threshold means the classification is a
    # coin flip for it; say so instead of silently deciding.
    $ambiguous = @($present | Where-Object { $_.Bytes -ge ($ImageryMinBytes * 0.8) -and $_.Bytes -lt ($ImageryMinBytes * 1.25) })
    if ($ambiguous.Count -gt 0) {
        Write-Warning "  $($ambiguous.Count) tile(s) are within 25% of the threshold and could be classified either way; inspect -ImageryMinBytes against the histogram above."
    }
    # The one failure this probe cannot see is a map that extends past the search box.
    $onEdge = @($imagery | Where-Object { $_.X -eq $minX -or $_.X -eq $maxX -or $_.Y -eq $minY -or $_.Y -eq $maxY })
    if ($onEdge.Count -gt 0) {
        Write-Warning "  $($onEdge.Count) imagery tile(s) touch the edge of the searched box; widen -SearchMargin, the footprint may be larger than what was measured."
    }

    if ($imagery.Count -eq 0) {
        throw "State $state has no tile at or above $ImageryMinBytes bytes; lower -ImageryMinBytes only after checking the histogram, or the search box is in the wrong place."
    }
    $footMinX = ($imagery | ForEach-Object { $_.X } | Measure-Object -Minimum).Minimum
    $footMaxX = ($imagery | ForEach-Object { $_.X } | Measure-Object -Maximum).Maximum
    $footMinY = ($imagery | ForEach-Object { $_.Y } | Measure-Object -Minimum).Minimum
    $footMaxY = ($imagery | ForEach-Object { $_.Y } | Measure-Object -Maximum).Maximum
    Write-Host ("  footprint: x {0}..{1} y {2}..{3}  window cells {4}  imagery {5}" -f `
        $footMinX, $footMaxX, $footMinY, $footMaxY, (($footMaxX - $footMinX + 1) * ($footMaxY - $footMinY + 1)), $imagery.Count)

    $downloaded = [Collections.Generic.List[object]]::new()
    if ($Download) {
        $directory = Join-Path $ArchiveRoot "$generation/$state"
        [IO.Directory]::CreateDirectory($directory) | Out-Null
        $fetch = $imagery | ForEach-Object -Parallel {
            $destination = Join-Path $using:directory $_.Name
            $partial = $destination + '.part'
            if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Force }
            $output = & curl.exe --fail --silent --show-error --location --proto '=https' --tlsv1.2 `
                --connect-timeout 15 --max-time 120 --retry 3 --retry-delay 2 $_.Url --output $partial 2>&1
            if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $partial)) {
                if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Force }
                return [pscustomobject]@{ Name = $_.Name; Ok = $false; Detail = "exit=$LASTEXITCODE $output" }
            }
            Move-Item -LiteralPath $partial -Destination $destination -Force
            [pscustomobject]@{ Name = $_.Name; Ok = $true; Detail = '' }
        } -ThrottleLimit $ThrottleLimit
        $bad = @($fetch | Where-Object { -not $_.Ok })
        if ($bad.Count -gt 0) {
            $bad | Select-Object -First 5 | ForEach-Object { Write-Warning "  $($_.Name): $($_.Detail)" }
            throw "$($bad.Count) imagery tile(s) failed to download; the archive for state $state is incomplete."
        }
        foreach ($tile in ($imagery | Sort-Object X, Y)) {
            $path = Join-Path $directory $tile.Name
            $downloaded.Add([ordered]@{
                x = $tile.X; y = $tile.Y; file = $tile.Name; bytes = $tile.Bytes
                etag = $tile.ETag
                sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
            })
        }
        Write-Host "  archived $(@($downloaded).Count) imagery tile(s) under $directory" -ForegroundColor Green
    }

    $evidence = [ordered]@{
        formatVersion = 1
        region = $id
        scene = $scene
        state = $state
        generation = $generation
        measuredAtUtc = [DateTime]::UtcNow.ToString('o')
        probe = [ordered]@{
            method = 'http-head content-length'
            imageryMinBytes = $ImageryMinBytes
            searchBasis = $basis
            searched = [ordered]@{ minX = $minX; maxX = $maxX; minY = $minY; maxY = $maxY }
            probedTiles = $targets.Count
            servedTiles = $present.Count
            imageryTiles = $imagery.Count
            placeholderTiles = $placeholder.Count
        }
        bounds = [ordered]@{ minX = $footMinX; maxX = $footMaxX; minY = $footMinY; maxY = $footMaxY }
        tiles = @($probes | Sort-Object X, Y | ForEach-Object {
            [ordered]@{
                x = $_.X; y = $_.Y
                present = $_.Code -eq 200
                bytes = $_.Bytes
                imagery = $_.Code -eq 200 -and $_.Bytes -ge $ImageryMinBytes
            }
        })
    }
    if ($Download) { $evidence['archivedTiles'] = @($downloaded) }

    if ($Check) { Write-Host '  check only; no evidence file was written.'; continue }
    New-Item -ItemType Directory -Force -Path $footprintRoot | Out-Null
    $path = Join-Path $footprintRoot "$id.json"
    [IO.File]::WriteAllText($path, (($evidence | ConvertTo-Json -Depth 8) + [Environment]::NewLine), [Text.UTF8Encoding]::new($false))
    Write-Host "  wrote $path" -ForegroundColor Green
    $written++
}

Write-Host "Map tile footprint: regions=$($regionIds.Count) written=$written generation=$generation"
if ($written -gt 0) { Write-Host 'Re-run scripts/New-MapRegionRegistry.ps1 so the registry picks the measurement up.' }
