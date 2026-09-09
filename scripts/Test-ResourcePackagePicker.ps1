[CmdletBinding()]
param(
    [string]$OutputDirectory = 'out\resource-picker-tests',
    [switch]$AbiOnly,
    [string]$PickerSource,
    [string]$AssemblyPath
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-DevEnvironment.ps1')
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskOutput = [IO.Path]::GetFullPath($OutputDirectory, $taskRoot)
New-Item -ItemType Directory -Force -Path $taskOutput | Out-Null
$taskInfo = [Diagnostics.ProcessStartInfo]::new()
$taskInfo.FileName = $env:IMAO_DOTNET
$taskInfo.UseShellExecute = $false
$taskInfo.CreateNoWindow = $true
$taskInfo.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
$taskInfo.RedirectStandardOutput = $true
$taskInfo.RedirectStandardError = $true
$taskInfo.StandardOutputEncoding = [Text.Encoding]::UTF8
$taskInfo.StandardErrorEncoding = [Text.Encoding]::UTF8
$taskInfo.WorkingDirectory = $taskRoot
[void]$taskInfo.EnvironmentVariables.Remove('Path')
[void]$taskInfo.EnvironmentVariables.Remove('PATH')
$taskInfo.EnvironmentVariables['PATH'] = [Environment]::GetEnvironmentVariable('Path', 'Process')
foreach ($taskArgument in @('build', 'Tests/ResourcePackagePicker/ResourcePackagePicker.csproj', '-c', 'Release', '--output', (Join-Path $taskOutput 'bin'), '--source', $env:NUGET_PACKAGES, '-p:NuGetAudit=false')) { $taskInfo.ArgumentList.Add($taskArgument) }
if ($PickerSource) { $taskInfo.ArgumentList.Add('-p:ResourcePackagePickerSource=' + [IO.Path]::GetFullPath($PickerSource, $taskRoot)) }
$taskProcess = [Diagnostics.Process]::Start($taskInfo)
$taskStdout = $taskProcess.StandardOutput.ReadToEndAsync()
$taskStderr = $taskProcess.StandardError.ReadToEndAsync()
$taskProcess.WaitForExit()
$taskText = $taskStdout.GetAwaiter().GetResult() + $taskStderr.GetAwaiter().GetResult()
[IO.File]::WriteAllText((Join-Path $taskOutput 'build.log'), $taskText)
Write-Host $taskText
if ($taskProcess.ExitCode -ne 0) { throw 'Resource package picker test build failed.' }
$taskInfo.ArgumentList.Clear()
$taskInfo.ArgumentList.Add((Join-Path $taskOutput 'bin/ResourcePackagePicker.dll'))
$taskInfo.ArgumentList.Add($taskOutput)
if ($AbiOnly) { $taskInfo.ArgumentList.Add('--abi-only') }
if ($AssemblyPath) {
    $taskInfo.ArgumentList.Add('--assembly')
    $taskInfo.ArgumentList.Add([IO.Path]::GetFullPath($AssemblyPath, $taskRoot))
}
$taskProcess = [Diagnostics.Process]::Start($taskInfo)
$taskStdout = $taskProcess.StandardOutput.ReadToEndAsync()
$taskStderr = $taskProcess.StandardError.ReadToEndAsync()
$taskTimedOut = -not $taskProcess.WaitForExit(60000)
if ($taskTimedOut) {
    # Only this dedicated test child process is terminated; never enumerate or stop the user's application.
    $taskProcess.Kill($true)
    $taskProcess.WaitForExit()
}
$taskText = $taskStdout.GetAwaiter().GetResult() + $taskStderr.GetAwaiter().GetResult()
[IO.File]::WriteAllText((Join-Path $taskOutput 'tests.log'), $taskText)
Write-Host $taskText
if ($taskTimedOut) { throw 'Resource package picker test timed out; its dedicated process was stopped. See tests.log.' }
if ($taskProcess.ExitCode -ne 0) { throw 'Resource package picker tests failed.' }
