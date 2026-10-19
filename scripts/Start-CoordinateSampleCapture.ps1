[CmdletBinding()]
param(
    [string]$ToolRoot = $PSScriptRoot,
    [string]$OutputRoot,
    [ValidateRange(250, 5000)][int]$IntervalMilliseconds = 500,
    [ValidateRange(10, 3600)][int]$MaxSeconds = 600,
    [ValidateRange(1, 10000)][int]$MaxSamples = 1200,
    [ValidateRange(1, 2048)][int]$MaxMiB = 256,
    [switch]$Stop
)
$ErrorActionPreference = 'Stop'
$ToolRoot = (Resolve-Path -LiteralPath $ToolRoot).Path
$executable = Join-Path $ToolRoot 'IMaoCoordinateSampleCapture.exe'
$statePath = Join-Path $ToolRoot 'capture-process.json'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw 'Capture package is missing. Specify -ToolRoot with the built package directory.' }
function Read-CaptureLog([string]$Path) {
    if ($Path -and (Test-Path -LiteralPath $Path -PathType Leaf)) { return Get-Content -LiteralPath $Path -Raw -Encoding UTF8 }
    return ''
}
function Complete-Capture($State, $ExitCode) {
    $diagnostics = Read-CaptureLog $State.stderr
    $output = Read-CaptureLog $State.stdout
    if (($null -ne $ExitCode -and $ExitCode -ne 0) -or -not $output -or -not $output.Contains('Finished:')) {
        throw "Capture did not finish successfully (exit $ExitCode). $diagnostics Inspect $($State.stdout)"
    }
    if ($diagnostics) { Write-Warning $diagnostics }
    Write-Host "Capture finished. Samples: $($State.outputRoot)"
}
function Quote-WindowsArgument([string]$Value) {
    # CRT parsing: double backslashes before a quote and before the final quote.
    $escaped = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
    return '"' + $escaped + '"'
}
if (Test-Path -LiteralPath $statePath) {
    $state = Get-Content -LiteralPath $statePath -Raw -Encoding UTF8 | ConvertFrom-Json
    $process = Get-Process -Id $state.processId -ErrorAction SilentlyContinue
    $matches = $process -and $process.Path -and
        [IO.Path]::GetFullPath($process.Path) -eq [IO.Path]::GetFullPath($executable) -and
        $process.StartTime.ToUniversalTime().ToString('o') -eq $state.startedAtUtc
    if ($matches) {
        if (-not $Stop) { throw "Capture tool is already running (PID $($state.processId)). Ctrl+Alt+F8 starts/pauses; Ctrl+Alt+F7 finishes." }
        [void]$process.Handle # Retain access to the exit code before it exits.
        $event = [Threading.EventWaitHandle]::OpenExisting('Local\IMaoCoordinateSamplesStop-' + $state.processId)
        try { [void]$event.Set() } finally { $event.Dispose() }
        if (-not $process.WaitForExit(5000)) { throw 'Capture has not finished yet; no process was force-killed.' }
        Complete-Capture $state $process.ExitCode
        exit 0
    }
    if ($Stop) { Complete-Capture $state $null; exit 0 }
}
if ($Stop) { Write-Host 'Capture tool is not running.'; exit 0 }
if (-not $OutputRoot) { $OutputRoot = Join-Path (Split-Path -Parent $ToolRoot) 'coordinate-samples' }
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
$logPrefix = Join-Path $OutputRoot ('launcher-' + [guid]::NewGuid().ToString('N'))
$stdout = $logPrefix + '.stdout.log'
$stderr = $logPrefix + '.stderr.log'
$arguments = '--output ' + (Quote-WindowsArgument $OutputRoot) + ' --interval-ms ' + $IntervalMilliseconds +
    ' --max-seconds ' + $MaxSeconds + ' --max-samples ' + $MaxSamples + ' --max-mib ' + $MaxMiB
$process = Start-Process -FilePath $executable -ArgumentList $arguments -WorkingDirectory $ToolRoot `
    -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
[ordered]@{processId=$process.Id; startedAtUtc=$process.StartTime.ToUniversalTime().ToString('o');
    outputRoot=$OutputRoot; stdout=$stdout; stderr=$stderr} | ConvertTo-Json |
    Set-Content -LiteralPath $statePath -Encoding UTF8
$ready = $false
for ($attempt = 0; $attempt -lt 50; ++$attempt) {
    if ($process.HasExited) { throw "Capture startup failed (exit $($process.ExitCode)). $(Get-Content -LiteralPath $stderr -Raw -Encoding UTF8)" }
    if (Test-Path -LiteralPath $stdout) {
        $text = Get-Content -LiteralPath $stdout -Raw -Encoding UTF8
        if ($text -and $text.Contains('Ready (paused).')) { $ready = $true; break }
    }
    Start-Sleep -Milliseconds 100
}
if (-not $ready) {
    throw "Capture has not reported ready. Inspect $stderr and finish with Ctrl+Alt+F7 if it is still running."
}
Write-Host 'Ready, PAUSED. In the game: Ctrl+Alt+F8 starts/pauses; Ctrl+Alt+F7 finishes.'
Write-Host "Samples: $OutputRoot"
Write-Host "Diagnostics: $stderr"
