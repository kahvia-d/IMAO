[CmdletBinding()]
param(
    [string]$PaddleLib = $env:IMAO_PADDLE_LIB,
    [string]$OpenCvDir = $env:IMAO_OPENCV_DIR
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$xmlPath = Join-Path $repoRoot 'Assets\FeaturesDatas\Map_features.yml'
$imfPath = Join-Path $repoRoot 'Assets\FeaturesDatas\Map_features.imf'
$manifestPath = Join-Path $repoRoot 'Assets\FeaturesDatas\Map_features.manifest.json'

if (-not (Test-Path -LiteralPath $xmlPath)) { throw "Missing XML source: $xmlPath" }
if ([string]::IsNullOrWhiteSpace($PaddleLib)) { throw 'Set IMAO_PADDLE_LIB or pass -PaddleLib.' }
if ([string]::IsNullOrWhiteSpace($OpenCvDir)) { throw 'Set IMAO_OPENCV_DIR or pass -OpenCvDir.' }

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$cmake = & $vswhere -latest -products * -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' | Select-Object -First 1
$vcvars = & $vswhere -latest -products * -find 'VC\Auxiliary\Build\vcvars64.bat' | Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($cmake) -or [string]::IsNullOrWhiteSpace($vcvars)) {
    throw 'Visual Studio CMake or the x64 developer environment is unavailable.'
}

$configureLine = 'call "' + $vcvars + '" >nul && "' + $cmake + '" --fresh --preset windows-x64-release "-DPADDLE_LIB=' + $PaddleLib + '" "-DOPENCV_DIR=' + $OpenCvDir + '"'
& "$env:SystemRoot\System32\cmd.exe" /d /c $configureLine
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }

$commandLine = 'call "' + $vcvars + '" >nul && "' + $cmake + '" --build --preset windows-x64-release-feature-converter'
& "$env:SystemRoot\System32\cmd.exe" /d /c $commandLine
if ($LASTEXITCODE -ne 0) { throw 'IMaoFeatureConverter build failed.' }

$outputDirectory = Join-Path $repoRoot 'x64\Release'
$opencvRuntime = Get-ChildItem -Path $OpenCvDir -Recurse -Filter 'opencv_world*.dll' -File | Sort-Object FullName | Select-Object -First 1
if ($null -eq $opencvRuntime) { throw 'Unable to find opencv_world*.dll.' }
Copy-Item -LiteralPath $opencvRuntime.FullName -Destination $outputDirectory -Force

$converter = Join-Path $outputDirectory 'IMaoFeatureConverter.exe'
& $converter $xmlPath $imfPath $manifestPath
if ($LASTEXITCODE -ne 0) { throw 'Feature conversion or exact round-trip validation failed.' }
Write-Host "Binary feature resource generated: $imfPath" -ForegroundColor Green
