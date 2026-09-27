[CmdletBinding()]
param(
    [string]$MapDataRoot = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# The daily-refresh classification is derived at runtime from the catalog the filter page
# groups by, so nothing here generates a file. What this script does is prove the derivation
# is safe to use as a switch: the two categories the farming mode is allowed to mark are the
# ones we think they are, every category the shipped point data actually uses resolves to one
# of them or to a deliberate "no", and the flat point-id set the C++ builds can stand in for a
# per-point category.
#
# See Docs/FarmMode_20260927.md sections 2.1, 4 and 8.

$taskRepo = Split-Path -Parent $PSScriptRoot
$taskMapRoot = if ($MapDataRoot) { $MapDataRoot } else { Join-Path $taskRepo 'Assets\KuroMap' }
$taskCatalogDir = Join-Path $taskMapRoot 'catalogs'
# Point snapshots ship from two roots: the five original scenes are compiled into the core as
# Windows resources, and the newer regions arrive as files inside the map-data package. The
# runtime reads both, so this check has to as well.
$taskPointRoots = @(
    (Join-Path $taskRepo 'IMao-Core\src\Resource'),
    (Join-Path $taskMapRoot 'runtime')
)
$taskFailures = [Collections.Generic.List[string]]::new()
$taskChecks = 0

function Assert-Refreshable {
    param([bool]$Condition, [string]$Message)
    $script:taskChecks++
    if ($Condition) { Write-Host "PASS $Message" } else { Write-Host "FAIL $Message"; $script:taskFailures.Add($Message) }
}
function Write-Info { param([string]$Message) Write-Host "INFO $Message" }

if (-not (Test-Path -LiteralPath $taskCatalogDir -PathType Container)) {
    throw "Category catalogs are missing: $taskCatalogDir"
}

# The same rewrite the C++ and the managed filter catalog apply. The catalog spells two 声匣
# ids with a middle dot while the point data uses an underscore; a category table that does not
# fold them would classify those points as unknown, and "unknown" must never mean "mark it".
function ConvertTo-PointCategoryId {
    param([string]$Id)
    if (-not $Id.StartsWith('sx', [StringComparison]::Ordinal)) { return $Id }
    return $Id.Replace([string][char]0x00B7, '_')
}

$taskFiles = @(Get-ChildItem -LiteralPath $taskCatalogDir -Filter 'catalog-*.json' | Sort-Object Name)
Assert-Refreshable ($taskFiles.Count -gt 0) "category catalogs found: $($taskFiles.Count) file(s)"

# Top-level category name -> set of leaf ids, unioned across every region catalog. A region that
# lists no 敌人 group (TimeRiftRuins does not) must not remove the ones the others supply.
$taskByCategory = @{}
$taskCatalogTopNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$taskMiddleDotIds = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($taskFile in $taskFiles) {
    $taskDocument = Get-Content -LiteralPath $taskFile.FullName -Raw | ConvertFrom-Json
    foreach ($taskCategory in $taskDocument) {
        [void]$taskCatalogTopNames.Add([string]$taskCategory.name)
        $taskName = [string]$taskCategory.name
        $taskLeaves = @($taskCategory.children)
        if ($taskLeaves.Count -eq 0) { continue }
        if (-not $taskByCategory.ContainsKey($taskName)) {
            $taskByCategory[$taskName] = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
        }
        foreach ($taskLeaf in $taskLeaves) {
            $taskRawId = [string]$taskLeaf.id
            if ([string]::IsNullOrEmpty($taskRawId)) { continue }
            if ($taskRawId.Contains([char]0x00B7)) { [void]$taskMiddleDotIds.Add($taskRawId) }
            [void]$taskByCategory[$taskName].Add((ConvertTo-PointCategoryId -Id $taskRawId))
        }
    }
}

foreach ($taskExpected in @('采集物', '敌人', '收集物')) {
    Assert-Refreshable ($taskCatalogTopNames.Contains($taskExpected)) "catalog still defines the '$taskExpected' category"
}

$taskRefreshable = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($taskName in @('采集物', '敌人')) {
    if ($taskByCategory.ContainsKey($taskName)) {
        foreach ($taskId in $taskByCategory[$taskName]) { [void]$taskRefreshable.Add($taskId) }
    }
}
$taskHarvest = if ($taskByCategory.ContainsKey('采集物')) { $taskByCategory['采集物'].Count } else { 0 }
$taskEnemy = if ($taskByCategory.ContainsKey('敌人')) { $taskByCategory['敌人'].Count } else { 0 }
Write-Info "refreshable categories: 采集物=$taskHarvest 敌人=$taskEnemy union=$($taskRefreshable.Count)"

$taskEveryCatalogId = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($taskName in $taskByCategory.Keys) {
    foreach ($taskId in $taskByCategory[$taskName]) { [void]$taskEveryCatalogId.Add($taskId) }
}

# Every point in every shipped snapshot, bucketed by the category it is filed under. This is the
# same walk the C++ does at start-up when it builds markerIdentities.
$taskPointCategory = @{}
$taskCategoryPointCount = @{}
$taskPointRootsSeen = 0
foreach ($taskRoot in $taskPointRoots) {
    if (-not (Test-Path -LiteralPath $taskRoot -PathType Container)) { continue }
    $taskPointRootsSeen++
    foreach ($taskFile in @(Get-ChildItem -LiteralPath $taskRoot -Filter 'itemsData_*.json' | Sort-Object Name)) {
        $taskPoints = Get-Content -LiteralPath $taskFile.FullName -Raw | ConvertFrom-Json
        foreach ($taskCategory in $taskPoints) {
            $taskId = ConvertTo-PointCategoryId -Id ([string]$taskCategory.id)
            if (-not $taskCategoryPointCount.ContainsKey($taskId)) { $taskCategoryPointCount[$taskId] = 0 }
            foreach ($taskLocation in $taskCategory.location) {
                $taskPointId = [string]$taskLocation.id
                $taskCategoryPointCount[$taskId]++
                if ($taskPointCategory.ContainsKey($taskPointId)) {
                    if ($taskPointCategory[$taskPointId] -ne $taskId) {
                        # Two categories claiming one point id would break the flat C++ set, which
                        # could then no longer answer "is this cloud identity a refresh point?".
                        $script:taskChecks++
                        $script:taskFailures.Add("point id $taskPointId appears under both $($taskPointCategory[$taskPointId]) and $taskId")
                    }
                }
                else { $taskPointCategory[$taskPointId] = $taskId }
            }
        }
    }
}
Assert-Refreshable ($taskPointRootsSeen -eq $taskPointRoots.Count) "every point-data root was found ($taskPointRootsSeen of $($taskPointRoots.Count))"
Assert-Refreshable ($taskPointCategory.Count -gt 0) "point snapshots indexed: $($taskPointCategory.Count) id(s)"
Assert-Refreshable ($taskFailures.Count -eq 0) "every point id belongs to exactly one category"
Write-Info "point categories in the shipped data: $($taskCategoryPointCount.Count)"

# The load-bearing guarantee: a category the point data actually uses must be classifiable. An
# id outside every catalog group would read as "unknown", and the C++ treats unknown as "not
# refreshable" — safe, but it means a real monster or herb silently stops being marked.
$taskClassifiable = @($taskCategoryPointCount.Keys | Where-Object { $taskEveryCatalogId.Contains($_) })
$taskUnclassifiable = @($taskCategoryPointCount.Keys | Where-Object { -not $taskEveryCatalogId.Contains($_) })
Assert-Refreshable ($taskUnclassifiable.Count -eq 0) `
    "every category used by a shipped point resolves to a catalog group (unknown: $($taskUnclassifiable -join ', '))"

# The two labels for "the same refresh bucket" must agree in the direction that decides the
# question: everything the name table calls 采集物/敌人 has to be refreshable by the catalogs.
$taskFilterPath = Join-Path $taskMapRoot 'filter-items.json'
$taskNewStatePath = Join-Path $taskMapRoot 'new-state-filter-items.json'
if (Test-Path -LiteralPath $taskFilterPath -PathType Leaf) {
    $taskFilterRefresh = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($taskPath in @($taskFilterPath, $taskNewStatePath)) {
        if (-not (Test-Path -LiteralPath $taskPath -PathType Leaf)) { continue }
        $taskFilter = Get-Content -LiteralPath $taskPath -Raw | ConvertFrom-Json
        foreach ($taskProperty in $taskFilter.PSObject.Properties) {
            $taskBucket = ([string]$taskProperty.Name -split '-')[-1]
            if ($taskBucket -notin @('采集物', '敌人')) { continue }
            foreach ($taskEntry in $taskProperty.Value.PSObject.Properties) {
                [void]$taskFilterRefresh.Add((ConvertTo-PointCategoryId -Id ([string]$taskEntry.Name)))
            }
        }
    }
    Assert-Refreshable ($taskFilterRefresh.Count -gt 0) "the name tables list refresh buckets: $($taskFilterRefresh.Count) id(s)"
    $taskMissed = @($taskFilterRefresh | Where-Object { -not $taskRefreshable.Contains($_) })
    Assert-Refreshable ($taskMissed.Count -eq 0) `
        "every id the name tables call 采集物 or 敌人 is refreshable by the catalogs (missed: $($taskMissed -join ', '))"
    $taskCatalogOnly = @($taskRefreshable | Where-Object { -not $taskFilterRefresh.Contains($_) })
    Write-Info "catalog lists $($taskCatalogOnly.Count) refresh id(s) with no name-table entry (regions we ship no points for): $($taskCatalogOnly -join ', ')"
}
else { Write-Info "filter-items.json not found; skipping the cross-file membership check." }

