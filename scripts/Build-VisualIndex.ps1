[CmdletBinding()]
param(
    [string]$PaddleLib = $env:IMAO_PADDLE_LIB,
    [string]$OpenCvDir = $env:IMAO_OPENCV_DIR
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$imfPath = Join-Path $repoRoot 'Assets\FeaturesDatas\Map_features.imf'
$indexPath = Join-Path $repoRoot 'Assets\FeaturesDatas\Map_visual_index.imx'
$manifestPath = Join-Path $repoRoot 'Assets\FeaturesDatas\Map_visual_index.manifest.json'
if (-not (Test-Path -LiteralPath $imfPath)) { throw "Missing IMF source: $imfPath" }
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

$commandLine = 'call "' + $vcvars + '" >nul && "' + $cmake + '" --build out\build\windows-x64-release --target IMaoFeatureConverter IMaoVisualIndexBuilder'
& "$env:SystemRoot\System32\cmd.exe" /d /c $commandLine
if ($LASTEXITCODE -ne 0) { throw 'IMaoVisualIndexBuilder build failed.' }

$outputDirectory = Join-Path $repoRoot 'x64\Release'
$opencvRuntime = Get-ChildItem -Path $OpenCvDir -Recurse -Filter 'opencv_world*.dll' -File | Sort-Object FullName | Select-Object -First 1
if ($null -eq $opencvRuntime) { throw 'Unable to find opencv_world*.dll.' }
Copy-Item -LiteralPath $opencvRuntime.FullName -Destination $outputDirectory -Force

$builder = Join-Path $outputDirectory 'IMaoVisualIndexBuilder.exe'
$converter = Join-Path $outputDirectory 'IMaoFeatureConverter.exe'
$featureRoot = Join-Path $repoRoot 'Assets\FeaturesDatas'
$registryPath = Join-Path $featureRoot 'kuro-tile-packs.json'
$packDirectories = if (Test-Path -LiteralPath $registryPath) {
    $registry = Get-Content -LiteralPath $registryPath -Raw | ConvertFrom-Json
    if ([int]$registry.formatVersion -ne 1 -or $null -eq $registry.packs) { throw 'Kuro tile-pack registry is invalid.' }
    @($registry.packs | ForEach-Object { [string]$_ })
} else {
    @('Dreamzhou')
}
foreach ($packDirectory in $packDirectories) {
    if ([string]::IsNullOrWhiteSpace($packDirectory) -or [IO.Path]::GetFileName($packDirectory) -ne $packDirectory) {
        throw "Kuro tile-pack registry contains an unsafe directory: $packDirectory"
    }
    $kuroPack = Join-Path $featureRoot "KuroTilePacks\$packDirectory"
    $kuroXml = Join-Path $kuroPack 'features.yml'
    if (-not (Test-Path -LiteralPath $kuroXml)) { continue }
    & $converter $kuroXml (Join-Path $kuroPack 'features.imf') (Join-Path $kuroPack 'features.imf.manifest.json')
    if ($LASTEXITCODE -ne 0) { throw "Kuro feature binary generation failed for $packDirectory." }
}
& $builder (Join-Path $repoRoot 'Assets') $indexPath $manifestPath
if ($LASTEXITCODE -ne 0) { throw 'Visual index generation or exact round-trip validation failed.' }
Write-Host "Visual index generated: $indexPath" -ForegroundColor Green
