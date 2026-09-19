param(
    [string]$ProgramDirectory = 'x64/Release',
    [string]$StateDirectory = "$env:LOCALAPPDATA/IMao-WinUI/ResourceUpdates",
    [string]$EvidenceDirectory = 'out/resource-selection-20260909'
)
$ErrorActionPreference = 'Stop'
$program = (Resolve-Path -LiteralPath $ProgramDirectory).Path
$stateRoot = (Resolve-Path -LiteralPath $StateDirectory).Path
$evidence = [IO.Path]::GetFullPath($EvidenceDirectory)
if (Get-Process -Name 'IMao-WinUI','IMao-CoreHost' -ErrorAction SilentlyContinue) { throw '请先退出 IMao。' }
New-Item -ItemType Directory -Force -Path $evidence | Out-Null
$assets = Join-Path $program 'Assets'
$snapshot = Get-Content -LiteralPath (Join-Path $assets 'Updates/bundled-snapshot.json') -Raw | ConvertFrom-Json
if (!$snapshot.bundled -or $snapshot.formatVersion -ne 1) { throw 'Invalid bundled snapshot.' }
$snapshot.baselineRoot = $assets
$snapshot.mapDataRoot = Join-Path $assets $snapshot.mapDataRoot
# Optional roots must be absolute as well; a relative one is rejected by the native validator.
foreach ($opt in @('mapIconRoot','mapFeatureRoot')) {
    $value = ''
    if ($snapshot.PSObject.Properties.Name -contains $opt) { $value = [string]$snapshot.$opt }
    if ($value) { $snapshot.$opt = Join-Path $assets $value }
}
foreach ($package in $snapshot.packages) { $package.directory = Join-Path $assets $package.directory }
$candidate = Join-Path $evidence 'bundled-runtime.json'
$snapshot | ConvertTo-Json -Depth 50 | Set-Content -LiteralPath $candidate -Encoding utf8
$report = & (Join-Path $program 'IMao-CoreHost.exe') --check-resource-snapshot $candidate
$report | Set-Content -LiteralPath (Join-Path $evidence 'resource-check.log') -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw 'Bundled resources failed native validation.' }
$ready = @($report | ForEach-Object { try { $_ | ConvertFrom-Json } catch {} } | Where-Object {
    $_.resourcesReady -eq $true -and $_.resourceSnapshotId -eq $snapshot.snapshotId
})
if (!$ready.Count) { throw 'Missing native resource readiness confirmation.' }
$lock = [IO.File]::Open((Join-Path $stateRoot '.update.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
try {
    $activationPath = Join-Path $stateRoot 'activation.json'
    $before = [IO.File]::ReadAllBytes($activationPath)
    $backup = Join-Path $evidence ('activation-before-' + [guid]::NewGuid().ToString('N') + '.json')
    [IO.File]::WriteAllBytes($backup, $before)
    $state = [Text.Encoding]::UTF8.GetString($before).TrimStart([char]0xfeff) | ConvertFrom-Json
    if ($state.attempt -or $state.pendingPath) { throw '资源仍有待启用更新，请先处理该更新。' }
    $stored = Join-Path $stateRoot ('snapshots/restored-bundled-' + [guid]::NewGuid().ToString('N') + '.json')
    Copy-Item -LiteralPath $candidate -Destination $stored
    $state.previousPath = $state.activePath
    $state.activePath = $stored
    $state.baselineId = $snapshot.baselineId
    $temp = $activationPath + '.restore.tmp'
    [IO.File]::WriteAllText($temp, ($state | ConvertTo-Json -Depth 20), [Text.UTF8Encoding]::new($false))
    [IO.File]::Replace($temp, $activationPath, ($backup + '.atomic'))
    Write-Output "Restored $($snapshot.snapshotId); previous selection backed up to $backup"
} finally { $lock.Dispose() }
