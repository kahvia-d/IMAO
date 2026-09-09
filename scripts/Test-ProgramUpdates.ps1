[CmdletBinding()]
param([string]$OutputDirectory = 'out/program-updates-tests')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-DevEnvironment.ps1')
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskOutput = [IO.Path]::GetFullPath($OutputDirectory, $taskRoot)
[IO.Directory]::CreateDirectory($taskOutput) | Out-Null
& $env:IMAO_DOTNET build (Join-Path $taskRoot 'tests/ProgramUpdates/ProgramUpdates.csproj') -c Release -o (Join-Path $taskOutput 'bin') --source $env:NUGET_PACKAGES -p:NuGetAudit=false
if ($LASTEXITCODE -ne 0) { throw 'Program update test build failed.' }
& $env:IMAO_DOTNET (Join-Path $taskOutput 'bin/ProgramUpdates.dll') (Join-Path $taskOutput ('run-' + [guid]::NewGuid().ToString('N')))
if ($LASTEXITCODE -ne 0) { throw 'Program update tests failed.' }
