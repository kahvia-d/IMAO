# Point synchronization supplies runtime point data. New scenes remain release-gated until their
# independently verified Kuro tile feature pack and game validation evidence are present.
#
# This script writes Assets\KuroMap only. Two other things carry the same data and are NOT written here, so a
# points refresh that stops after -Apply leaves the program serving the old set (2026-10-02):
#   * Assets\KuroMapIcons does not exist in the repository. Staging splits the icons out of KuroMap into a
#     separate package (Stage-UpdateResources.ps1), and the packaged copy under x64\Release\Assets - which
#     out\map-test links to - keeps the old icon-manifest.json and icon files until staging runs again. A new
#     point category then has no icon reference, and the native validator rejects the whole snapshot with
#     "point category missing icon reference", failing Test-ResourceUpdates' region-selection check.
#   * Publish-KuroMapNewStates.ps1 refuses to publish a gated scene whose count no longer matches its own
#     expected table, and Test-KuroMapNewStates.ps1 keeps a second copy of that table. Both are deliberate
#     review gates: update them by hand after looking at what upstream changed.
[CmdletBinding(DefaultParameterSetName = 'Check')]
param(
    [Parameter(ParameterSetName = 'Check')]
    [switch]$Check,
    [Parameter(Mandatory = $true, ParameterSetName = 'Apply')]
    [switch]$Apply
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$kuroApiHost = 'api.kurobbs.com'
$kuroStaticHost = 'web-static.kurobbs.com'
$stagedReason = '运行时点位由 Publish-KuroMapNewStates.ps1 单独发布；本表只归档上游快照。'
$routes = [ordered]@{
    '8'   = [ordered]@{ runtime = 'World';         supported = $true;  reason = '' }
    '900' = [ordered]@{ runtime = 'Tethys';        supported = $true;  reason = '' }
    '905' = [ordered]@{ runtime = 'Fabricatorium'; supported = $true;  reason = '' }
    '903' = [ordered]@{ runtime = 'Avinoleum';     supported = $true;  reason = '' }
    '906' = [ordered]@{ runtime = 'Lahai';         supported = $true;  reason = '' }
    # The four scenes below publish their runtime point lists through
    # scripts/Publish-KuroMapNewStates.ps1 instead of this snapshot.  Keeping them
    # out of the supported set is what keeps their item ids in the *gated*
    # new-state-filter-items.json table: an id that reaches filter-items.json is
    # shown in the filter page regardless of scene approval, so an unreleased
    # region would appear there before its pack and calibration exist.
    '902' = [ordered]@{ runtime = 'LowerVault';    supported = $false; reason = $stagedReason }
    '909' = [ordered]@{ runtime = 'Darkplain';     supported = $false; reason = $stagedReason }
    '910' = [ordered]@{ runtime = 'TimeRiftRuins'; supported = $false; reason = $stagedReason }
    '912' = [ordered]@{ runtime = 'MengshuTianluo'; supported = $false; reason = $stagedReason }
}
$middleDot = [char]0x00B7
$idAliases = @{ ("sx${middleDot}qq") = 'sx_qq'; ("sx${middleDot}lgn") = 'sx_lgn' }

function Assert-KuroUri([string]$Url, [string[]]$AllowedHosts) {
    $uri = [Uri]$Url
    if ($uri.Scheme -ne 'https' -or $AllowedHosts -notcontains $uri.Host) {
        throw "Refusing non-public Kuro map URI: $Url"
    }
}

function Invoke-KuroDownload([string]$Url, [string]$Destination) {
    Assert-KuroUri $Url @($kuroStaticHost)
    # --retry covers the transport failures only (timeouts, resets, 5xx). A 404 is a
    # real answer here and must still fail the run, which --retry-all-errors would hide.
    # Schannel handshake failures (35) are not included in curl's normal retry
    # policy. Retry only that transport error; a genuine HTTP error stays fatal.
    for ($handshakeAttempt = 0; $handshakeAttempt -lt 5; $handshakeAttempt++) {
        & curl.exe --fail --silent --show-error --location --proto '=https' --tlsv1.2 --connect-timeout 15 --max-time 45 --retry 3 --retry-delay 2 $Url --output $Destination
        if ($LASTEXITCODE -ne 35) { break }
        if ($handshakeAttempt -lt 4) { Start-Sleep -Seconds 2 }
    }
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $Destination)) {
        throw "Download failed: $Url"
    }
}

