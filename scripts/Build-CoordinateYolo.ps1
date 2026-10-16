[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$DependencyRoot,
    [string]$BuildDirectory,
    [ValidateRange(1, 16)][int]$Parallel = 2
)
$ErrorActionPreference = 'Stop'
$sourceRoot = Split-Path -Parent $PSScriptRoot
$DependencyRoot = (Resolve-Path -LiteralPath $DependencyRoot).Path
. (Join-Path $DependencyRoot 'scripts/Enter-DevEnvironment.ps1')
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $sourceRoot 'out/build/coordinate-yolo' }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$cmake = & $vswhere -latest -products * -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' | Select-Object -First 1
$ninja = & $vswhere -latest -products * -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe' | Select-Object -First 1
$vcvars = & $vswhere -latest -products * -find 'VC\Auxiliary\Build\vcvars64.bat' | Select-Object -First 1
if (-not $cmake -or -not $ninja -or -not $vcvars) { throw 'MSVC, CMake and Ninja are required; no dependencies are downloaded.' }

function Invoke-OfflineBuild([string]$Command) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = "$env:SystemRoot\System32\cmd.exe"
    $start.Arguments = '/d /c call "' + $vcvars + '" >nul && ' + $Command
    $start.WorkingDirectory = $sourceRoot
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.EnvironmentVariables['VSLANG'] = '1033'
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    [void]$start.EnvironmentVariables.Remove('Path')
    [void]$start.EnvironmentVariables.Remove('PATH')
    $start.EnvironmentVariables.Add('PATH', (Split-Path -Parent $ninja) + ';' + $env:Path)
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    [void]$process.Start()
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    Write-Host $stdout.GetAwaiter().GetResult()
    Write-Host $stderr.GetAwaiter().GetResult()
    if ($process.ExitCode -ne 0) { throw "Offline build failed: exit $($process.ExitCode)" }
}

$nativeSource = Join-Path $sourceRoot 'IMao-Core/tools/CoordinateYolo'
Invoke-OfflineBuild ('"' + $cmake + '" -S "' + $nativeSource + '" -B "' + $BuildDirectory + '" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DIMAO_DEPENDENCY_ROOT=' + $DependencyRoot + '"')
Invoke-OfflineBuild ('"' + $cmake + '" --build "' + $BuildDirectory + '" --parallel ' + $Parallel)
Write-Host "Offline adapter: $(Join-Path $BuildDirectory 'IMaoCoordinateYolo.exe')"
