[CmdletBinding()]
param([Parameter(Mandatory)][string]$DependencyRoot, [switch]$SelectionTestsOnly)
$ErrorActionPreference = 'Stop'
$probeRoot = Split-Path -Parent $PSScriptRoot
$DependencyRoot = (Resolve-Path -LiteralPath $DependencyRoot).Path
. (Join-Path $DependencyRoot 'scripts/Enter-DevEnvironment.ps1')
$probeBuild = Join-Path $probeRoot 'out/build/cloud-capture-probe'
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$cmake = & $vswhere -latest -products * -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' | Select-Object -First 1
$ninja = & $vswhere -latest -products * -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe' | Select-Object -First 1
$vcvars = & $vswhere -latest -products * -find 'VC\Auxiliary\Build\vcvars64.bat' | Select-Object -First 1
if (-not $cmake -or -not $ninja -or -not $vcvars) { throw 'MSVC, CMake and Ninja are required. No downloads performed.' }
function Invoke-ProbeBuild([string]$Command) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = "$env:SystemRoot\System32\cmd.exe"
    $start.Arguments = '/d /c call "' + $vcvars + '" >nul && ' + $Command
    $start.WorkingDirectory = $probeRoot
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    [void]$start.EnvironmentVariables.Remove('Path')
    [void]$start.EnvironmentVariables.Remove('PATH')
    $start.EnvironmentVariables.Add('PATH', (Split-Path -Parent $ninja) + ';' + $env:Path)
    $process = [Diagnostics.Process]::Start($start)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    Write-Output $stdout.GetAwaiter().GetResult()
    Write-Output $stderr.GetAwaiter().GetResult()
    if ($process.ExitCode -ne 0) { throw "Probe build failed: exit $($process.ExitCode)" }
}
Invoke-ProbeBuild ('"' + $cmake + '" -S "' + (Join-Path $probeRoot 'IMao-Core/tools/CloudCaptureProbe') + '" -B "' + $probeBuild + '" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DIMAO_DEPENDENCY_ROOT=' + $DependencyRoot + '"')
$targets = if ($SelectionTestsOnly) { ' --target IMaoCloudSelectionTests' } else { '' }
Invoke-ProbeBuild ('"' + $cmake + '" --build "' + $probeBuild + '" --parallel 2' + $targets)
& (Join-Path $probeBuild 'IMaoCloudSelectionTests.exe')
if ($LASTEXITCODE -ne 0) { throw 'Cloud selection tests failed.' }
Write-Host "Probe build: $probeBuild"
