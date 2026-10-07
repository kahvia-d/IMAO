[CmdletBinding()]
param([Parameter(Mandatory)][string]$RunRoot, [Parameter(Mandatory)][string]$BinaryRoot,
    [Parameter(Mandatory)][string]$EvidenceRoot)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'Use PowerShell 7.' }
$RunRoot = [IO.Path]::GetFullPath($RunRoot).TrimEnd('\')
$BinaryRoot = [IO.Path]::GetFullPath($BinaryRoot).TrimEnd('\')
$EvidenceRoot = [IO.Path]::GetFullPath($EvidenceRoot).TrimEnd('\')
& (Join-Path $PSScriptRoot 'Assert-SelfContainedRuntime.ps1') -AppRoot $BinaryRoot
if ($RunRoot -eq $BinaryRoot -or $EvidenceRoot.StartsWith($RunRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Test tree, build and backup must be separate directories.'
}
if (Test-Path -LiteralPath $EvidenceRoot) { throw 'Use a new evidence/backup directory.' }
foreach ($name in @('IMao-WinUI.exe','IMao-WinUI.dll','IMao-CoreHost.exe','SDL3.dll','SDL3-LICENSE.txt','build-info.json')) {
    if (-not (Test-Path -LiteralPath (Join-Path $BinaryRoot $name))) { throw "Incomplete build: $name" }
}
$assets = Join-Path $RunRoot 'Assets'
if (-not (Test-Path -LiteralPath (Join-Path $assets 'Updates/bundled-snapshot.json'))) { throw 'Existing maptest snapshot required.' }
if ((Get-Item -LiteralPath $RunRoot).LinkType -or (Get-Item -LiteralPath $assets).LinkType) { throw 'Maptest root and Assets must be physical directories.' }
foreach ($name in @('Map_features.imf','Map_visual_index.imx')) {
    if (Test-Path -LiteralPath (Join-Path $assets "FeaturesDatas/$name")) { throw 'This updater preserves only the existing no-base-atlas layout.' }
}
foreach ($process in @(Get-CimInstance Win32_Process -Filter "name = 'IMao-WinUI.exe' OR name = 'IMao-CoreHost.exe'")) {
    if ($process.ExecutablePath -and $process.ExecutablePath.StartsWith($RunRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Close the maptest application before updating. Other installations are not stopped.'
    }
}
$oldSnapshot = Get-Content (Join-Path $assets 'Updates/bundled-snapshot.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$build = Get-Content (Join-Path $BinaryRoot 'build-info.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$backup = Join-Path $EvidenceRoot 'backup'
$replacement = Join-Path $EvidenceRoot 'replacement'
[IO.Directory]::CreateDirectory($backup) | Out-Null
[IO.Directory]::CreateDirectory($replacement) | Out-Null
$bootstrapDotnet = Join-Path (Split-Path -Parent $PSScriptRoot) 'tools/dotnet-sdk-8.0.424/dotnet.exe'
if (-not (Test-Path -LiteralPath $bootstrapDotnet)) { $bootstrapDotnet = (Get-Command dotnet -ErrorAction Stop).Source }
$bootstrapOutput = Join-Path $EvidenceRoot 'bootstrap-probe'
& $bootstrapDotnet build (Join-Path $PSScriptRoot '../Tests/ResourceBootstrapRuntime/ResourceBootstrapRuntime.csproj') -c Release -o $bootstrapOutput -p:NuGetAudit=false
if ($LASTEXITCODE -ne 0) { throw 'Client bootstrap probe build failed before deployment.' }
$links = @()
foreach ($name in @('KuroMap','KuroMapIcons','Updates')) {
    $existing = Get-Item -LiteralPath (Join-Path $assets $name)
    if ($existing.LinkType -and $existing.LinkType -ne 'Junction') { throw 'Only physical or junction resource directories are supported.' }
    $links += [ordered]@{ name = $name; linkType = $existing.LinkType; target = $existing.Target }
    Copy-Item -LiteralPath $existing.FullName -Destination (Join-Path $backup $name) -Recurse
    Copy-Item -LiteralPath (Join-Path $BinaryRoot "Assets/$name") -Destination (Join-Path $replacement $name) -Recurse
}
$links | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $EvidenceRoot 'original-links.json') -Encoding UTF8
# Preserve the user's exact calibration and scene approval, including local trials.
foreach ($name in @('scene-calibrations.json','scene-validation.json')) {
    Copy-Item -LiteralPath (Join-Path $backup "KuroMap/$name") -Destination (Join-Path $replacement "KuroMap/$name") -Force
}
# The snapshot inventory below hashes the preserved calibration and approval files.
# Compute all binary replacements and validate parent paths before touching any program file.
$changes = @()
foreach ($file in Get-ChildItem -LiteralPath $BinaryRoot -Recurse -File) {
    $relative = $file.FullName.Substring($BinaryRoot.Length + 1)
    if ($relative.StartsWith('Assets\') -or $relative -match '(^|\\)(publish|SavedPoints|SavedRoutes|ProgramUpdates)(\\|$)' -or $relative -match 'Tests\.exe$') { continue }
    $target = Join-Path $RunRoot $relative
    $parent = [IO.Path]::GetDirectoryName($target)
    while ($parent.Length -gt $RunRoot.Length) {
        if ((Test-Path -LiteralPath $parent) -and (Get-Item -LiteralPath $parent).LinkType) { throw "Refusing binary directory junction: $parent" }
        $parent = [IO.Path]::GetDirectoryName($parent)
    }
    $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    if ((Test-Path -LiteralPath $target) -and (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() -ceq $hash) { continue }
    $changes += [ordered]@{ path = $relative; sha256 = $hash; size = $file.Length; existed = Test-Path -LiteralPath $target }
}
# Validate preserved packages and save every affected binary before mutation.
foreach ($package in $oldSnapshot.packages | Where-Object kind -ne 'map-features') {
    $directory = if ([IO.Path]::IsPathRooted($package.directory)) { [IO.Path]::GetFullPath($package.directory) } else { [IO.Path]::GetFullPath((Join-Path $assets $package.directory)) }
    if (-not $directory.StartsWith($assets + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Snapshot package escaped test Assets.' }
    $sourceDirectory = if ($package.kind -eq 'map-data') { Join-Path $replacement 'KuroMap' } elseif ($package.kind -eq 'map-icons') { Join-Path $replacement 'KuroMapIcons' } else { $directory }
    $manifestName = if ($package.kind -eq 'map-icons') { 'icon-manifest.json' } else { 'manifest.json' }
    $null = Get-Content (Join-Path $sourceDirectory $manifestName) -Raw -Encoding UTF8 | ConvertFrom-Json
}
$changes | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $EvidenceRoot 'binary-changes.json') -Encoding UTF8
foreach ($entry in $changes | Where-Object existed) {
    $saved = Join-Path $backup "binaries/$($entry.path)"
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($saved)) | Out-Null
    Copy-Item -LiteralPath (Join-Path $RunRoot $entry.path) -Destination $saved
}
$runReceipt = Join-Path $RunRoot 'maptest-build-receipt.json'
$hadReceipt = Test-Path -LiteralPath $runReceipt
if ($hadReceipt) { Copy-Item -LiteralPath $runReceipt -Destination (Join-Path $backup 'maptest-build-receipt.json') }
$receiptTouched = $false
$appliedBinaries = @()
$appliedResources = @()
function Remove-TestResource([string]$Path) {
    if (-not $Path.StartsWith($assets + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Rollback path escaped test Assets.' }
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $item = Get-Item -LiteralPath $Path
    if ($item.LinkType -eq 'Junction') { [IO.Directory]::Delete($Path, $false); return }
    if ($item.LinkType -or @(Get-ChildItem -LiteralPath $Path -Recurse -Force | Where-Object LinkType).Count) { throw 'Unexpected nested resource link during rollback.' }
    Remove-Item -LiteralPath $Path -Recurse -Force
}
try {
foreach ($entry in $changes) {
    $target = Join-Path $RunRoot $entry.path
    $appliedBinaries += $entry
    # Unlink hard-linked files instead of overwriting their installed source bytes.
    if ($entry.existed) { [IO.File]::Delete($target) }
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
    Copy-Item -LiteralPath (Join-Path $BinaryRoot $entry.path) -Destination $target
}
foreach ($entry in $links) {
    $target = Join-Path $assets $entry.name
    $appliedResources += $entry
    if ($entry.linkType -eq 'Junction') { [IO.Directory]::Delete($target, $false) }
    elseif ($entry.linkType) { throw "Unsupported resource link: $($entry.linkType)" }
    else { Move-Item -LiteralPath $target -Destination (Join-Path $EvidenceRoot "original-$($entry.name)") }
    Copy-Item -LiteralPath (Join-Path $replacement $entry.name) -Destination $target -Recurse
}
$baseline = Join-Path $assets 'Updates/baseline-files.json'
if (Test-Path -LiteralPath $baseline) { [IO.File]::Delete($baseline) }
$packages = @()
foreach ($package in $oldSnapshot.packages | Where-Object kind -ne 'map-features') {
    $directory = if ([IO.Path]::IsPathRooted($package.directory)) { [IO.Path]::GetFullPath($package.directory) } else { [IO.Path]::GetFullPath((Join-Path $assets $package.directory)) }
    if (-not $directory.StartsWith($assets + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Snapshot package escaped test Assets.' }
    $manifestPath = Join-Path $directory $(if ($package.kind -eq 'map-icons') { 'icon-manifest.json' } else { 'manifest.json' })
    $manifest = Get-Content $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $files = @(Get-ChildItem -LiteralPath $directory -Recurse -File | Sort-Object FullName | ForEach-Object {
        [ordered]@{ path = $_.FullName.Substring($directory.Length + 1).Replace('\','/'); size = $_.Length;
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    })
    $packageVersion = if ($package.kind -in @('map-data','map-icons')) { $build.appVersion } else { $package.version }
    $packages += [ordered]@{ id = $package.id; kind = $package.kind; version = $packageVersion; directory = $directory;
        sha256 = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant(); files = $files }
}
$snapshot = [ordered]@{ formatVersion = 2; snapshotId = "maptest-$($build.sourceCommit)-$(Get-Date -Format yyyyMMddHHmmss)";
    sequence = 0; baselineId = $oldSnapshot.baselineId; minAppVersion = $build.appVersion; baselineRoot = $assets;
    mapDataRoot = (Join-Path $assets 'KuroMap'); mapIconRoot = (Join-Path $assets 'KuroMapIcons'); mapFeatureRoot = '';
    bundled = $false; packages = $packages }
$snapshotPath = Join-Path $assets 'Updates/bundled-snapshot.json'
$candidatePath = Join-Path $EvidenceRoot 'native-candidate.json'
$snapshot | ConvertTo-Json -Depth 100 | Set-Content $candidatePath -Encoding UTF8
# The client bootstrap accepts only a relative format-1 bundled descriptor.
# The absolute format-2 non-bundled candidate is strictly separate native evidence.
$snapshot.formatVersion = 1
$snapshot.bundled = $true
$snapshot.baselineRoot = '.'
$snapshot.mapDataRoot = 'KuroMap'
$snapshot.mapIconRoot = 'KuroMapIcons'
foreach ($package in $packages) { $package.directory = [IO.Path]::GetRelativePath($assets, $package.directory).Replace('\','/') }
$snapshot | ConvertTo-Json -Depth 100 | Set-Content $snapshotPath -Encoding UTF8
& (Join-Path $RunRoot 'IMao-CoreHost.exe') --check-resource-snapshot $candidatePath
if ($LASTEXITCODE -ne 0) { throw 'Updated maptest snapshot failed native validation. Backup retained; do not launch.' }
& (Join-Path $PSScriptRoot 'Assert-SelfContainedRuntime.ps1') -AppRoot $RunRoot
& $bootstrapDotnet (Join-Path $bootstrapOutput 'ResourceBootstrapRuntime.dll') $RunRoot (Join-Path $EvidenceRoot 'bootstrap-state')
if ($LASTEXITCODE -ne 0) { throw 'Production client bootstrap rejected updated maptest; restoring previous tree.' }
$receipt = [ordered]@{ sourceCommit = $build.sourceCommit; sourceDirty = $build.sourceDirty; version = $build.appVersion;
    completedAtUtc = [DateTime]::UtcNow.ToString('o'); runRoot = $RunRoot; backup = $EvidenceRoot;
    snapshotSha256 = (Get-FileHash $snapshotPath -Algorithm SHA256).Hash.ToLowerInvariant(); changedBinaries = $changes;
    resourceManifests = @($packages | Select-Object id, sha256) }
$receipt | ConvertTo-Json -Depth 100 | Set-Content (Join-Path $EvidenceRoot 'receipt.json') -Encoding UTF8
$receiptTouched = $true
if (Test-Path -LiteralPath $runReceipt) { [IO.File]::Delete($runReceipt) }
Copy-Item -LiteralPath (Join-Path $EvidenceRoot 'receipt.json') -Destination $runReceipt
Write-Host "Maptest updated: $RunRoot. Backup/evidence: $EvidenceRoot"
} catch {
    $failure = $_
    $rollbackFailures = @()
    foreach ($entry in $appliedResources) {
      try {
        $target = Join-Path $assets $entry.name
        Remove-TestResource $target
        if ($entry.linkType -eq 'Junction') { New-Item -ItemType Junction -Path $target -Target $entry.target | Out-Null }
        elseif (Test-Path -LiteralPath (Join-Path $EvidenceRoot "original-$($entry.name)")) {
            Move-Item -LiteralPath (Join-Path $EvidenceRoot "original-$($entry.name)") -Destination $target
        } else { Copy-Item -LiteralPath (Join-Path $backup $entry.name) -Destination $target -Recurse }
      } catch { $rollbackFailures += "resource $($entry.name): $($_.Exception.Message)" }
    }
    foreach ($entry in $appliedBinaries) {
      try {
        $target = Join-Path $RunRoot $entry.path
        if (Test-Path -LiteralPath $target) { [IO.File]::Delete($target) }
        if ($entry.existed) { Copy-Item -LiteralPath (Join-Path $backup "binaries/$($entry.path)") -Destination $target }
      } catch { $rollbackFailures += "binary $($entry.path): $($_.Exception.Message)" }
    }
    if ($receiptTouched) {
      try {
        if (Test-Path -LiteralPath $runReceipt) { [IO.File]::Delete($runReceipt) }
        if ($hadReceipt) { Copy-Item -LiteralPath (Join-Path $backup 'maptest-build-receipt.json') -Destination $runReceipt }
      } catch { $rollbackFailures += "receipt: $($_.Exception.Message)" }
    }
    if ($rollbackFailures.Count) {
        $rollbackFailures | Set-Content (Join-Path $EvidenceRoot 'rollback-failures.txt') -Encoding UTF8
        throw "Maptest update failed; recovery is INCOMPLETE. Do not launch. $($failure.Exception.Message). Recovery failures: $($rollbackFailures -join '; '). Backup: $EvidenceRoot"
    }
    throw "Maptest update failed and original binaries/resources were restored: $($failure.Exception.Message). Evidence: $EvidenceRoot"
}
