[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$DependencyRoot,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$BuildDirectory
)
$ErrorActionPreference = 'Stop'
$sourceRoot = Split-Path -Parent $PSScriptRoot
$DependencyRoot = (Resolve-Path -LiteralPath $DependencyRoot).Path
. (Join-Path $DependencyRoot 'scripts/Enter-DevEnvironment.ps1')
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $sourceRoot 'out/build/coordinate-offline' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a new output directory; existing capture packages are never overwritten.' }
& (Join-Path $PSScriptRoot 'Build-CoordinateOffline.ps1') -DependencyRoot $DependencyRoot -BuildDirectory $BuildDirectory
$exe = Join-Path $BuildDirectory 'IMaoCoordinateSampleCapture.exe'
$opencv = Join-Path $DependencyRoot 'x64/Release/opencv_world4110.dll'
if (-not (Test-Path -LiteralPath $exe) -or -not (Test-Path -LiteralPath $opencv)) { throw 'Capture executable/OpenCV runtime is missing.' }
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products * -property installationPath
$redistParent = Join-Path $installation 'VC/Redist/MSVC'
$crt = Get-ChildItem -LiteralPath $redistParent -Directory | Sort-Object Name -Descending |
    ForEach-Object { Join-Path $_.FullName 'x64/Microsoft.VC143.CRT' } |
    Where-Object { Test-Path -LiteralPath (Join-Path $_ 'msvcp140.dll') } | Select-Object -First 1
if (-not $crt) { throw 'Existing local x64 VC runtime cannot be found. No runtime is downloaded.' }
$sources = @($exe, $opencv, (Join-Path $PSScriptRoot 'Start-CoordinateSampleCapture.ps1'),
    (Join-Path $sourceRoot 'Docs/CoordinateSampleCapture_zh-Hans.md'),
    (Join-Path $sourceRoot 'Docs/CoordinateOfflineEvaluation_zh-Hans.md')) +
    @(Get-ChildItem -LiteralPath $crt -Filter '*.dll' -File | Select-Object -ExpandProperty FullName)
foreach ($source in $sources) { if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Package input is missing: $source" } }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$sourceCommit = & git -C $sourceRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'Cannot record package source commit.' }
$dirty = @(& git -C $sourceRoot status --porcelain)
if ($LASTEXITCODE -ne 0) { throw 'Cannot record package source status.' }
$manifest = [ordered]@{formatVersion=1; kind='native-coordinate-sample-tool'; sourceCommit=$sourceCommit;
    sourceTreeDirty=($dirty.Count -gt 0); files=@()}
foreach ($source in $sources) {
    $destination = Join-Path $OutputDirectory ([IO.Path]::GetFileName($source))
    Copy-Item -LiteralPath $source -Destination $destination
    $expected = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    $actual = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
    if ($actual -ne $expected) { throw "Package copy verification failed: $destination" }
    $manifest.files += [ordered]@{name=[IO.Path]::GetFileName($source); sha256=$actual; bytes=(Get-Item -LiteralPath $destination).Length}
}
foreach ($action in 'Start', 'Stop') {
    $switch = if ($action -eq 'Stop') { ' -Stop' } else { '' }
    $command = '@echo off' + "`r`n" + 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-CoordinateSampleCapture.ps1" -ToolRoot "%~dp0."' + $switch + "`r`npause`r`n"
    $path = Join-Path $OutputDirectory ($action + '-Capture.cmd')
    [IO.File]::WriteAllText($path, $command, [Text.Encoding]::ASCII)
    $manifest.files += [ordered]@{name=[IO.Path]::GetFileName($path); sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash; bytes=(Get-Item -LiteralPath $path).Length}
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'package-manifest.json') -Encoding UTF8
Write-Host "Capture package ready: $OutputDirectory"
Write-Host 'Start-Capture.cmd launches paused; Stop-Capture.cmd finishes without force-killing.'
