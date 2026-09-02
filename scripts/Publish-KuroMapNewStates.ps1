[CmdletBinding(DefaultParameterSetName = 'Check')]
param(
    [Parameter(ParameterSetName = 'Check')]
    [switch]$Check,
    [Parameter(Mandatory = $true, ParameterSetName = 'Apply')]
    [switch]$Apply,
    [switch]$ReplacePublished
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# This intentionally publishes only files that did not exist before this feature:
# three external runtime lists and one additive filter list.  It never overwrites
# the global Kuro snapshot, icon manifest, or existing scene resources.
$repoRoot = Split-Path -Parent $PSScriptRoot
$mapRoot = Join-Path $repoRoot 'Assets\KuroMap'
$routes = [ordered]@{
    '902' = [ordered]@{ scene = 'LowerVault';    itemTypes = 37; points = 238 }
    '909' = [ordered]@{ scene = 'Darkplain';     itemTypes = 43; points = 673 }
    '910' = [ordered]@{ scene = 'TimeRiftRuins'; itemTypes = 9;  points = 59 }
}
$middleDot = [char]0x00B7
$idAliases = @{ ("sx${middleDot}qq") = 'sx_qq'; ("sx${middleDot}lgn") = 'sx_lgn' }

function Normalize-ItemId([string]$Id) {
    if ($idAliases.ContainsKey($Id)) { return $idAliases[$Id] }
    return $Id
}

function Write-Utf8Json([object]$Value, [string]$Path) {
    $text = $Value | ConvertTo-Json -Depth 100
    [IO.File]::WriteAllText($Path, $text + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
}

function Get-OptionalProperty([object]$Object, [string]$Name) {
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Get-CatalogEntries([object]$Node, [string]$ParentName, [hashtable]$Entries) {
    foreach ($entry in @($Node)) {
        if ($null -eq $entry) { continue }
        $name = [string](Get-OptionalProperty $entry 'name')
        $id = [string](Get-OptionalProperty $entry 'id')
        $tableName = [string](Get-OptionalProperty $entry 'tableName')
        $category = if (-not [string]::IsNullOrWhiteSpace($tableName)) { $tableName } elseif ($ParentName) { $ParentName } else { 'Uncategorized' }
        if (-not [string]::IsNullOrWhiteSpace($id) -and -not [string]::IsNullOrWhiteSpace($name) -and -not $Entries.ContainsKey((Normalize-ItemId $id))) {
            $Entries[(Normalize-ItemId $id)] = [ordered]@{ name = $name; category = $category }
        }
        $children = Get-OptionalProperty $entry 'children'
        if ($null -ne $children) { Get-CatalogEntries $children $name $Entries }
    }
}

function Convert-PositionsForRuntime([object[]]$Items, [string]$StateId) {
    $pointIds = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $result = [Collections.Generic.List[object]]::new()
    foreach ($item in $Items) {
        if ($null -eq $item -or [string]::IsNullOrWhiteSpace([string]$item.id) -or $null -eq $item.location) {
            throw "State $StateId contains an item without id or location."
        }
        $itemCopy = [ordered]@{}
        foreach ($property in $item.PSObject.Properties) { $itemCopy[$property.Name] = $property.Value }
        $itemCopy.id = Normalize-ItemId ([string]$item.id)
        $locations = [Collections.Generic.List[object]]::new()
        foreach ($location in @($item.location)) {
            if ($null -eq $location -or [string]::IsNullOrWhiteSpace([string]$location.id)) {
                throw "State $StateId contains a point without id."
            }
            if (-not $pointIds.Add([string]$location.id)) { throw "State $StateId contains duplicate point id $($location.id)." }
            if ($location.x -isnot [ValueType] -or $location.y -isnot [ValueType]) {
                throw "State $StateId point $($location.id) has non-numeric coordinates."
            }
            $locationCopy = [ordered]@{}
            foreach ($property in $location.PSObject.Properties) { $locationCopy[$property.Name] = $property.Value }
            if ($locationCopy.Contains('typeId')) { $locationCopy.typeId = Normalize-ItemId ([string]$locationCopy.typeId) }
            $locations.Add([pscustomobject]$locationCopy)
        }
        $itemCopy.location = @($locations)
        $result.Add([pscustomobject]$itemCopy)
    }
    return @($result)
}

$runtimeData = [ordered]@{}
$catalogEntries = @{}
$allRuntimeIds = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$itemScenes = [ordered]@{}
foreach ($stateId in $routes.Keys) {
    $statePath = Join-Path $mapRoot "states\state-$stateId.json"
    $catalogPath = Join-Path $mapRoot "catalogs\catalog-$stateId.json"
    if (-not (Test-Path -LiteralPath $statePath) -or -not (Test-Path -LiteralPath $catalogPath)) {
        throw "State $stateId is not archived locally; run the full synchronized snapshot before publishing it."
    }
    $normalized = Convert-PositionsForRuntime @((Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json)) $stateId
    $pointCount = @($normalized | ForEach-Object { @($_.location).Count } | Measure-Object -Sum).Sum
    if ($normalized.Count -ne $routes[$stateId].itemTypes -or [int]$pointCount -ne $routes[$stateId].points) {
        throw "State $stateId count changed: expected $($routes[$stateId].itemTypes)/$($routes[$stateId].points), got $($normalized.Count)/$pointCount. Refresh and review the snapshot first."
    }
    foreach ($item in $normalized) {
        $itemId = [string]$item.id
        $null = $allRuntimeIds.Add($itemId)
        if (-not $itemScenes.Contains($itemId)) { $itemScenes[$itemId] = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal) }
        $null = $itemScenes[$itemId].Add([string]$routes[$stateId].scene)
    }
    Get-CatalogEntries @((Get-Content -LiteralPath $catalogPath -Raw | ConvertFrom-Json)) '' $catalogEntries
    $runtimeData[$routes[$stateId].scene] = $normalized
    Write-Host "Validated state $stateId ($($routes[$stateId].scene)): $($normalized.Count) item types, $pointCount points."
}

$filterItems = [ordered]@{}
foreach ($id in @($allRuntimeIds | Sort-Object)) {
    $entry = if ($catalogEntries.ContainsKey($id)) { $catalogEntries[$id] } else { $null }
    $category = if ($null -ne $entry) { "Kuro Map New States - $($entry.category)" } else { 'Kuro Map New States - Uncategorized' }
    $name = if ($null -ne $entry) { $entry.name } else { $id }
    if (-not $filterItems.Contains($category)) { $filterItems[$category] = [ordered]@{} }
    $filterItems[$category][$id] = [ordered]@{ 'zh-CN' = $name; 'en-US' = $id }
}

$runtimeDirectory = Join-Path $mapRoot 'runtime'
$filterPath = Join-Path $mapRoot 'new-state-filter-items.json'
$sceneMapPath = Join-Path $mapRoot 'new-state-item-scenes.json'
$sceneMap = [ordered]@{ formatVersion = 1; items = [ordered]@{} }
foreach ($itemId in @($itemScenes.Keys | Sort-Object)) { $sceneMap.items[$itemId] = @($itemScenes[$itemId] | Sort-Object) }
$targets = @($filterPath, $sceneMapPath)
foreach ($scene in $runtimeData.Keys) { $targets += Join-Path $runtimeDirectory "itemsData_$scene.json" }
foreach ($target in $targets) {
    if ($Apply -and (Test-Path -LiteralPath $target) -and -not $ReplacePublished) {
        throw "Refusing to overwrite existing publication file: $target"
    }
}

Write-Host "New-state publication $($PSCmdlet.ParameterSetName): files=$($targets.Count) filters=$($allRuntimeIds.Count)"
if ($Apply) {
    New-Item -ItemType Directory -Force -Path $runtimeDirectory | Out-Null
    foreach ($scene in $runtimeData.Keys) {
        Write-Utf8Json $runtimeData[$scene] (Join-Path $runtimeDirectory "itemsData_$scene.json")
    }
    Write-Utf8Json $filterItems $filterPath
    Write-Utf8Json $sceneMap $sceneMapPath
    Write-Host 'Published three additive runtime lists, one filter list, and one scene mapping.' -ForegroundColor Green
}