$taskRefreshablePoints = 0
$taskNonRefreshablePoints = 0
foreach ($taskPointId in $taskPointCategory.Keys) {
    if ($taskRefreshable.Contains($taskPointCategory[$taskPointId])) { $taskRefreshablePoints++ } else { $taskNonRefreshablePoints++ }
}
Assert-Refreshable ($taskRefreshablePoints -gt 0) "refreshable points exist: $taskRefreshablePoints"
Assert-Refreshable ($taskNonRefreshablePoints -gt 0) "non-refreshable points exist: $taskNonRefreshablePoints"
Write-Info "points: refreshable=$taskRefreshablePoints other=$taskNonRefreshablePoints"

# The one-off collectible is what the whole guard exists for: it must never share an id with a
# category the farming mode is allowed to tick, or the reset would consume it for good.
$taskCollectionIds = @()
if ($taskByCategory.ContainsKey('收集物')) { $taskCollectionIds = @($taskByCategory['收集物']) }
Assert-Refreshable ($taskCollectionIds.Count -gt 0) "the collectible category is populated: $($taskCollectionIds.Count) id(s)"
$taskOverlap = @($taskCollectionIds | Where-Object { $taskRefreshable.Contains($_) })
Assert-Refreshable ($taskOverlap.Count -eq 0) "no collectible category id is also a refresh category id"

