# Runs the overlay present-path probe against the live game and records one PresentMon trace across
# three phases: no overlay, the DirectComposition surface, and the WS_EX_LAYERED colorkey window the
# overlay uses today. All three phases draw the same visible block, so the comparison is between window
# types rather than between "something on screen" and "nothing on screen".
#
# PresentMon needs its own ETW session, so this has to run elevated. Keep the game in the foreground
# for the whole run: the probe never activates itself, but nothing else may steal focus either.
#
# Usage:  .\Test-OverlayPresentPath.ps1
#         .\Test-OverlayPresentPath.ps1 -PhaseSeconds 60 -PresentHz 0   # display-rate maximum
#         .\Test-OverlayPresentPath.ps1 -SkipLayered
[CmdletBinding()]
param(
    [string]$ProcessName = 'Client-Win64-Shipping.exe',
    [int]$PhaseSeconds = 45,
    [int]$ProbeHoldSeconds = 0,
    [int]$PresentHz = 30,
    [string]$OutputPath,
    [string]$PresentMonPath,
    [switch]$SkipLayered,
    [switch]$SkipDcomp
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputPath)) { $OutputPath = Join-Path $repoRoot 'out\perf\overlay-present-path.csv' }
if ([string]::IsNullOrWhiteSpace($PresentMonPath)) { $PresentMonPath = Join-Path $repoRoot 'out\perf\PresentMon-2.5.1.exe' }
$probe = Join-Path $repoRoot 'x64\Release\IMaoOverlayPresentProbe.exe'

if (-not (Test-Path -LiteralPath $PresentMonPath)) { throw "PresentMon not found: $PresentMonPath" }
if (-not (Test-Path -LiteralPath $probe)) { throw "Probe not built: $probe. Build the IMaoOverlayPresentProbe target first." }
if (-not (Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($ProcessName)) -ErrorAction SilentlyContinue)) {
    throw "The game ($ProcessName) is not running. Start it and leave it in the foreground."
}
# A leftover session makes the next capture fail silently, which is how an earlier trace produced an
# empty CSV.
Get-Process -Name 'PresentMon-2.5.1' -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2

# The probe holds itself for this long; the capture runs a little past it so every phase has the same
# recorded length even if a launch is slow.
if ($ProbeHoldSeconds -le 0) { $ProbeHoldSeconds = $PhaseSeconds }
$totalSeconds = $PhaseSeconds * 3 + 20

$phaseLog = [Collections.Generic.List[string]]::new()
$record = {
    param([string]$Line)
    $stamp = (Get-Date).ToString('HH:mm:ss')
    $text = "$stamp  $Line"
    $phaseLog.Add($text)
    Write-Host $text -ForegroundColor Cyan
}

& $record "phases: none -> dcomp -> layered, about $PhaseSeconds s each, presentHz=$PresentHz"
& $record "trace:  $OutputPath"
& $record ("-" * 60)

$capture = Start-Process -FilePath $PresentMonPath -ArgumentList @(
    '--process_name', $ProcessName,
    '--output_file', $OutputPath,
    '--timed', $totalSeconds,
    '--no_console_stats',
    '--date_time',
    '--terminate_after_timed',
    '--stop_existing_session'
) -PassThru -WindowStyle Hidden

Start-Sleep -Seconds 3

$phases = @([pscustomobject]@{ name = 'none'; mode = 'none' })
if (-not $SkipDcomp) { $phases += [pscustomobject]@{ name = 'dcomp'; mode = 'dcomp' } }
if (-not $SkipLayered) { $phases += [pscustomobject]@{ name = 'layered'; mode = 'layered' } }

try {
    foreach ($phase in $phases) {
        & $record "PHASE START $($phase.name) (mode=$($phase.mode))"
        if ($phase.mode -eq 'none') {
            Start-Sleep -Seconds $PhaseSeconds
        }
        else {
            # The probe holds the window itself, so the phase length is enforced in one place.
            & $probe "--mode=$($phase.mode)" "--hold=$ProbeHoldSeconds" "--hz=$PresentHz" | ForEach-Object { if ($_) { Write-Host "    probe: $_" } }
        }
        & $record "PHASE END   $($phase.name)"
        Start-Sleep -Seconds 2
    }
}
finally {
    if (-not $capture.HasExited) { $capture.WaitForExit(($totalSeconds + 30) * 1000) | Out-Null }
}

$logPath = [IO.Path]::ChangeExtension($OutputPath, '.phases.txt')
$phaseLog | Set-Content -LiteralPath $logPath -Encoding utf8
& $record "phase marks written to $logPath"

if (-not (Test-Path -LiteralPath $OutputPath) -or (Get-Item -LiteralPath $OutputPath).Length -eq 0) {
    throw "PresentMon produced no frames. Re-run elevated and confirm no other ETW session holds PresentMon."
}

# PresentMon's MsBetweenPresents is the number the whole comparison rests on, so it is summarised here
# rather than left to be read out of a multi-megabyte CSV by hand.
$rows = Import-Csv -LiteralPath $OutputPath
if ($rows.Count -eq 0) { throw 'PresentMon recorded frames but the CSV has no rows.' }
& $record ("captured {0} frames" -f $rows.Count)
$summary = $rows | Group-Object -Property PresentMode | ForEach-Object {
    [pscustomobject]@{ PresentMode = $_.Name; Frames = $_.Count; Percent = [math]::Round(100.0 * $_.Count / $rows.Count, 1) }
} | Sort-Object Frames -Descending
$summary | Format-Table -AutoSize | Out-String | Write-Host
$summaryPath = [IO.Path]::ChangeExtension($OutputPath, '.presentmode.txt')
$summary | Format-Table -AutoSize | Out-String | Set-Content -LiteralPath $summaryPath -Encoding utf8
& $record "present-mode summary written to $summaryPath"
Write-Host ''
Write-Host 'Split the trace by the timestamps in the phase marks and compare dcomp against layered.' -ForegroundColor Yellow