function Get-KuroResourceVersion([string]$Destination) {
    $url = "https://$kuroApiHost/map/core/config/getMapResource"
    Assert-KuroUri $url @($kuroApiHost)
    & curl.exe --fail --silent --show-error --location --proto '=https' --tlsv1.2 --connect-timeout 15 --max-time 45 -X POST $url -H 'content-type: application/json' -d '{}' --output $Destination
    if ($LASTEXITCODE -ne 0) { throw "Kuro map resource request failed." }
    $response = Get-Content -LiteralPath $Destination -Raw | ConvertFrom-Json
    if ($response.code -ne 200 -or [string]::IsNullOrWhiteSpace([string]$response.data)) {
        throw 'Kuro map resource response was invalid.'
    }
    return [string]$response.data
}

function Get-KuroStateSelection([string]$Destination) {
    $url = "https://$kuroApiHost/map/core/position/getMapStateSelection"
    Assert-KuroUri $url @($kuroApiHost)
    & curl.exe --fail --silent --show-error --location --proto '=https' --tlsv1.2 --connect-timeout 15 --max-time 45 $url --output $Destination
    if ($LASTEXITCODE -ne 0) { throw 'Kuro map state-selection request failed.' }
    $response = Get-Content -LiteralPath $Destination -Raw | ConvertFrom-Json
    if ($response.code -ne 200 -or $null -eq $response.data -or $null -eq $response.data.state) {
        throw 'Kuro map state-selection response was invalid.'
    }
    return @($response.data.state | ForEach-Object { [string]$_.id })
}

function Normalize-ItemId([string]$Id) {
    if ($idAliases.ContainsKey($Id)) { return $idAliases[$Id] }
    return $Id
}

