[CmdletBinding()]
param(
    [string]$Source,
    [string]$Contrib,
    [string]$BuildDirectory,
    [switch]$ConfigureOnly
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($Source)) { $Source = Join-Path $repoRoot 'third_party\src\opencv-4.11.0' }
if ([string]::IsNullOrWhiteSpace($Contrib)) { $Contrib = Join-Path $repoRoot 'third_party\src\opencv_contrib-4.11.0' }
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) { $BuildDirectory = Join-Path $repoRoot 'third_party\build\opencv-4.11.0' }

foreach ($path in @(
    (Join-Path $Source 'CMakeLists.txt'),
    (Join-Path $Contrib 'modules\xfeatures2d\include\opencv2\xfeatures2d.hpp')
)) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "OpenCV source dependency is missing: $path"
    }
}

# OpenCV 4.11.0 compiles VGG's factory without its getDefaultName() method
# when the optional descriptor data is unavailable.  We do not use VGG or
# BoostDesc, so make that offline configuration link correctly and skip the
# otherwise unnecessary network downloads.  SURF remains enabled.
$offlineVggPatch = Join-Path $repoRoot 'patches\opencv_contrib-4.11.0-offline-vgg.patch'
if (-not (Test-Path -LiteralPath $offlineVggPatch)) { throw "Missing OpenCV offline patch: $offlineVggPatch" }
& git -C $Contrib apply --reverse --check $offlineVggPatch 2>$null
if ($LASTEXITCODE -ne 0) {
    & git -C $Contrib apply --check $offlineVggPatch
    if ($LASTEXITCODE -ne 0) { throw 'The OpenCV offline patch does not apply. Use a clean opencv_contrib 4.11.0 checkout.' }
    & git -C $Contrib apply $offlineVggPatch
    if ($LASTEXITCODE -ne 0) { throw 'Unable to apply the OpenCV offline patch.' }
}

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer (vswhere) was not found.' }

$cmake = & $vswhere -latest -products * -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' | Select-Object -First 1
$ninja = & $vswhere -latest -products * -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe' | Select-Object -First 1
$vcvars = & $vswhere -latest -products * -find 'VC\Auxiliary\Build\vcvars64.bat' | Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($cmake) -or [string]::IsNullOrWhiteSpace($ninja) -or [string]::IsNullOrWhiteSpace($vcvars)) {
    throw 'Visual Studio CMake, Ninja, or the x64 developer environment is missing.'
}

function Invoke-VisualStudioCommand([string]$CommandLine) {
    $logPath = Join-Path $env:TEMP ("imao-opencv-" + [guid]::NewGuid().ToString() + '.log')
    $cmdCommand = 'call "' + $vcvars + '" >nul && (' + $CommandLine + ') > "' + $logPath + '" 2>&1'
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.UseShellExecute = $false

    # The desktop host supplies both Path and PATH. Keep one spelling: MSBuild
    # treats the duplicate as an invalid environment dictionary.
    [void]$startInfo.EnvironmentVariables
    [void]$startInfo.EnvironmentVariables.Remove('Path')
    [void]$startInfo.EnvironmentVariables.Remove('PATH')
    $startInfo.EnvironmentVariables.Add('PATH', (Split-Path -Parent $ninja) + ';' + [System.Environment]::GetEnvironmentVariable('Path', 'Process'))
    $startInfo.FileName = "$env:SystemRoot\System32\cmd.exe"
    $startInfo.Arguments = '/d /c ' + $cmdCommand
    $startInfo.WorkingDirectory = $repoRoot

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    [void]$process.Start()
    $process.WaitForExit()
    if (Test-Path -LiteralPath $logPath) {
        Get-Content -LiteralPath $logPath
        Remove-Item -LiteralPath $logPath -Force -ErrorAction SilentlyContinue
    }
    if ($process.ExitCode -ne 0) { throw "Command failed with exit code $($process.ExitCode): $CommandLine" }
}

$configure = '"' + $cmake + '" -S "' + $Source + '" -B "' + $BuildDirectory + '" -G Ninja ' +
    '-DCMAKE_BUILD_TYPE=Release -DOPENCV_SKIP_SYSTEM_PROCESSOR_DETECTION=ON -DX86_64=ON -DOPENCV_EXTRA_MODULES_PATH="' + (Join-Path $Contrib 'modules') + '" ' +
    '-DBUILD_SHARED_LIBS=ON -DBUILD_opencv_world=ON -DOPENCV_ENABLE_NONFREE=ON ' +
    '-DBUILD_LIST=core,imgproc,imgcodecs,features2d,flann,calib3d,xfeatures2d ' +
    '-DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_opencv_apps=OFF -DBUILD_opencv_highgui=OFF -DBUILD_opencv_videoio=OFF -DOPENCV_PYTHON_SKIP_DETECTION=ON -DOPENCV_SKIP_FEATURES2D_DOWNLOADING=ON ' +
    '-DWITH_FFMPEG=OFF -DWITH_IPP=OFF -DWITH_ITT=OFF -DWITH_OPENCL=OFF -DWITH_AVIF=OFF'

Invoke-VisualStudioCommand $configure
if ($ConfigureOnly) {
    Write-Host "OpenCV configured: $BuildDirectory" -ForegroundColor Green
    exit 0
}

Invoke-VisualStudioCommand ('"' + $cmake + '" --build "' + $BuildDirectory + '" --target opencv_world --parallel')
Write-Host "OpenCV built: $BuildDirectory" -ForegroundColor Green
