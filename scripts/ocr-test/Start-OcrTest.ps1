[CmdletBinding()]
param(
    [ValidateSet('cpu', 'directml')][string]$Backend = 'cpu',
    [ValidateRange(0, 1000000)][int]$Adapter = 0,
    [switch]$Elevated
)
$ErrorActionPreference = 'Stop'

function Get-OcrElevationArguments {
    param([string]$ScriptPath, [string]$Backend, [int]$Adapter)
    # UAC can discard the caller's environment. Carry choices as literal arguments;
    # encoding also preserves spaces, apostrophes and non-ASCII paths in Windows PowerShell.
    $literalPath = $ScriptPath.Replace("'", "''")
    $command = "& '$literalPath' -Backend '$Backend' -Adapter $Adapter -Elevated"
    return '-NoProfile -ExecutionPolicy Bypass -EncodedCommand ' +
        [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
}

if ($MyInvocation.InvocationName -eq '.') { return }
try {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $administrator = ([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $administrator) {
        if ($Elevated) { throw 'Administrator access was not granted.' }
        $arguments = Get-OcrElevationArguments -ScriptPath $PSCommandPath -Backend $Backend -Adapter $Adapter
        $child = Start-Process -FilePath "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -Verb RunAs -ArgumentList $arguments -Wait -PassThru
        exit $child.ExitCode
    }

    # Refuse mode changes while any edition is running. Never stop a player process.
    $running = @(Get-Process -Name 'IMao-WinUI', 'IMao-CoreHost', 'IMao-Launcher' -ErrorAction SilentlyContinue)
    if ($running.Count -gt 0) { throw 'Please fully exit IMao (including the tray icon) before switching OCR mode.' }
    $launcher = Join-Path $PSScriptRoot 'IMao-Launcher.exe'
    if (-not (Test-Path -LiteralPath $launcher -PathType Leaf)) {
        throw 'Place Start-OcrTest.ps1 and both CMD files beside IMao-Launcher.exe in the extracted test package.'
    }
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $launcher
    $start.WorkingDirectory = $PSScriptRoot
    $start.UseShellExecute = $false
    $start.EnvironmentVariables['IMAO_OCR_BACKEND'] = $Backend
    $start.EnvironmentVariables['IMAO_OCR_ADAPTER'] = [string]$Adapter
    # The already-elevated launcher passes these process-local choices to UI/CoreHost.
    $process = [Diagnostics.Process]::Start($start)
    if ($null -eq $process) { throw 'IMao launcher did not start.' }
    $process.Dispose()
    Write-Host "Requested OCR: $Backend; adapter: $Adapter. Check Diagnostics for the actual backend."
    exit 0
} catch {
    Write-Host ('OCR test launch failed or elevation was cancelled: ' + $_.Exception.Message) -ForegroundColor Red
    exit 1
}
