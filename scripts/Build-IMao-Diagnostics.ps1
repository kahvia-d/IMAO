[CmdletBinding()]
param(
    [string]$PaddleLib = $env:IMAO_PADDLE_LIB,
    [string]$OpenCvDir = $env:IMAO_OPENCV_DIR,
    [string]$Dotnet = $env:IMAO_DOTNET,
    [switch]$BuildOnly
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$preflight = Join-Path $PSScriptRoot 'Test-BuildPrerequisites.ps1'

if ([string]::IsNullOrWhiteSpace($Dotnet)) {
    $Dotnet = Join-Path $repoRoot 'tools\dotnet-sdk-8.0.424\dotnet.exe'
}

& $preflight -PaddleLib $PaddleLib -OpenCvDir $OpenCvDir -Dotnet $Dotnet
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$cmake = & $vswhere -latest -products * -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' | Select-Object -First 1
$ninja = & $vswhere -latest -products * -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe' | Select-Object -First 1
$vcvars = & $vswhere -latest -products * -find 'VC\Auxiliary\Build\vcvars64.bat' | Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($cmake) -or [string]::IsNullOrWhiteSpace($ninja) -or [string]::IsNullOrWhiteSpace($vcvars)) {
    throw 'Unable to locate the Visual Studio CMake, Ninja, or x64 developer environment.'
}

function Invoke-VisualStudioCommand([string]$CommandLine) {
    $logPath = Join-Path $env:TEMP ("imao-diagnostics-" + [guid]::NewGuid().ToString() + '.log')
    $cmdCommand = 'call "' + $vcvars + '" >nul && (' + $CommandLine + ') > "' + $logPath + '" 2>&1'
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.UseShellExecute = $false
    [void]$startInfo.EnvironmentVariables
    [void]$startInfo.EnvironmentVariables.Remove('Path')
    [void]$startInfo.EnvironmentVariables.Remove('PATH')
    $startInfo.EnvironmentVariables.Add('PATH', (Split-Path -Parent $ninja) + ';' + [System.Environment]::GetEnvironmentVariable('Path', 'Process'))
    $startInfo.FileName = "$env:SystemRoot\System32\cmd.exe"
    $startInfo.Arguments = '/d /c ' + $cmdCommand
    $startInfo.WorkingDirectory = $repoRoot
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    [void]$process.Start()
    $process.WaitForExit()
    if (Test-Path -LiteralPath $logPath) {
        Get-Content -LiteralPath $logPath
        Remove-Item -LiteralPath $logPath -Force
    }
    if ($process.ExitCode -ne 0) {
        throw "Command failed with exit code $($process.ExitCode): $CommandLine"
    }
}

Push-Location $repoRoot
try {
    # A previous failed configure may cache the OpenCV Windows-package directory.
    # Clear only that discovery cache entry so OPENCV_DIR is always authoritative.
    Invoke-VisualStudioCommand ('"' + $cmake + '" -UOpenCV_DIR --preset windows-x64-relwithdebinfo-diagnostics "-DPADDLE_LIB=' + $PaddleLib + '" "-DOPENCV_DIR=' + $OpenCvDir + '"')
    Invoke-VisualStudioCommand ('"' + $cmake + '" --build --preset windows-x64-relwithdebinfo-diagnostics-core')

    if ($BuildOnly) {
        Write-Host 'Diagnostics native build completed without modifying x64\\Debug.' -ForegroundColor Green
        exit 0
    }

    $nativeDirectory = Join-Path $repoRoot 'x64\RelWithDebInfo'
    $debugDirectory = Join-Path $repoRoot 'x64\Debug'
    if (-not (Test-Path -LiteralPath (Join-Path $debugDirectory 'IMao-WinUI.exe'))) {
        throw "Missing Debug WinUI output: $debugDirectory. Build the Debug|x64 solution once in Visual Studio first."
    }
    Copy-Item -LiteralPath (Join-Path $nativeDirectory 'IMao-Core.dll') -Destination $debugDirectory -Force
    $nativePdb = Join-Path $nativeDirectory 'IMao-Core.pdb'
    if (Test-Path -LiteralPath $nativePdb) {
        Copy-Item -LiteralPath $nativePdb -Destination $debugDirectory -Force
    }

    Write-Host "Diagnostics build installed: $debugDirectory" -ForegroundColor Green
    Write-Host "After running the Debug app, diagnostics will be in: $debugDirectory\Diagnostics" -ForegroundColor Green
}
finally {
    Pop-Location
}
