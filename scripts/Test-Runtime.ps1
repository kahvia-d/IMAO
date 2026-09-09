[CmdletBinding()]
param([switch]$SkipBuild, [switch]$SkipHost, [string]$OutputDirectory,
    [string]$NativeBuildDirectory = 'out\build\windows-x64-release',
    [ValidateRange(1, 64)][int]$Parallel = 4)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-DevEnvironment.ps1')
$taskRepo = Split-Path -Parent $PSScriptRoot
$taskOutput = if ([string]::IsNullOrWhiteSpace($OutputDirectory)) { Join-Path $taskRepo 'out\system-audit' } else { [IO.Path]::GetFullPath($OutputDirectory, $taskRepo) }
$taskManagedOutput = Join-Path $taskOutput 'managed-runtime'
New-Item -ItemType Directory -Force -Path $taskOutput | Out-Null
$taskVswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$taskCmake = & $taskVswhere -latest -products * -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' | Select-Object -First 1
$taskVcvars = & $taskVswhere -latest -products * -find 'VC\Auxiliary\Build\vcvars64.bat' | Select-Object -First 1
function Invoke-TestCommand([string]$Command, [string]$LogName) {
    $log = Join-Path $taskOutput $LogName
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = "$env:SystemRoot\System32\cmd.exe"
    $info.Arguments = '/d /c call "' + $taskVcvars + '" >nul && (' + $Command + ') > "' + $log + '" 2>&1'
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WorkingDirectory = $taskRepo
    [void]$info.EnvironmentVariables.Remove('Path')
    [void]$info.EnvironmentVariables.Remove('PATH')
    $info.EnvironmentVariables['PATH'] = [Environment]::GetEnvironmentVariable('Path', 'Process')
    $info.EnvironmentVariables['LOCALAPPDATA'] = Join-Path $taskOutput 'local-app-data'
    $info.EnvironmentVariables['CMAKE_BUILD_PARALLEL_LEVEL'] = [string]$Parallel
    $process = [Diagnostics.Process]::Start($info)
    $process.WaitForExit()
    $code = $process.ExitCode
    $process.Dispose()
    Get-Content -LiteralPath $log -Tail 8
    if ($code -ne 0) { throw "$LogName failed with exit code $code; see $log" }
}
if (-not $SkipBuild) {
    Invoke-TestCommand ('"' + $taskCmake + '" --build "' + $NativeBuildDirectory + '" --config Release --target IMao-CoreHost IMaoOptimizationTests IMaoMarkerTests IMaoRoutePlanningTests IMaoRoutePlanningServiceTests IMaoVisualRegression IMaoResourceSnapshotTests --parallel ' + $Parallel) 'native-build.log'
    Invoke-TestCommand ('"' + $env:IMAO_DOTNET + '" build Tests\ManagedRuntime\ManagedRuntime.csproj -c Release --output "' + $taskManagedOutput + '" --source "' + $env:NUGET_PACKAGES + '" -p:NuGetAudit=false') 'managed-build.log'
}
Invoke-TestCommand 'x64\Release\IMaoOptimizationTests.exe' 'native-tests.log'
Invoke-TestCommand 'x64\Release\IMaoMarkerTests.exe' 'marker-tests.log'
Invoke-TestCommand 'x64\Release\IMaoResourceSnapshotTests.exe' 'resource-snapshot-tests.log'
if (Test-Path -LiteralPath (Join-Path $taskRepo 'out\auto-replan-native\IMaoRoutePlanningServiceTests.exe')) {
    Invoke-TestCommand ('out\auto-replan-native\IMaoRoutePlanningServiceTests.exe "' + (Join-Path $taskOutput 'route-service-data') + '"') 'route-service-tests.log'
    Invoke-TestCommand ('out\auto-replan-native\IMaoRoutePlanningServiceTests.exe "' + (Join-Path $taskOutput 'route-service-failure-data') + '" save-failure') 'route-service-failure-tests.log'
}
if (-not $SkipBuild -or (Test-Path -LiteralPath (Join-Path $taskRepo 'x64\Release\IMaoRoutePlanningTests.exe'))) {
    Invoke-TestCommand 'x64\Release\IMaoRoutePlanningTests.exe' 'route-planning-tests.log'
} else {
    Write-Host 'Skipped route planning tests: -SkipBuild used and IMaoRoutePlanningTests.exe is not built. Build preset windows-x64-release-route-planning-tests to enable this check.' -ForegroundColor Yellow
}
$taskHostArgument = if ($SkipHost) { '' } else { ' "' + (Join-Path $taskRepo 'x64\Release') + '"' }
Invoke-TestCommand ('"' + $env:IMAO_DOTNET + '" "' + (Join-Path $taskManagedOutput 'ManagedRuntime.dll') + '"' + $taskHostArgument) 'managed-tests.log'
& (Join-Path $PSScriptRoot 'Test-ResourceUpdates.ps1') -OutputDirectory (Join-Path $taskOutput 'resource-updates')
& (Join-Path $PSScriptRoot 'Test-ProgramUpdates.ps1') -OutputDirectory (Join-Path $taskOutput 'program-updates')
& (Join-Path $PSScriptRoot 'Test-ResourcePackagePicker.ps1') -OutputDirectory (Join-Path $taskOutput 'resource-package-picker')
& (Join-Path $PSScriptRoot 'Test-ResourceBuildProvenance.ps1') -OutputRoot (Join-Path $taskOutput ('resource-provenance-' + [guid]::NewGuid().ToString('N')))
& (Join-Path $PSScriptRoot 'Test-ResourceUpdateStaging.ps1') -OutputRoot (Join-Path $taskOutput ('resource-staging-' + [guid]::NewGuid().ToString('N')))
& (Join-Path $PSScriptRoot 'Test-ResourceCatalogTransition.ps1') -OutputRoot (Join-Path $taskOutput ('resource-catalog-' + [guid]::NewGuid().ToString('N')))
Write-Host "Runtime tests passed. Evidence: $taskOutput" -ForegroundColor Green
# Negative-path subprocess tests intentionally return nonzero before asserting rejection.
# Do not leak their expected exit status to automation after all suites have passed.
$global:LASTEXITCODE = 0