function Get-OptionalProperty([object]$Object, [string]$Name) {
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Write-Utf8Json([object]$Value, [string]$Path) {
    $parent = Split-Path -Parent $Path
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    $text = $Value | ConvertTo-Json -Depth 100
    [IO.File]::WriteAllText($Path, $text + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
}

function Copy-ValidatedPng([string]$Source, [string]$Destination) {
    $bytes = [IO.File]::ReadAllBytes($Source)
    if ($bytes.Length -lt 8 -or $bytes[0] -ne 0x89 -or $bytes[1] -ne 0x50 -or $bytes[2] -ne 0x4E -or $bytes[3] -ne 0x47 -or $bytes[4] -ne 0x0D -or $bytes[5] -ne 0x0A -or $bytes[6] -ne 0x1A -or $bytes[7] -ne 0x0A) {
        throw "Icon is not a PNG: $Source"
    }
    if ($bytes.Length -gt 5MB) { throw "Icon exceeds 5 MB: $Source" }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

function Get-CatalogEntries([object]$Node, [string]$ParentName, [hashtable]$Entries) {
    foreach ($entry in @($Node)) {
        if ($null -eq $entry) { continue }
        $name = [string](Get-OptionalProperty $entry 'name')
        $id = [string](Get-OptionalProperty $entry 'id')
        $tableName = [string](Get-OptionalProperty $entry 'tableName')
        $category = if (-not [string]::IsNullOrWhiteSpace($tableName)) { $tableName } elseif ($ParentName) { $ParentName } else { 'Uncategorized' }
        if (-not [string]::IsNullOrWhiteSpace($id) -and -not [string]::IsNullOrWhiteSpace($name) -and -not $Entries.ContainsKey((Normalize-ItemId $id))) {
            $Entries[(Normalize-ItemId $id)] = [ordered]@{ name = $name; category = $category; icon = [string](Get-OptionalProperty $entry 'icon') }
        }
        $children = Get-OptionalProperty $entry 'children'
        if ($null -ne $children) {
            Get-CatalogEntries $children $name $Entries
        }
    }
}

function Convert-PositionsForRuntime([object[]]$Items, [string]$StateId) {
    $ids = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $result = [Collections.Generic.List[object]]::new()
    foreach ($item in $Items) {
        if ($null -eq $item -or [string]::IsNullOrWhiteSpace([string]$item.id) -or $null -eq $item.location) {
            throw "State $StateId contains an item without id or location."
        }
        $copy = [ordered]@{}
        foreach ($property in $item.PSObject.Properties) { $copy[$property.Name] = $property.Value }
        $copy.id = Normalize-ItemId ([string]$item.id)
        $locations = [Collections.Generic.List[object]]::new()
        foreach ($location in @($item.location)) {
            if ($null -eq $location -or [string]::IsNullOrWhiteSpace([string]$location.id)) { throw "State $StateId contains a point without id." }
            if (-not $ids.Add([string]$location.id)) { throw "State $StateId contains duplicate point id $($location.id)." }
            if ($location.x -isnot [ValueType] -or $location.y -isnot [ValueType]) { throw "State $StateId point $($location.id) has non-numeric coordinates." }
            $locationCopy = [ordered]@{}
            foreach ($property in $location.PSObject.Properties) { $locationCopy[$property.Name] = $property.Value }
            if ($locationCopy.Contains('typeId')) { $locationCopy.typeId = Normalize-ItemId ([string]$locationCopy.typeId) }
            $locations.Add([pscustomobject]$locationCopy)
        }
        $copy.location = @($locations)
        $result.Add([pscustomobject]$copy)
    }
    return @($result)
}

function Compare-GeneratedDirectory([string]$Generated, [string]$Current) {
    $generatedFiles = @(Get-ChildItem -LiteralPath $Generated -Recurse -File)
    $new = 0; $changed = 0; $same = 0
    foreach ($file in $generatedFiles) {
        $relative = $file.FullName.Substring($Generated.Length).TrimStart('\')
        $currentFile = Join-Path $Current $relative
        if (-not (Test-Path -LiteralPath $currentFile)) { $new++; continue }
        if ((Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash -eq (Get-FileHash -LiteralPath $currentFile -Algorithm SHA256).Hash) { $same++ } else { $changed++ }
    }
    [pscustomobject]@{ files = $generatedFiles.Count; new = $new; changed = $changed; unchanged = $same }
}

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ("imao-kuromap-" + [guid]::NewGuid().ToString('N'))
try {
    New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null
    $rawDir = Join-Path $tempRoot 'raw'
    $generatedDir = Join-Path $tempRoot 'generated'
    New-Item -ItemType Directory -Force -Path $rawDir, $generatedDir | Out-Null

    $resource = Get-KuroResourceVersion (Join-Path $rawDir 'resource.json')
    $publishedStates = Get-KuroStateSelection (Join-Path $rawDir 'state-selection.json')
    $missingStates = @($routes.Keys | Where-Object { $publishedStates -notcontains $_ })
    if ($missingStates.Count -gt 0) { throw "Kuro map state selection is missing expected states: $($missingStates -join ', ')." }

    $countryRaw = Join-Path $rawDir 'country.json'
    Invoke-KuroDownload "https://$kuroStaticHost/mcmap/country/$resource/country.json" $countryRaw
    $null = Get-Content -LiteralPath $countryRaw -Raw | ConvertFrom-Json
    New-Item -ItemType Directory -Force -Path (Join-Path $generatedDir 'states'), (Join-Path $generatedDir 'catalogs'), (Join-Path $generatedDir 'icons') | Out-Null
    Copy-Item -LiteralPath $countryRaw -Destination (Join-Path $generatedDir 'country.json') -Force

    $catalogEntries = @{}
    $runtimeItems = @{}
    $iconSources = @{}
    $stateManifest = [Collections.Generic.List[object]]::new()
    foreach ($stateId in $routes.Keys) {
        $positionRaw = Join-Path $rawDir "position-$stateId.json"
        $catalogRaw = Join-Path $rawDir "catalog-$stateId.json"
        Invoke-KuroDownload "https://$kuroStaticHost/mcmap/position/$stateId/position.json" $positionRaw
        Invoke-KuroDownload "https://$kuroStaticHost/mcmap/catalog/$resource/$stateId/catalog.json" $catalogRaw
        $positions = @(Get-Content -LiteralPath $positionRaw -Raw | ConvertFrom-Json)
        $catalog = @(Get-Content -LiteralPath $catalogRaw -Raw | ConvertFrom-Json)
        if ($positions.Count -eq 0) { throw "State $stateId has no position records." }
        Get-CatalogEntries $catalog '' $catalogEntries
        $normalized = Convert-PositionsForRuntime $positions $stateId
        Copy-Item -LiteralPath $positionRaw -Destination (Join-Path $generatedDir "states/state-$stateId.json") -Force
        Copy-Item -LiteralPath $catalogRaw -Destination (Join-Path $generatedDir "catalogs/catalog-$stateId.json") -Force
        if ($routes[$stateId].supported) { $runtimeItems[$routes[$stateId].runtime] = $normalized }
        foreach ($item in $normalized) {
            if (-not [string]::IsNullOrWhiteSpace([string]$item.icon) -and -not $iconSources.ContainsKey([string]$item.id)) {
                $iconSources[[string]$item.id] = [string]$item.icon
            }
        }
        $pointCount = @($normalized | ForEach-Object { @($_.location).Count } | Measure-Object -Sum).Sum
        $stateManifest.Add([ordered]@{
            state = [int]$stateId; runtime = $routes[$stateId].runtime; supported = $routes[$stateId].supported; reason = $routes[$stateId].reason
            itemTypes = $normalized.Count; points = [int]$pointCount; sha256 = (Get-FileHash -LiteralPath $positionRaw -Algorithm SHA256).Hash.ToLowerInvariant()
        })
        Write-Host "Validated state ${stateId}: $($normalized.Count) item types, $pointCount points."
    }

    # Icon file names are stable per item id.  Assigning them by position in the
    # sorted id list renumbers the whole set as soon as one id is inserted -- the
    # 梦枢天罗 sync rewrote 469 of 527 files although only 10 images were new.
    # Every id that already had a name keeps it, and only then do unseen ids take
    # the lowest index nobody holds.  Reserving the survivors first is what makes
    # this work: claiming names while walking the list in id order hands a new id
    # the name of a later id and shifts everything after it.
    $existingIconNames = @{}
    $existingIconManifestPath = Join-Path $repoRoot 'Assets\KuroMap\icon-manifest.json'
    if (Test-Path -LiteralPath $existingIconManifestPath) {
        $existingIconManifest = Get-Content -LiteralPath $existingIconManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($null -ne $existingIconManifest.PSObject.Properties['icons']) {
            foreach ($entry in $existingIconManifest.icons.PSObject.Properties) { $existingIconNames[$entry.Name] = [string]$entry.Value }
        }
    }

    $sortedItemIds = @($iconSources.Keys | Sort-Object)
    $iconNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $iconNameById = @{}
    foreach ($itemId in $sortedItemIds) {
        if (-not $existingIconNames.ContainsKey($itemId)) { continue }
        $candidate = $existingIconNames[$itemId]
        if ($candidate -match '^icons/icon-\d{4}\.png$' -and $iconNames.Add($candidate)) { $iconNameById[$itemId] = $candidate }
    }
    $iconIndex = 0
    foreach ($itemId in $sortedItemIds) {
        if ($iconNameById.ContainsKey($itemId)) { continue }
        while ($iconNames.Contains('icons/icon-{0:D4}.png' -f $iconIndex)) { ++$iconIndex }
        $assigned = 'icons/icon-{0:D4}.png' -f $iconIndex
        [void]$iconNames.Add($assigned)
        $iconNameById[$itemId] = $assigned
    }

    $iconManifest = [ordered]@{ formatVersion = 1; icons = [ordered]@{} }
    $iconDownloadIndex = 0
    foreach ($itemId in $sortedItemIds) {
        $relativeIcon = [string]$iconSources[$itemId]
        if ($relativeIcon.StartsWith('/')) { $relativeIcon = $relativeIcon.TrimStart('/') }
        $iconUrl = "https://$kuroStaticHost/$relativeIcon"
        Assert-KuroUri $iconUrl @($kuroStaticHost)
        $asciiName = $iconNameById[$itemId]
        $downloadedIcon = Join-Path $rawDir ("icon-$iconDownloadIndex.download")
        Invoke-KuroDownload $iconUrl $downloadedIcon
        Copy-ValidatedPng $downloadedIcon (Join-Path $generatedDir $asciiName)
        $iconManifest.icons[$itemId] = $asciiName
        $iconDownloadIndex++
        if ($iconDownloadIndex % 50 -eq 0) { Write-Host "Validated $iconDownloadIndex of $($iconSources.Count) icons." }
    }
    Write-Utf8Json $iconManifest (Join-Path $generatedDir 'icon-manifest.json')

    $filterItems = [ordered]@{}
    $runtimeTypeIds = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($scene in $runtimeItems.Keys) {
        foreach ($item in @($runtimeItems[$scene])) { $null = $runtimeTypeIds.Add([string]$item.id) }
    }
    foreach ($id in @($runtimeTypeIds | Sort-Object)) {
        $entry = if ($catalogEntries.ContainsKey($id)) { $catalogEntries[$id] } else { $null }
        $category = if ($null -ne $entry) { "Kuro Map Sync - $($entry.category)" } else { 'Kuro Map Sync - Uncategorized' }
        $name = if ($null -ne $entry) { $entry.name } else { $id }
        if (-not $filterItems.Contains($category)) { $filterItems[$category] = [ordered]@{} }
        $filterItems[$category][$id] = [ordered]@{ 'zh-CN' = $name; 'en-US' = $id }
    }
    Write-Utf8Json $filterItems (Join-Path $generatedDir 'filter-items.json')

    $manifest = [ordered]@{
        formatVersion = 1; generatedAtUtc = [DateTime]::UtcNow.ToString('o'); resourceVersion = $resource
        source = [ordered]@{ api = "https://$kuroApiHost"; static = "https://$kuroStaticHost"; states = @($publishedStates) }
        aliases = $idAliases; states = @($stateManifest); iconCount = $iconIndex; runtimeFilterItemCount = $runtimeTypeIds.Count
    }
    Write-Utf8Json $manifest (Join-Path $generatedDir 'manifest.json')

    $report = @(
        '# Kuro Map sync report', '', "- Resource version: $resource", "- Generated (UTC): $($manifest.generatedAtUtc)", "- Icons: $iconIndex PNG files with ASCII names", "- Runtime filter item IDs: $($runtimeTypeIds.Count)", '',
        '| state | runtime scene | item types | points | status |', '| --- | --- | ---: | ---: | --- |'
    )
    foreach ($state in $stateManifest) {
        $status = if ($state.supported) { 'Integrated' } else { $state.reason }
        $report += "| $($state.state) | $($state.runtime) | $($state.itemTypes) | $($state.points) | $status |"
    }
    $report += @('', 'Point synchronization supplies runtime point data. New scenes remain release-gated until their independently verified Kuro tile feature pack and game validation evidence are present.')
    [IO.File]::WriteAllLines((Join-Path $generatedDir 'sync-report.md'), $report, [Text.UTF8Encoding]::new($false))

    $target = Join-Path $repoRoot 'Assets\KuroMap'
    $difference = Compare-GeneratedDirectory $generatedDir $target
    Write-Host "Kuro map $($PSCmdlet.ParameterSetName): resource=$resource files=$($difference.files) new=$($difference.new) changed=$($difference.changed) unchanged=$($difference.unchanged)"
    foreach ($state in $stateManifest) { Write-Host " state=$($state.state) types=$($state.itemTypes) points=$($state.points) supported=$($state.supported)" }

    if ($Apply) {
        New-Item -ItemType Directory -Force -Path $target | Out-Null
        Copy-Item -Path (Join-Path $generatedDir '*') -Destination $target -Recurse -Force
        # Stable icon names mean an id that disappeared upstream leaves its file
        # behind, and the copy above never deletes. The icons directory is generated
        # content whose only reference is the manifest, so anything the fresh
        # manifest does not name is removed -- otherwise dead PNGs keep being staged
        # into the downloadable resource package.
        $iconDirectory = Join-Path $target 'icons'
        if (Test-Path -LiteralPath $iconDirectory -PathType Container) {
            $orphans = [Collections.Generic.List[string]]::new()
            foreach ($file in @(Get-ChildItem -LiteralPath $iconDirectory -Filter 'icon-*.png' -File)) {
                if (-not $iconNames.Contains("icons/$($file.Name)")) { $orphans.Add($file.FullName) }
            }
            foreach ($orphan in $orphans) { Remove-Item -LiteralPath $orphan -Force }
            if ($orphans.Count -gt 0) { Write-Host "Removed $($orphans.Count) icon file(s) no id references any more." }
        }
        foreach ($stateId in $routes.Keys) {
            if (-not $routes[$stateId].supported) { continue }
            $runtimeName = $routes[$stateId].runtime
            if ($runtimeName -in @('LowerVault', 'Darkplain', 'TimeRiftRuins', 'MengshuTianluo')) {
                # New scenes stay as external staged resources.  IMao-Core.rc
                # cannot be safely changed by the sync process, while the
                # runtime loader verifies this explicit publication path.
                Write-Utf8Json $runtimeItems[$runtimeName] (Join-Path $target "runtime/itemsData_$runtimeName.json")
            }
            else {
                $runtimePath = Join-Path $repoRoot "IMao-Core\src\Resource\itemsData_$runtimeName.json"
                Write-Utf8Json $runtimeItems[$runtimeName] $runtimePath
            }
        }
        Write-Host 'Applied Kuro map snapshots, icons, names and supported runtime point files.'
    }
}
finally {
    if (Test-Path -LiteralPath $tempRoot) { Remove-Item -LiteralPath $tempRoot -Recurse -Force }
}
