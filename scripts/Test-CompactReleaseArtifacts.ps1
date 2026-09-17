[CmdletBinding()]
param([string]$OutputRoot)
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
if (-not $OutputRoot) { $OutputRoot = Join-Path $repoRoot ('out/compact-test-' + [guid]::NewGuid().ToString('N')) }
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot, $repoRoot)
if (Test-Path -LiteralPath $OutputRoot) { throw 'Use a fresh compaction regression output directory.' }
[IO.Directory]::CreateDirectory($OutputRoot) | Out-Null
$scriptPath = Join-Path $PSScriptRoot 'Compact-ReleaseArtifacts.ps1'
$passed = [Collections.Generic.List[string]]::new()

function New-FixtureFile([string]$Relative, [int]$Size) {
    $path = Join-Path $OutputRoot $Relative
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) | Out-Null
    $bytes = [byte[]]::new($Size)
    for ($index = 0; $index -lt $Size; $index++) { $bytes[$index] = [byte](($index * 31 + $Relative.Length) % 251) }
    [IO.File]::WriteAllBytes($path, $bytes)
    return $path
}
$olderCandidate = 'release-candidate-2026.1.1.1'
$olderPrepared = 'maps-2026.1.1.2'
$newest = 'maps-2026.1.1.3'
foreach ($target in @($olderCandidate, $olderPrepared, $newest)) { New-FixtureFile "$target/keep-marker.tmp" 4 | Out-Null }
New-FixtureFile "$olderCandidate/native-build/object.bin" 4096 | Out-Null
New-FixtureFile "$olderCandidate/native/IMao-CoreHost.exe" 8192 | Out-Null
New-FixtureFile "$olderCandidate/native/native-build-info.json" 128 | Out-Null
New-FixtureFile "$olderCandidate/native-build.log" 64 | Out-Null
New-FixtureFile "$olderCandidate/publish/build-info.json" 96 | Out-Null
New-FixtureFile "$olderCandidate/launcher/launcher-build-info.json" 96 | Out-Null
New-FixtureFile "$olderCandidate/program/IMao-v2026.1.1.1-windows-x64.report.json" 256 | Out-Null
New-FixtureFile "$olderCandidate/program/IMao-v2026.1.1.1-windows-x64/IMao-WinUI.exe" 16384 | Out-Null
New-FixtureFile "$olderPrepared/release-report.json" 512 | Out-Null
New-FixtureFile "$olderPrepared/update.json" 768 | Out-Null
New-FixtureFile "$olderPrepared/preflight-snapshot.json" 256 | Out-Null
New-FixtureFile "$olderPrepared/native-check.stdout.json" 128 | Out-Null
New-FixtureFile "$olderPrepared/packages/map-data.zip" 4096 | Out-Null
New-FixtureFile "$olderPrepared/resources-2026.1.1.2-offline.zip" 4096 | Out-Null
New-FixtureFile "$olderPrepared/publish-verification-aaa/previous-stable.json" 256 | Out-Null
New-FixtureFile "$olderPrepared/publish-verification-aaa/stable-promotion.json" 256 | Out-Null
New-FixtureFile "$olderPrepared/publish-verification-aaa/channel-state-promotion.json" 128 | Out-Null
New-FixtureFile "$olderPrepared/publish-verification-aaa/uploaded-1/program.zip" 8192 | Out-Null
New-FixtureFile "$olderPrepared/program-verification/app/IMao-WinUI.exe" 8192 | Out-Null
New-FixtureFile 'runtime-tests/result.json' 128 | Out-Null
[IO.Directory]::SetLastWriteTime((Join-Path $OutputRoot $olderCandidate), [datetime]'2026-01-01T00:00:00Z')
[IO.Directory]::SetLastWriteTime((Join-Path $OutputRoot $olderPrepared), [datetime]'2026-01-02T00:00:00Z')
[IO.Directory]::SetLastWriteTime((Join-Path $OutputRoot $newest), [datetime]'2026-01-03T00:00:00Z')

