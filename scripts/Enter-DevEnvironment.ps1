# Dot-source this script before using the repository build scripts:
# . .\scripts\Enter-DevEnvironment.ps1
$repoRoot = Split-Path -Parent $PSScriptRoot
$env:IMAO_DOTNET = Join-Path $repoRoot 'tools\dotnet-sdk-8.0.424\dotnet.exe'
$env:IMAO_OPENCV_DIR = Join-Path $repoRoot 'third_party\build\opencv-4.11.0'
$env:IMAO_PADDLE_LIB = Join-Path $repoRoot 'third_party\paddle-inference-3.0.0'
if (Test-Path -LiteralPath (Join-Path $env:IMAO_PADDLE_LIB 'paddle_inference\paddle')) {
    $env:IMAO_PADDLE_LIB = Join-Path $env:IMAO_PADDLE_LIB 'paddle_inference'
}
$env:DOTNET_ROOT = Split-Path -Parent $env:IMAO_DOTNET
$env:DOTNET_CLI_HOME = Join-Path $repoRoot 'third_party\dotnet-cli-home'
$env:NUGET_PACKAGES = Join-Path $repoRoot 'third_party\nuget-packages'
$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
if (($env:Path -split ';') -notcontains $env:DOTNET_ROOT) {
    $env:Path = $env:DOTNET_ROOT + ';' + $env:Path
}
Write-Host "Development environment loaded for $repoRoot"
