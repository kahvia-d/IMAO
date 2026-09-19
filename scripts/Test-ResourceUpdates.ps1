[CmdletBinding()]
param([string]$OutputDirectory = 'out\resource-updates-tests', [string]$StagedTree = 'out\map-test', [string]$DeselectRegion = 'tethys-kurotiles')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-DevEnvironment.ps1')
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskOutput = [IO.Path]::GetFullPath($OutputDirectory, $taskRoot)
New-Item -ItemType Directory -Force -Path $taskOutput | Out-Null
$taskInfo = [Diagnostics.ProcessStartInfo]::new()
$taskInfo.FileName = $env:IMAO_DOTNET
$taskInfo.UseShellExecute = $false
$taskInfo.CreateNoWindow = $true
$taskInfo.RedirectStandardOutput = $true
$taskInfo.RedirectStandardError = $true
$taskInfo.WorkingDirectory = $taskRoot
[void]$taskInfo.EnvironmentVariables.Remove('Path')
[void]$taskInfo.EnvironmentVariables.Remove('PATH')
$taskInfo.EnvironmentVariables['PATH'] = [Environment]::GetEnvironmentVariable('Path', 'Process')
foreach ($taskArgument in @('build', 'Tests/ResourceUpdates/ResourceUpdates.csproj', '-c', 'Release', '--output', (Join-Path $taskOutput 'bin'), '--source', $env:NUGET_PACKAGES, '-p:NuGetAudit=false')) { $taskInfo.ArgumentList.Add($taskArgument) }
$taskProcess = [Diagnostics.Process]::Start($taskInfo)
$taskStdout = $taskProcess.StandardOutput.ReadToEndAsync()
$taskStderr = $taskProcess.StandardError.ReadToEndAsync()
$taskProcess.WaitForExit()
$taskText = $taskStdout.GetAwaiter().GetResult() + $taskStderr.GetAwaiter().GetResult()
[IO.File]::WriteAllText((Join-Path $taskOutput 'build.log'), $taskText)
Write-Host $taskText
if ($taskProcess.ExitCode -ne 0) { throw 'Resource update test build failed.' }
$taskInfo.ArgumentList.Clear()
$taskInfo.ArgumentList.Add((Join-Path $taskOutput 'bin/ResourceUpdates.dll'))
$taskInfo.ArgumentList.Add($taskOutput)
$taskProcess = [Diagnostics.Process]::Start($taskInfo)
$taskStdout = $taskProcess.StandardOutput.ReadToEndAsync()
$taskStderr = $taskProcess.StandardError.ReadToEndAsync()
$taskProcess.WaitForExit()
$taskText = $taskStdout.GetAwaiter().GetResult() + $taskStderr.GetAwaiter().GetResult()
[IO.File]::WriteAllText((Join-Path $taskOutput 'tests.log'), $taskText)
Write-Host $taskText
if ($taskProcess.ExitCode -ne 0) { throw 'Resource update tests failed.' }
$taskInfo.ArgumentList.Clear()
# The synthetic suite above cannot judge whether the real native loader accepts a filtered snapshot, so
# run that against a staged tree when one is present. This is the check that proves deselection works.
$taskStagedTree = [IO.Path]::GetFullPath($StagedTree, $taskRoot)
if (Test-Path -LiteralPath (Join-Path $taskStagedTree 'IMao-CoreHost.exe')) {
    $taskRegionOutput = Join-Path $taskOutput 'region-selection'
    $taskInfo.ArgumentList.Add((Join-Path $taskOutput 'bin/ResourceUpdates.dll'))
    $taskInfo.ArgumentList.Add('region-selection')
    $taskInfo.ArgumentList.Add($taskStagedTree)
    $taskInfo.ArgumentList.Add($DeselectRegion)
    $taskInfo.ArgumentList.Add($taskRegionOutput)
    $taskProcess = [Diagnostics.Process]::Start($taskInfo)
    $taskStdout = $taskProcess.StandardOutput.ReadToEndAsync()
    $taskStderr = $taskProcess.StandardError.ReadToEndAsync()
    $taskProcess.WaitForExit()
    $taskText = $taskStdout.GetAwaiter().GetResult() + $taskStderr.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $taskOutput 'region-selection.log'), $taskText)
    Write-Host $taskText
    if ($taskProcess.ExitCode -ne 0) { throw 'Native region selection check failed.' }
} else {
    Write-Host "Skipping native region selection check: no staged tree at $taskStagedTree"
}

$taskInfo.ArgumentList.Clear()
foreach ($taskArgument in @('build', 'tools/UpdatePublisher/UpdatePublisher.csproj', '-c', 'Release', '--output', (Join-Path $taskOutput 'publisher-bin'), '--source', $env:NUGET_PACKAGES, '-p:NuGetAudit=false')) { $taskInfo.ArgumentList.Add($taskArgument) }
$taskProcess = [Diagnostics.Process]::Start($taskInfo)
$taskStdout = $taskProcess.StandardOutput.ReadToEndAsync()
$taskStderr = $taskProcess.StandardError.ReadToEndAsync()
$taskProcess.WaitForExit()
$taskText = $taskStdout.GetAwaiter().GetResult() + $taskStderr.GetAwaiter().GetResult()
[IO.File]::WriteAllText((Join-Path $taskOutput 'publisher-build.log'), $taskText)
Write-Host $taskText
if ($taskProcess.ExitCode -ne 0) { throw 'Publisher test build failed.' }
$taskPublisherOutput = Join-Path $taskOutput ('publisher-' + [Guid]::NewGuid().ToString('N'))
$taskInfo.ArgumentList.Clear()
foreach ($taskArgument in @((Join-Path $taskOutput 'publisher-bin/UpdatePublisher.dll'), 'self-test', '--output', $taskPublisherOutput)) { $taskInfo.ArgumentList.Add($taskArgument) }
$taskProcess = [Diagnostics.Process]::Start($taskInfo)
$taskStdout = $taskProcess.StandardOutput.ReadToEndAsync()
$taskStderr = $taskProcess.StandardError.ReadToEndAsync()
$taskProcess.WaitForExit()
$taskText = $taskStdout.GetAwaiter().GetResult() + $taskStderr.GetAwaiter().GetResult()
[IO.File]::WriteAllText((Join-Path $taskOutput 'publisher-tests.log'), $taskText)
Write-Host $taskText
if ($taskProcess.ExitCode -ne 0) { throw 'Publisher tests failed.' }
