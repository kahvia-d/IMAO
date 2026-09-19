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

$hasCompatibleDotnetSdk = $false
if (-not [string]::IsNullOrWhiteSpace($Dotnet) -and (Test-Path -LiteralPath $Dotnet)) {
    $sdkList = & $Dotnet --list-sdks 2>$null
    $hasCompatibleDotnetSdk = [bool]($sdkList | Where-Object {
        $_ -match '^(\d+)\.' -and [int]$Matches[1] -ge 8
    })
}
Test-Requirement $hasCompatibleDotnetSdk 'Missing a .NET SDK version 8 or newer. A runtime alone cannot build the WinUI project.'

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
# The base map features are being retired: every region pack now carries its own calibrated features and the
# runtime tolerates the base set being absent. A checkout that still has them keeps using them, so they are
# only required to be a matching pair — both present (still shipping them) or both absent (retired).
$baseFeatureBinary = Join-Path $repoRoot 'Assets\FeaturesDatas\Map_features.imf'
$baseVisualIndex = Join-Path $repoRoot 'Assets\FeaturesDatas\Map_visual_index.imx'
Test-Requirement ((Test-Path -LiteralPath $baseFeatureBinary) -eq (Test-Path -LiteralPath $baseVisualIndex)) 'Map_features.imf and Map_visual_index.imx must both be present or both be absent.'
$kuroRegistryPath = Join-Path $repoRoot 'Assets\FeaturesDatas\kuro-tile-packs.json'
Test-Requirement (Test-Path -LiteralPath $kuroRegistryPath) 'Missing kuro-tile-packs.json. Restore the tile-pack registry.'
if (Test-Path -LiteralPath $kuroRegistryPath) {
    try {
        $kuroRegistry = Get-Content -LiteralPath $kuroRegistryPath -Raw | ConvertFrom-Json
        Test-Requirement ([int]$kuroRegistry.formatVersion -eq 1 -and $null -ne $kuroRegistry.packs) 'kuro-tile-packs.json has an unsupported format.'
        foreach ($packDirectoryValue in @($kuroRegistry.packs)) {
            $packDirectory = [string]$packDirectoryValue
            $packRoot = Join-Path $repoRoot (Join-Path 'Assets\FeaturesDatas\KuroTilePacks' $packDirectory)
            $manifestPath = Join-Path $packRoot 'manifest.json'
            # Every registered pack must now be present: the registry no longer names
            # packs that are intentionally absent, and the old Dreamzhou exemption would
            # have masked a missing manifest for a shipped region.
            Test-Requirement (Test-Path -LiteralPath $manifestPath) "Missing Kuro tile-pack manifest: $packDirectory"
            if (-not (Test-Path -LiteralPath $manifestPath)) { continue }
            Test-Requirement (Test-Path -LiteralPath (Join-Path $packRoot 'visual-index.imx')) "Missing Kuro visual-index.imx shard: $packDirectory. Run scripts\Build-VisualIndex.ps1."
            Test-Requirement (Test-Path -LiteralPath (Join-Path $packRoot 'features.imf')) "Missing Kuro binary features: $packDirectory. Run scripts\Build-VisualIndex.ps1."
        }
    }
    catch {
        Test-Requirement $false "Unable to parse kuro-tile-packs.json: $($_.Exception.Message)"
    }
}
$candidateRegistryPath = Join-Path $repoRoot 'Assets\FeaturesDatas\candidate-packs.json'
# Optional. A layout without curated candidate packs ships no registry, which is the current state:
# the mengzhou region pack superseded the Dreamzhou curated candidate.
if (Test-Path -LiteralPath $candidateRegistryPath) {
    try {
        $candidateRegistry = Get-Content -LiteralPath $candidateRegistryPath -Raw | ConvertFrom-Json
        Test-Requirement ([int]$candidateRegistry.formatVersion -eq 1 -and $null -ne $candidateRegistry.packs) 'candidate-packs.json has an unsupported format.'
        foreach ($candidateDirectory in @($candidateRegistry.packs)) {
            $candidateName = [string]$candidateDirectory
            $candidateRoot = Join-Path $repoRoot (Join-Path 'Assets\FeaturesDatas' $candidateName)
            Test-Requirement (Test-Path -LiteralPath (Join-Path $candidateRoot 'manifest.json')) "Missing candidate manifest: $candidateName"
            Test-Requirement (Test-Path -LiteralPath (Join-Path $candidateRoot 'visual-index.imx')) "Missing candidate visual-index.imx shard: $candidateName. Run scripts\Build-VisualIndex.ps1."
        }
    }
    catch {
        Test-Requirement $false "Unable to parse candidate-packs.json: $($_.Exception.Message)"
    }
}
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
