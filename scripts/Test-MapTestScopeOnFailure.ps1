[CmdletBinding()]
param([Parameter(Mandatory)][string]$OutputRoot)
$ErrorActionPreference = 'Stop'
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if (Test-Path -LiteralPath $OutputRoot) { throw 'Use a new fixture directory.' }
$binary = Join-Path $OutputRoot 'source'
$assets = Join-Path $binary 'Assets'
[IO.Directory]::CreateDirectory($assets) | Out-Null
[IO.File]::WriteAllText((Join-Path $binary 'IMao-WinUI.exe'), 'fixture-only')
foreach ($script in @('New-MapTestTree.ps1','New-NoBaselineMapTestTree.ps1')) {
    $run = Join-Path $OutputRoot ([IO.Path]::GetFileNameWithoutExtension($script))
    $arguments = @{ RunRoot = $run; BinaryRoot = $binary; AssetsRoot = $assets }
    if ($script -eq 'New-MapTestTree.ps1') { $arguments.PackRegionId = 'fixture-missing-region' }
    $failed = $false
    try { & (Join-Path $PSScriptRoot $script) @arguments | Out-Null }
    catch { $failed = $true }
    if (-not $failed) { throw "Fixture should fail during tree creation: $script" }
    if (-not (Test-Path -LiteralPath (Join-Path $run 'maptest-build-receipt.json'))) {
        throw "An incomplete tree was left without resource isolation: $script"
    }
    if (-not (Test-Path -LiteralPath (Join-Path $run 'IMao-WinUI.exe'))) {
        throw "Failure fixture did not reach executable deployment: $script"
    }
}
'PASS incomplete maptest trees retain isolation before executable deployment.'
