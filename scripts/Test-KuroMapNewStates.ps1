[CmdletBinding()]
param(
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [switch]$RequireRelease
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$mapRoot = Join-Path $RepoRoot 'Assets\KuroMap'
$expected = [ordered]@{
    LowerVault = [ordered]@{ state = 902; types = 37; points = 238 }
    Darkplain = [ordered]@{ state = 909; types = 43; points = 673 }
    TimeRiftRuins = [ordered]@{ state = 910; types = 9; points = 59 }
}

function Read-Json([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { throw "Missing required file: $Path" }
    return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
}

function Get-PointCount([object[]]$Items) {
    return [int](@($Items | ForEach-Object { @($_.location).Count } | Measure-Object -Sum).Sum)
}

$packRegistry = Read-Json (Join-Path $RepoRoot 'Assets\FeaturesDatas\kuro-tile-packs.json')
if ([int]$packRegistry.formatVersion -ne 1) { throw 'Kuro tile pack registry format is invalid.' }
$registeredPacks = @($packRegistry.packs)
foreach ($scene in $expected.Keys) {
    if ($registeredPacks -notcontains $scene) { throw "Tile pack registry does not contain $scene." }
}

$validation = Read-Json (Join-Path $mapRoot 'scene-validation.json')
$sceneMap = Read-Json (Join-Path $mapRoot 'new-state-item-scenes.json')
$filterItems = Read-Json (Join-Path $mapRoot 'new-state-filter-items.json')
if ([int]$sceneMap.formatVersion -ne 1 -or $null -eq $sceneMap.items) { throw 'New-state item scene mapping is invalid.' }

$totalTypes = 0
$totalPoints = 0
$allIds = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($scene in $expected.Keys) {
    $definition = $expected[$scene]
    $raw = @(Read-Json (Join-Path $mapRoot "states\state-$($definition.state).json"))
    $runtime = @(Read-Json (Join-Path $mapRoot "runtime\itemsData_$scene.json"))
    if ($raw.Count -ne $definition.types -or (Get-PointCount $raw) -ne $definition.points) {
        throw "Archived state $($definition.state) count is not $($definition.types)/$($definition.points)."
    }
    if ($runtime.Count -ne $definition.types -or (Get-PointCount $runtime) -ne $definition.points) {
        throw "Runtime scene $scene count is not $($definition.types)/$($definition.points)."
    }
    foreach ($item in $runtime) {
        if ([string]::IsNullOrWhiteSpace([string]$item.id) -or $null -eq $item.location) { throw "Runtime scene $scene has an invalid item." }
        $null = $allIds.Add([string]$item.id)
        foreach ($location in @($item.location)) {
            if ([int]$location.stateId -ne $definition.state -or $null -eq $location.PSObject.Properties['floorId'] -or $null -eq $location.PSObject.Properties['level']) {
                throw "Runtime scene $scene did not preserve stateId, floorId, or level."
            }
        }
    }
    $approval = $validation.scenes.$scene
    if ($null -eq $approval -or $null -eq $approval.PSObject.Properties['approved']) { throw "Release gate is missing for $scene." }
    if ($RequireRelease -and -not [bool]$approval.approved) { throw "Scene $scene is not release-approved." }
    Write-Host "New scene valid: $scene state=$($definition.state) types=$($runtime.Count) points=$(Get-PointCount $runtime) approved=$($approval.approved)"
}

foreach ($id in $allIds) {
    if ($null -eq $sceneMap.items.PSObject.Properties[$id]) { throw "New-state item scene mapping is missing $id." }
}

$filterIds = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($category in $filterItems.PSObject.Properties) {
    foreach ($item in $category.Value.PSObject.Properties) { $null = $filterIds.Add($item.Name) }
}
foreach ($id in $allIds) {
    if (-not $filterIds.Contains($id)) { throw "New-state filter list is missing $id." }
}

Write-Host "New Kuro states passed: scenes=3 types=$($expected.Values | ForEach-Object { $_.types } | Measure-Object -Sum | Select-Object -ExpandProperty Sum) points=$($expected.Values | ForEach-Object { $_.points } | Measure-Object -Sum | Select-Object -ExpandProperty Sum) itemIds=$($allIds.Count)" -ForegroundColor Green
