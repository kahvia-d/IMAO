[CmdletBinding()]
param([Parameter(Mandatory)][string]$FixtureRoot, [Parameter(Mandatory)][string]$BinaryRoot,
    [Parameter(Mandatory)][string]$OutputRoot)
$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $OutputRoot) { throw 'Use a fresh test output directory.' }
$badBuild = Join-Path $OutputRoot 'bad-build'
[IO.Directory]::CreateDirectory($badBuild) | Out-Null
foreach ($name in @('IMao-WinUI.exe','IMao-WinUI.dll','SDL3.dll','SDL3-LICENSE.txt','SDL3-BUILD.json','build-info.json')) {
    Copy-Item -LiteralPath (Join-Path $BinaryRoot $name) -Destination (Join-Path $badBuild $name)
}
# Deliberate post-mutation executable failure; only the disposable fixture is touched.
[IO.File]::WriteAllText((Join-Path $badBuild 'IMao-CoreHost.exe'), 'invalid executable rollback fixture')
[IO.Directory]::CreateDirectory((Join-Path $badBuild 'Assets')) | Out-Null
foreach ($name in @('KuroMap','KuroMapIcons','Updates')) {
    Copy-Item -LiteralPath (Join-Path $BinaryRoot "Assets/$name") -Destination (Join-Path $badBuild "Assets/$name") -Recurse
}
$snapshot = Join-Path $FixtureRoot 'Assets/Updates/bundled-snapshot.json'
$native = Join-Path $FixtureRoot 'IMao-CoreHost.exe'
$beforeSnapshot = (Get-FileHash $snapshot).Hash
$beforeNative = (Get-FileHash $native).Hash
$beforeResources = @{}
foreach ($name in @('KuroMap','KuroMapIcons','Updates')) {
    $item = Get-Item -LiteralPath (Join-Path $FixtureRoot "Assets/$name")
    $beforeResources[$name] = "$($item.LinkType):$($item.Target)"
}
$failed = $false
try { & (Join-Path $PSScriptRoot 'Update-MapTestBuild.ps1') -RunRoot $FixtureRoot -BinaryRoot $badBuild -EvidenceRoot (Join-Path $OutputRoot 'evidence') }
catch {
    if ($_.Exception.Message -notlike '*original binaries/resources were restored*') { throw }
    $failed = $true
}
if (-not $failed) { throw 'Failure fixture unexpectedly passed.' }
if ((Get-FileHash $snapshot).Hash -cne $beforeSnapshot -or (Get-FileHash $native).Hash -cne $beforeNative) { throw 'Rollback changed original files.' }
foreach ($name in $beforeResources.Keys) {
    $item = Get-Item -LiteralPath (Join-Path $FixtureRoot "Assets/$name")
    if ("$($item.LinkType):$($item.Target)" -cne $beforeResources[$name]) { throw 'Rollback changed resource directory ownership.' }
}
Write-Host 'Post-mutation failure restored snapshot, binary and resource ownership.'
