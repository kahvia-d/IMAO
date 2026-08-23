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
$routes = [ordered]@{
    '8'   = [ordered]@{ runtime = 'World';         supported = $true;  reason = '' }
    '900' = [ordered]@{ runtime = 'Tethys';        supported = $true;  reason = '' }
    '905' = [ordered]@{ runtime = 'Fabricatorium'; supported = $true;  reason = '' }
    '903' = [ordered]@{ runtime = 'Avinoleum';     supported = $true;  reason = '' }
    '906' = [ordered]@{ runtime = 'Lahai';         supported = $true;  reason = '' }
    '902' = [ordered]@{ runtime = '';              supported = $false; reason = 'Pending: no coordinate transform, map base, or SURF feature set.' }
    '909' = [ordered]@{ runtime = '';              supported = $false; reason = 'Pending: no coordinate transform, map base, or SURF feature set.' }
    '910' = [ordered]@{ runtime = '';              supported = $false; reason = 'Pending: no coordinate transform, map base, or SURF feature set.' }
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
    & curl.exe --fail --silent --show-error --location --proto '=https' --tlsv1.2 --connect-timeout 15 --max-time 45 $Url --output $Destination
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

    $iconManifest = [ordered]@{ formatVersion = 1; icons = [ordered]@{} }
    $iconIndex = 0
    foreach ($itemId in @($iconSources.Keys | Sort-Object)) {
        $relativeIcon = [string]$iconSources[$itemId]
        if ($relativeIcon.StartsWith('/')) { $relativeIcon = $relativeIcon.TrimStart('/') }
        $iconUrl = "https://$kuroStaticHost/$relativeIcon"
        Assert-KuroUri $iconUrl @($kuroStaticHost)
        $downloadedIcon = Join-Path $rawDir ("icon-$iconIndex.download")
        $asciiName = ('icon-{0:D4}.png' -f $iconIndex)
        Invoke-KuroDownload $iconUrl $downloadedIcon
        Copy-ValidatedPng $downloadedIcon (Join-Path $generatedDir "icons/$asciiName")
        $iconManifest.icons[$itemId] = "icons/$asciiName"
        $iconIndex++
        if ($iconIndex % 50 -eq 0) { Write-Host "Validated $iconIndex of $($iconSources.Count) icons." }
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
    $report += @('', 'Point synchronization does not make a new map runtime-ready. States 902, 909, and 910 are archived only until coordinate transforms, base maps, and SURF feature sets are added.')
    [IO.File]::WriteAllLines((Join-Path $generatedDir 'sync-report.md'), $report, [Text.UTF8Encoding]::new($false))

    $target = Join-Path $repoRoot 'Assets\KuroMap'
    $difference = Compare-GeneratedDirectory $generatedDir $target
    Write-Host "Kuro map $($PSCmdlet.ParameterSetName): resource=$resource files=$($difference.files) new=$($difference.new) changed=$($difference.changed) unchanged=$($difference.unchanged)"
    foreach ($state in $stateManifest) { Write-Host " state=$($state.state) types=$($state.itemTypes) points=$($state.points) supported=$($state.supported)" }

    if ($Apply) {
        New-Item -ItemType Directory -Force -Path $target | Out-Null
        Copy-Item -Path (Join-Path $generatedDir '*') -Destination $target -Recurse -Force
        foreach ($stateId in $routes.Keys) {
            if (-not $routes[$stateId].supported) { continue }
            $runtimeName = $routes[$stateId].runtime
            $runtimePath = Join-Path $repoRoot "IMao-Core\src\Resource\itemsData_$runtimeName.json"
            Write-Utf8Json $runtimeItems[$runtimeName] $runtimePath
        }
        Write-Host "Applied snapshots, icons, names, reports, and five runtime data files."
    }
}
finally {
    if (Test-Path -LiteralPath $tempRoot) { Remove-Item -LiteralPath $tempRoot -Recurse -Force }
}
