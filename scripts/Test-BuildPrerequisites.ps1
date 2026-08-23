[CmdletBinding()]
param(
    [string]$PaddleLib = $env:IMAO_PADDLE_LIB,
    [string]$OpenCvDir = $env:IMAO_OPENCV_DIR,
    [string]$Dotnet = $env:IMAO_DOTNET
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$errors = [System.Collections.Generic.List[string]]::new()

function Test-Requirement([bool]$Condition, [string]$Message) {
    if (-not $Condition) {
        $script:errors.Add($Message)
    }
}

function Find-VisualStudioTool([string]$RelativePath) {
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        return $null
    }

    $result = [string](& $vswhere -latest -products * -find $RelativePath 2>$null | Select-Object -First 1)
    if ([string]::IsNullOrWhiteSpace($result)) {
        return $null
    }
    return $result
}

$cmake = Find-VisualStudioTool 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ninja = Find-VisualStudioTool 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$msvcCompiler = Find-VisualStudioTool 'VC\Tools\MSVC\**\bin\Hostx64\x64\cl.exe'
$msbuild = Find-VisualStudioTool 'MSBuild\Current\Bin\MSBuild.exe'

Test-Requirement ($null -ne $cmake) 'Missing Visual Studio CMake. Install the C++ CMake tools component.'
Test-Requirement ($null -ne $ninja) 'Missing Ninja from Visual Studio CMake tools.'
Test-Requirement ($null -ne $msvcCompiler) 'Missing MSVC x64 compiler. Install the Desktop development with C++ workload.'
Test-Requirement ($null -ne $msbuild) 'Missing MSBuild from Visual Studio Build Tools.'
Test-Requirement (Test-Path -LiteralPath 'C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um\windows.h') 'Missing Windows SDK 10.0.26100.0 headers.'

if ([string]::IsNullOrWhiteSpace($Dotnet)) {
    $dotnetCommand = Get-Command dotnet -ErrorAction SilentlyContinue
    if ($null -ne $dotnetCommand) {
        $Dotnet = $dotnetCommand.Source
    }
}

$hasDotnet8Sdk = $false
if (-not [string]::IsNullOrWhiteSpace($Dotnet) -and (Test-Path -LiteralPath $Dotnet)) {
    $sdkList = & $Dotnet --list-sdks 2>$null
    $hasDotnet8Sdk = [bool]($sdkList | Where-Object { $_ -match '^8\.' })
}
Test-Requirement $hasDotnet8Sdk 'Missing .NET 8 SDK. A runtime alone cannot build the WinUI project.'

Test-Requirement (-not [string]::IsNullOrWhiteSpace($PaddleLib)) 'PADDLE_LIB is unset. Set IMAO_PADDLE_LIB to the Paddle Inference root.'
if (-not [string]::IsNullOrWhiteSpace($PaddleLib)) {
    Test-Requirement (Test-Path -LiteralPath (Join-Path $PaddleLib 'paddle\include\paddle_inference_api.h')) 'PADDLE_LIB does not contain paddle/include/paddle_inference_api.h.'
    Test-Requirement (Test-Path -LiteralPath (Join-Path $PaddleLib 'paddle\lib\paddle_inference.lib')) 'PADDLE_LIB does not contain paddle/lib/paddle_inference.lib.'
    Test-Requirement (Test-Path -LiteralPath (Join-Path $PaddleLib 'third_party\install\mklml\lib\mklml.lib')) 'PADDLE_LIB is missing the MKL development libraries required by CMakeLists.txt.'
}

Test-Requirement (-not [string]::IsNullOrWhiteSpace($OpenCvDir)) 'OPENCV_DIR is unset. Set IMAO_OPENCV_DIR to the folder containing OpenCVConfig.cmake.'
if (-not [string]::IsNullOrWhiteSpace($OpenCvDir)) {
    Test-Requirement (Test-Path -LiteralPath (Join-Path $OpenCvDir 'OpenCVConfig.cmake')) 'OPENCV_DIR does not contain OpenCVConfig.cmake.'
}

Test-Requirement (Test-Path -LiteralPath (Join-Path $repoRoot 'Assets\FeaturesDatas\Map_features.yml')) 'Missing archived runtime assets. Run Git LFS pull or restore Assets/.'
Test-Requirement (Test-Path -LiteralPath (Join-Path $repoRoot 'IMao-Core\paddleocr_cpp_infer\lib\ppocr.lib')) 'Missing the committed PaddleOCR helper library.'

if ($errors.Count -gt 0) {
    Write-Host 'Build prerequisites: FAILED' -ForegroundColor Red
    $errors | ForEach-Object { Write-Host " - $_" -ForegroundColor Red }
    exit 1
}

Write-Host 'Build prerequisites: PASSED' -ForegroundColor Green
Write-Host "CMake:  $cmake"
Write-Host "Ninja:  $ninja"
Write-Host "MSVC:   $msvcCompiler"
Write-Host "MSBuild: $msbuild"
Write-Host "dotnet: $Dotnet"
Write-Host "Paddle: $PaddleLib"
Write-Host "OpenCV: $OpenCvDir"