# The sources disappear with their directories, so record their hashes before compaction runs.
$expectedEvidence = @{
    $olderCandidate = @('native/native-build-info.json', 'native-build.log', 'publish/build-info.json', 'launcher/launcher-build-info.json', 'program/IMao-v2026.1.1.1-windows-x64.report.json')
    $olderPrepared = @('release-report.json', 'update.json', 'preflight-snapshot.json', 'native-check.stdout.json', 'publish-verification-aaa/previous-stable.json', 'publish-verification-aaa/stable-promotion.json', 'publish-verification-aaa/channel-state-promotion.json')
}
$expectedHashes = @{}
foreach ($target in $expectedEvidence.Keys) {
    foreach ($relative in $expectedEvidence[$target]) { $expectedHashes["$target/$relative"] = (Get-FileHash -LiteralPath (Join-Path $OutputRoot "$target/$relative") -Algorithm SHA256).Hash }
}

$dryRun = & $scriptPath -OutputRoot $OutputRoot -WhatIf *>&1
if (-not (Test-Path -LiteralPath (Join-Path $OutputRoot $olderCandidate)) -or (Test-Path -LiteralPath (Join-Path $OutputRoot 'evidence'))) { throw 'A dry run must not change the directory.' }
if (($dryRun -join "`n") -notlike '*keep native/native-build-info.json*' -or ($dryRun -join "`n") -notlike '*keep publish-verification-aaa/stable-promotion.json*') { throw 'A dry run must list the evidence it would keep.' }
$passed.Add('dry run lists retained evidence without changing the directory')

$output = & $scriptPath -OutputRoot $OutputRoot -RetainFull 1 *>&1
if (-not (Test-Path -LiteralPath (Join-Path $OutputRoot $newest))) { throw 'The retained artifact directory was removed.' }
foreach ($target in @($olderCandidate, $olderPrepared)) {
    if (Test-Path -LiteralPath (Join-Path $OutputRoot $target)) { throw "Compacted artifact directory survived: $target" }
}
if (-not (Test-Path -LiteralPath (Join-Path $OutputRoot 'runtime-tests/result.json'))) { throw 'Compaction must not touch unrelated output directories.' }
$passed.Add('compaction removes candidate and prepared payloads, retaining the newest artifact in full')

foreach ($key in $expectedHashes.Keys) {
    $archived = Join-Path $OutputRoot "evidence/$key"
    if (-not (Test-Path -LiteralPath $archived)) { throw "Evidence was not archived: $key" }
    if ((Get-FileHash -LiteralPath $archived -Algorithm SHA256).Hash -ne $expectedHashes[$key]) { throw "Archived evidence differs from the file that was removed: $key" }
}
if (Test-Path -LiteralPath (Join-Path $OutputRoot "evidence/$olderPrepared/publish-verification-aaa/uploaded-1")) { throw 'Verification downloads must not be archived as evidence.' }
$passed.Add('archived evidence matches the hashes recorded before compaction and excludes re-downloadable payloads')

$manifest = Get-Content -LiteralPath (Join-Path $OutputRoot "evidence/$olderPrepared/archive-manifest.json") -Raw | ConvertFrom-Json
if ($manifest.formatVersion -ne 1 -or $manifest.artifact -ne $olderPrepared -or $manifest.releasedBytes -le 0 -or $manifest.evidence.Count -lt 5) { throw 'Archive manifest does not describe the compacted artifact.' }
if ($manifest.evidence[0].sha256.Length -ne 64) { throw 'Archive manifest evidence hashes are incomplete.' }
$passed.Add('archive manifest records artifact identity, released bytes and evidence hashes')

$finalRun = & $scriptPath -OutputRoot $OutputRoot 2>&1
if (Test-Path -LiteralPath (Join-Path $OutputRoot $newest)) { throw 'The newest artifact survived the default full compaction.' }
if (-not (Test-Path -LiteralPath (Join-Path $OutputRoot "evidence/$newest/archive-manifest.json"))) { throw 'The newest artifact produced no archive manifest.' }
$passed.Add('a later run compacts previously retained artifacts')

$rejected = $false
try { & $scriptPath -OutputRoot $repoRoot 2>&1 | Out-Null } catch { $rejected = $true }
if (-not $rejected) { throw 'Compacting outside the out directory must be refused.' }
$rejected = $false
try { & $scriptPath -OutputRoot $OutputRoot -EvidenceRoot (Join-Path $repoRoot 'Docs') 2>&1 | Out-Null } catch { $rejected = $true }
if (-not $rejected) { throw 'Archiving evidence outside the out directory must be refused.' }
$passed.Add('targets outside the out directory are refused')

[IO.File]::WriteAllText((Join-Path $OutputRoot 'test-report.json'), (@{ passed = $passed.Count; tests = @($passed.ToArray()) } | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
Write-Host "PASS $($passed.Count) compaction regressions."