# The middle-dot rewrite is load-bearing while upstream keeps that spelling, and it must fold to
# exactly the ids the point data uses.
$taskFolded = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($taskId in $taskMiddleDotIds) { [void]$taskFolded.Add((ConvertTo-PointCategoryId -Id $taskId)) }
Assert-Refreshable ($taskMiddleDotIds.Contains('sx' + [char]0x00B7 + 'qq')) "upstream still spells the 声匣 id with a middle dot"
Assert-Refreshable ($taskFolded.Contains('sx_qq') -and $taskFolded.Contains('sx_lgn')) "middle-dot ids fold to the underscore spelling"
$taskNormalizedPresent = @(@('sx_qq', 'sx_lgn') | Where-Object { $taskCategoryPointCount.ContainsKey($_) })
Assert-Refreshable ($taskNormalizedPresent.Count -eq 2) "both folded ids are ids the point data really uses"
foreach ($taskId in @('sx_qq', 'sx_lgn')) {
    Assert-Refreshable (-not $taskRefreshable.Contains($taskId)) "$taskId is a one-off collectible, not a refresh category"
}
Write-Info "other middle-dot catalog ids: $(@($taskMiddleDotIds | Where-Object { $_ -notlike 'sx*' }) -join ', ')"

if ($taskFailures.Count -gt 0) {
    throw "Refresh category validation failed with $($taskFailures.Count) problem(s): $($taskFailures -join '; ')"
}
Write-Host "Refresh category validation passed: $taskChecks checks ($($taskRefreshable.Count) refresh category ids, $taskRefreshablePoints points)."
