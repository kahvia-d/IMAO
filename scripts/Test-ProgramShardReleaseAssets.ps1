[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutputRoot,
    # Optional: a real prepared program release. When given, the same binding is run against the real
    # archives so the upload plan can be reviewed before anything is uploaded.
    [string]$RealPreparedRoot
)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'PowerShell 7 or newer is required.' }
. (Join-Path $PSScriptRoot 'ResourceUpdateAssets.ps1')
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if (Test-Path -LiteralPath $OutputRoot) { throw 'Use a fresh shard asset regression output directory.' }
[IO.Directory]::CreateDirectory($OutputRoot) | Out-Null
$repo = 'kahvia-d/IMAO'
$tag = 'v2026.9.20.1'
$passed = [Collections.Generic.List[string]]::new()
function Expect-Rejection([string]$Name, [scriptblock]$Action) {
    try { & $Action } catch { $passed.Add($Name); return }
    throw "Expected rejection: $Name"
}
function Write-Asset([string]$Name, [string]$Content) {
    $path = Join-Path $OutputRoot ('program/' + $Name)
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) | Out-Null
    [IO.File]::WriteAllText($path, $Content, [Text.UTF8Encoding]::new($false))
    return [pscustomobject]@{ name = $Name; size = [IO.FileInfo]::new($path).Length; sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
}
function Get-Url([string]$Name, [string]$Release) { return "https://github.com/$repo/releases/download/$Release/$Name" }
# One changed shard on this release, one unchanged shard and the descriptor pointing at this release.
$changed = Write-Asset 'IMao-v2026.9.20.1-ui.zip' 'changed shard bytes'
$unchanged = Write-Asset 'IMao-v2026.9.19.2-runtime.zip' 'unchanged shard bytes'
$descriptor = Write-Asset 'IMao-v2026.9.20.1-shards.json' '{"formatVersion":1}'
function New-Catalog {
    return [pscustomobject]@{ app = [pscustomobject]@{ package = [pscustomobject]@{
        url = (Get-Url $descriptor.name $tag); size = $descriptor.size; sha256 = $descriptor.sha256
        shards = @(
            [pscustomobject]@{ id = 'ui'; url = (Get-Url $changed.name $tag); size = $changed.size; sha256 = $changed.sha256; files = @('IMao-WinUI.exe') },
            [pscustomobject]@{ id = 'runtime'; url = (Get-Url $unchanged.name 'v2026.9.19.2'); size = $unchanged.size; sha256 = $unchanged.sha256; files = @('System.Private.CoreLib.dll') }
        ) } } }
}
$plan = Get-ProgramShardAssets (New-Catalog) $OutputRoot $repo $tag
if ($plan.Upload.Count -ne 2 -or $plan.Retained.Count -ne 1) { throw "Expected two uploads and one retained asset, got $($plan.Upload.Count)/$($plan.Retained.Count)." }
if (@($plan.Upload | Where-Object { $_.name -ne $changed.name -and $_.name -ne $descriptor.name }).Count -ne 0) { throw 'Only this release''s shard and descriptor may be uploaded.' }
if ($plan.Retained[0].name -ne $unchanged.name -or $plan.Retained[0].url -ne (Get-Url $unchanged.name 'v2026.9.19.2')) { throw 'An unchanged shard must keep its published URL.' }
$passed.Add('changed shard and descriptor upload while an unchanged shard keeps its published URL')
$carried = Get-RetainedProgramAssets (New-Catalog) $repo 'v2026.9.21.1'
if ($carried.Count -ne 3) { throw "A carried-forward program release must have all three assets checked for reachability, got $($carried.Count)." }
$passed.Add('a release that does not build the program still asks every published URL')
Expect-Rejection 'a shard whose local bytes differ from the signed catalog is refused' {
    $catalog = New-Catalog; $catalog.app.package.shards[0].sha256 = ('a' * 64); Get-ProgramShardAssets $catalog $OutputRoot $repo $tag | Out-Null
}
Expect-Rejection 'a shard whose local length differs from the signed catalog is refused' {
    $catalog = New-Catalog; $catalog.app.package.shards[0].size = [long]$catalog.app.package.shards[0].size + 1; Get-ProgramShardAssets $catalog $OutputRoot $repo $tag | Out-Null
}
Expect-Rejection 'a shard archive missing from the prepared output is refused' {
    $catalog = New-Catalog; $catalog.app.package.shards[0].url = (Get-Url 'IMao-v2026.9.20.1-absent.zip' $tag); Get-ProgramShardAssets $catalog $OutputRoot $repo $tag | Out-Null
}
Expect-Rejection 'a descriptor whose bytes differ from the signed catalog is refused' {
    $catalog = New-Catalog; $catalog.app.package.sha256 = ('b' * 64); Get-ProgramShardAssets $catalog $OutputRoot $repo $tag | Out-Null
}
Expect-Rejection 'a catalog without a shard program package is refused' {
    $catalog = New-Catalog; $catalog.app.package.shards = @(); Get-ProgramShardAssets $catalog $OutputRoot $repo $tag | Out-Null
}
Expect-Rejection 'a catalog without a program package is refused' {
    $catalog = New-Catalog; $catalog.app.package = $null; Get-RetainedProgramAssets $catalog $repo $tag | Out-Null; Get-ProgramShardAssets $catalog $OutputRoot $repo $tag | Out-Null
}
# The first-install archive is not named by the catalog, so its package report has to tie it to the release.
$releaseReport = [pscustomobject]@{ sourceCommit = ('c' * 40); appVersion = '2026.9.20.1' }
$zipPath = Join-Path $OutputRoot 'IMao-v2026.9.20.1-windows-x64.zip'
[IO.File]::WriteAllText($zipPath, 'complete program archive', [Text.UTF8Encoding]::new($false))
$zipFile = [IO.FileInfo]::new($zipPath)
$reportPath = [IO.Path]::ChangeExtension($zipPath, '.report.json')
function Write-ProgramReport([hashtable]$Overrides) {
    $body = @{ passed = $true; sourceDirty = $false; sourceCommit = $releaseReport.sourceCommit; version = $releaseReport.appVersion
        sha256 = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant(); size = $zipFile.Length }
    foreach ($key in $Overrides.Keys) { $body[$key] = $Overrides[$key] }
    [IO.File]::WriteAllText($reportPath, ($body | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
}
Write-ProgramReport @{}
$manual = Assert-ManualInstallArchive $zipPath $releaseReport
if ($manual.name -ne $zipFile.Name -or $manual.sha256 -ne (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()) { throw 'The first-install archive must be bound to its own verified bytes.' }
$passed.Add('a first-install archive with a matching clean-source package report is accepted')
foreach ($case in @(
    @{ Name = 'a first-install archive without a package report is refused'; Overrides = $null },
    @{ Name = 'a first-install archive whose report failed packaging is refused'; Overrides = @{ passed = $false } },
    @{ Name = 'a first-install archive built from a dirty tree is refused'; Overrides = @{ sourceDirty = $true } },
    @{ Name = 'a first-install archive from another source commit is refused'; Overrides = @{ sourceCommit = ('d' * 40) } },
    @{ Name = 'a first-install archive for another version is refused'; Overrides = @{ version = '2026.9.20.2' } },
    @{ Name = 'a first-install archive that changed after verification is refused'; Overrides = @{ sha256 = ('e' * 64) } },
    @{ Name = 'a first-install archive whose length changed is refused'; Overrides = @{ size = [long]$zipFile.Length + 1 } }
)) {
    if ($null -eq $case.Overrides) { if (Test-Path -LiteralPath $reportPath) { Remove-Item -LiteralPath $reportPath -Force } }
    else { Write-ProgramReport $case.Overrides }
    Expect-Rejection $case.Name { Assert-ManualInstallArchive $zipPath $releaseReport | Out-Null }
    Write-ProgramReport @{}
}
Expect-Rejection 'a first-install archive that does not exist is refused' {
    Assert-ManualInstallArchive (Join-Path $OutputRoot 'absent.zip') $releaseReport | Out-Null
}
if ($RealPreparedRoot) {
    $realRoot = [IO.Path]::GetFullPath($RealPreparedRoot)
    $envelope = Get-Content -LiteralPath (Join-Path $realRoot 'update.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $realCatalog = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($envelope.payload)) | ConvertFrom-Json
    $report = Get-Content -LiteralPath (Join-Path $realRoot 'release-report.json') -Raw | ConvertFrom-Json
    if (-not $report.programPrepared -or $report.programPrepared -ne $true) { throw 'A prepared program release must report programPrepared.' }
    $realPlan = Get-ProgramShardAssets $realCatalog $realRoot $repo ([string]$report.tag)
    # Every archive is read out of the prepared output and compared against the signed catalog before it
    # reaches this point, so a retained shard is bound exactly as tightly as an uploaded one. An
    # incremental release is expected to keep the archives whose bytes did not change on their published
    # URL, so retention is not an error here; what must hold is that the plan accounts for every shard and
    # the descriptor, and that a release which built the program uploads something of its own.
    $expected = @($realCatalog.app.package.shards).Count + 1
    if (($realPlan.Upload.Count + $realPlan.Retained.Count) -ne $expected) {
        throw "Every shard plus the descriptor must be in the plan: expected $expected, got $($realPlan.Upload.Count + $realPlan.Retained.Count)."
    }
    if ($realPlan.Upload.Count -eq 0) { throw 'A prepared program release must bind at least one archive to its own tag.' }
    $total = ($realPlan.Upload | ForEach-Object { [IO.FileInfo]::new($_.path).Length } | Measure-Object -Sum).Sum
    $retainedTotal = ($realPlan.Retained | ForEach-Object { [IO.FileInfo]::new($_.path).Length } | Measure-Object -Sum).Sum
    Write-Host ("real prepared release {0}: {1} program archive(s) bound to the signed catalog, {2:N1} MB to upload, {3} retained on their published URL ({4:N1} MB)" -f $report.tag, $realPlan.Upload.Count, ($total / 1MB), $realPlan.Retained.Count, ($retainedTotal / 1MB))
    foreach ($asset in $realPlan.Upload) { Write-Host ("  upload    {0,-44} {1,10:N2} MB" -f $asset.name, ([IO.FileInfo]::new($asset.path).Length / 1MB)) }
    foreach ($asset in $realPlan.Retained) { Write-Host ("  retained  {0,-44} {1,10:N2} MB  {2}" -f $asset.name, ([IO.FileInfo]::new($asset.path).Length / 1MB), $asset.url) }
    $passed.Add('a real prepared release binds every shard and the descriptor to the signed catalog')
}
Write-Host ("Program shard asset checks: {0} passed." -f $passed.Count)
