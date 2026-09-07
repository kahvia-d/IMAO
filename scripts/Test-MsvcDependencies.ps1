[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-DevEnvironment.ps1')
$taskRepo = Split-Path -Parent $PSScriptRoot
$taskProbe = Join-Path $taskRepo 'out\system-audit\dependency-probe'
New-Item -ItemType Directory -Force -Path $taskProbe | Out-Null
$taskVswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$taskCmake = & $taskVswhere -latest -products * -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' | Select-Object -First 1
$taskNinja = & $taskVswhere -latest -products * -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe' | Select-Object -First 1
$taskVcvars = & $taskVswhere -latest -products * -find 'VC\Auxiliary\Build\vcvars64.bat' | Select-Object -First 1
$taskAdapter = (Join-Path $taskRepo 'cmake/DetectMsvcIncludes.cmake').Replace('\', '/')
$taskProject = @"
cmake_minimum_required(VERSION 3.14)
project(DependencyProbe CXX)
include("$taskAdapter")
add_executable(probe_utf8 main.cpp)
target_compile_options(probe_utf8 PRIVATE /source-charset:utf-8 /execution-charset:utf-8)
add_executable(probe_legacy main.cpp)
"@
[IO.File]::WriteAllText((Join-Path $taskProbe 'CMakeLists.txt'), $taskProject)
[IO.File]::WriteAllText((Join-Path $taskProbe 'main.cpp'), "#include `"probe.h`"`nint main(){return value;}`n")
[IO.File]::WriteAllText((Join-Path $taskProbe 'probe.h'), "constexpr int value = 0;`n")
function Invoke-ProbeCommand([string]$Command, [string]$LogName) {
    $log = Join-Path $taskProbe $LogName
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = "$env:SystemRoot\System32\cmd.exe"
    $info.Arguments = '/d /c call "' + $taskVcvars + '" >nul && (' + $Command + ') > "' + $log + '" 2>&1'
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WorkingDirectory = $taskProbe
    [void]$info.EnvironmentVariables.Remove('Path')
    [void]$info.EnvironmentVariables.Remove('PATH')
    $info.EnvironmentVariables['PATH'] = [Environment]::GetEnvironmentVariable('Path', 'Process')
    $process = [Diagnostics.Process]::Start($info)
    $process.WaitForExit()
    $code = $process.ExitCode
    $process.Dispose()
    if ($code -ne 0) { throw "Dependency probe failed: $log ($code)" }
}
Invoke-ProbeCommand ('"' + $taskCmake + '" -S . -B build -G Ninja -DCMAKE_MAKE_PROGRAM="' + $taskNinja + '"') 'configure.log'
Invoke-ProbeCommand ('"' + $taskCmake + '" --build build --parallel 2') 'build.log'
Invoke-ProbeCommand ('"' + $taskNinja + '" -C build -t deps') 'deps.log'
$taskDeps = Get-Content (Join-Path $taskProbe 'deps.log') -Raw
if (($taskDeps | Select-String -Pattern 'probe\.h' -AllMatches).Matches.Count -ne 2) {
    throw 'Both UTF-8 and legacy targets must record the header dependency.'
}
[IO.File]::WriteAllText((Join-Path $taskProbe 'probe.h'), "constexpr int value = 1;`n")
Invoke-ProbeCommand ('"' + $taskNinja + '" -C build -n') 'rebuild.log'
$taskRebuild = Get-Content (Join-Path $taskProbe 'rebuild.log') -Raw
if (($taskRebuild | Select-String -Pattern 'Building CXX object' -AllMatches).Matches.Count -ne 2) {
    throw 'Changing a header must schedule both targets for recompilation.'
}
Write-Host "MSVC dependency tracking passed for UTF-8 and legacy diagnostics: $taskProbe" -ForegroundColor Green
