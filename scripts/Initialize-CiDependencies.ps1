[CmdletBinding()]
param([string]$LockFile, [string]$ArchiveRoot)
$ErrorActionPreference='Stop'
$repoRoot=Split-Path -Parent $PSScriptRoot
if (-not $LockFile) { $LockFile=Join-Path $repoRoot '.github/dependencies.lock.json' }
if (-not $ArchiveRoot) { $ArchiveRoot=Join-Path $repoRoot 'third_party/downloads' }
$env:SOURCE_DATE_EPOCH=[string](& git -C $repoRoot show -s --format=%ct HEAD)
if ($LASTEXITCODE -ne 0 -or $env:SOURCE_DATE_EPOCH -notmatch '^\d+$') { throw 'Cannot establish reproducible source date.' }
$lock=Get-Content $LockFile -Raw | ConvertFrom-Json
if ((& dotnet --version).Trim() -cne $lock.dotnetSdk) { throw 'The locked .NET SDK is not active.' }
$patch=Join-Path $repoRoot 'patches/opencv_contrib-4.11.0-offline-vgg.patch'
if ((Get-FileHash $patch -Algorithm SHA256).Hash.ToLowerInvariant() -cne $lock.opencvPatchSha256) { throw 'OpenCV patch differs from dependency lock.' }
[IO.Directory]::CreateDirectory($ArchiveRoot) | Out-Null
foreach ($dep in $lock.dependencies) {
    if ($dep.name -eq 'sdl3') { continue }
    $archive=Join-Path $ArchiveRoot "$($dep.name)-$($dep.version).zip"
    if (-not (Test-Path $archive)) { Invoke-WebRequest -Uri $dep.url -OutFile $archive -TimeoutSec 600 }
    if ((Get-Item $archive).Length -ne $dep.size -or (Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant() -cne $dep.sha256) { throw "Dependency hash/size mismatch: $($dep.name). Cached bytes were not trusted." }
    $destination=if ($dep.name -eq 'paddle') { Join-Path $repoRoot 'third_party/paddle-inference-3.0.0' } else { Join-Path $repoRoot 'third_party/src' }
    [IO.Directory]::CreateDirectory($destination) | Out-Null
    Expand-Archive -LiteralPath $archive -DestinationPath $destination -Force
}
& (Join-Path $PSScriptRoot 'Initialize-GamepadDependency.ps1') -SourceRoot $repoRoot -ArchiveRoot $ArchiveRoot
$env:IMAO_DOTNET=(Get-Command dotnet).Source
$env:IMAO_PADDLE_LIB=Join-Path $repoRoot 'third_party/paddle-inference-3.0.0/paddle_inference'
if (-not (Test-Path (Join-Path $env:IMAO_PADDLE_LIB 'paddle/include/paddle_inference_api.h'))) { $env:IMAO_PADDLE_LIB=Split-Path $env:IMAO_PADDLE_LIB }
if (-not (Test-Path (Join-Path $env:IMAO_PADDLE_LIB 'paddle/include/paddle_inference_api.h'))) { throw 'Unexpected Paddle archive layout.' }
$paddleVersionFile=Join-Path $env:IMAO_PADDLE_LIB 'version.txt'
if (-not (Test-Path $paddleVersionFile)) { $paddleVersionFile=Join-Path (Split-Path $env:IMAO_PADDLE_LIB) 'version.txt' }
$paddleVersion=Get-Content $paddleVersionFile -Raw
if ($paddleVersion -notmatch '6ed5dd3833c32c3b21e14b1fb1a71f5a535a0fcc') { throw 'Paddle binary provenance differs from locked 3.0.0 CPU build.' }
foreach ($name in @('opencv','opencv_contrib')) {
    & git -C (Join-Path $repoRoot "third_party/src/$name-4.11.0") init --quiet
    if ($LASTEXITCODE -ne 0) { throw 'Cannot initialize extracted dependency patch workspace.' }
}
. (Join-Path $PSScriptRoot 'Enter-DevEnvironment.ps1')
& (Join-Path $PSScriptRoot 'Build-OpenCV.ps1')
$vswhere='C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products * -version '[17.0,18.0)' -property installationPath
if (-not $vs) { throw 'Visual Studio 2022 is required.' }
$toolset=@(Get-ChildItem (Join-Path $vs 'VC/Tools/MSVC') -Directory | Where-Object Name -Like "$($lock.msvc).*" | Sort-Object Name -Descending)[0]
if (-not $toolset) { throw 'Locked MSVC 14.44 toolset missing.' }
if (-not (Test-Path "${env:ProgramFiles(x86)}/Windows Kits/10/Include/$($lock.windowsSdk)/um/Windows.h")) { throw 'Locked Windows SDK missing.' }
& cmake -S $repoRoot -B (Join-Path $repoRoot 'out/ci-native-config') -G 'Visual Studio 17 2022' -A x64 -T "v143,version=$($toolset.Name)" "-DCMAKE_GENERATOR_INSTANCE=$vs" "-DCMAKE_SYSTEM_VERSION=$($lock.windowsSdk)" "-DPADDLE_LIB=$env:IMAO_PADDLE_LIB" "-DOPENCV_DIR=$env:IMAO_OPENCV_DIR" '-DIMAO_ENABLE_DIAGNOSTICS=OFF' '-DIMAO_ALLOW_XML_FEATURE_FALLBACK=OFF'
if ($LASTEXITCODE -ne 0) { throw 'Cloud native configuration failed.' }
if ($env:GITHUB_ENV) {
    "SOURCE_DATE_EPOCH=$env:SOURCE_DATE_EPOCH" >> $env:GITHUB_ENV
    "IMAO_DOTNET=$env:IMAO_DOTNET" >> $env:GITHUB_ENV
    "IMAO_PADDLE_LIB=$env:IMAO_PADDLE_LIB" >> $env:GITHUB_ENV
    "IMAO_OPENCV_DIR=$env:IMAO_OPENCV_DIR" >> $env:GITHUB_ENV
}
