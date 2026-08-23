[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$root = Join-Path $repoRoot 'Assets\KuroMap'
$routes = [ordered]@{ '8' = 'World'; '900' = 'Tethys'; '905' = 'Fabricatorium'; '903' = 'Avinoleum'; '906' = 'Lahai'; '902' = ''; '909' = ''; '910' = '' }
$middleDot = [char]0x00B7
$aliases = @{ ("sx${middleDot}qq") = 'sx_qq'; ("sx${middleDot}lgn") = 'sx_lgn' }
function Normalize-Id([string]$Id) { if ($aliases.ContainsKey($Id)) { return $aliases[$Id] }; return $Id }
if (-not (Test-Path -LiteralPath $root)) { throw 'Assets/KuroMap is absent. Run Sync-KuroMapData.ps1 -Apply first.' }

$manifest = Get-Content -LiteralPath (Join-Path $root 'manifest.json') -Raw | ConvertFrom-Json
$icons = Get-Content -LiteralPath (Join-Path $root 'icon-manifest.json') -Raw | ConvertFrom-Json
$filters = Get-Content -LiteralPath (Join-Path $root 'filter-items.json') -Raw | ConvertFrom-Json
$filterIds = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($category in $filters.PSObject.Properties) {
    foreach ($item in $category.Value.PSObject.Properties) {
        if (-not $filterIds.Add($item.Name)) { throw "Duplicate filter item ID: $($item.Name)" }
    }
}
foreach ($icon in $icons.icons.PSObject.Properties) {
    $iconPath = Join-Path $root $icon.Value
    if (-not (Test-Path -LiteralPath $iconPath)) { throw "Missing icon manifest file: $($icon.Name) -> $($icon.Value)" }
}
$runtimeTypes = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($stateId in $routes.Keys) {
    $raw = Get-Content -LiteralPath (Join-Path $root "states\state-$stateId.json") -Raw | ConvertFrom-Json
    $pointIds = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($item in @($raw)) {
        if ([string]::IsNullOrWhiteSpace([string]$item.id)) { throw "State $stateId has blank item ID." }
        if ($null -eq $icons.icons.PSObject.Properties[(Normalize-Id ([string]$item.id))]) { throw "State $stateId item has no external icon: $($item.id)" }
        foreach ($point in @($item.location)) {
            if (-not $pointIds.Add([string]$point.id)) { throw "State $stateId duplicate point ID: $($point.id)" }
            if ($point.x -isnot [ValueType] -or $point.y -isnot [ValueType]) { throw "State $stateId non-numeric point coordinates." }
        }
    }
    if ($routes[$stateId]) {
        $runtime = Get-Content -LiteralPath (Join-Path $repoRoot "IMao-Core\src\Resource\itemsData_$($routes[$stateId]).json") -Raw | ConvertFrom-Json
        $expectedPoints = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
        foreach ($item in @($raw)) {
            $id = Normalize-Id ([string]$item.id)
            foreach ($point in @($item.location)) { $null = $expectedPoints.Add("$id|$($point.id)") }
        }
        $runtimePoints = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
        foreach ($item in @($runtime)) {
            $id = Normalize-Id ([string]$item.id)
            $null = $runtimeTypes.Add($id)
            foreach ($point in @($item.location)) { $null = $runtimePoints.Add("$id|$($point.id)") }
        }
        if ($runtimePoints.Count -eq 0 -or $runtimePoints.Count -ne $expectedPoints.Count -or @($expectedPoints | Where-Object { -not $runtimePoints.Contains($_) }).Count -ne 0) {
            throw "Runtime scene $($routes[$stateId]) does not exactly map the archived state $stateId points."
        }
    }
}
foreach ($filterId in $filterIds) {
    if (-not $runtimeTypes.Contains($filterId)) { throw "External filter item is not in a supported runtime state: $filterId" }
}
Write-Host "Kuro map offline validation passed: $($manifest.resourceVersion), $(@($icons.icons.PSObject.Properties).Count) icons, $($filterIds.Count) filter IDs."
