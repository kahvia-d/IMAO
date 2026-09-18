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
    [switch]$SkipDcomp,
    [switch]$IncludePlain,
    [switch]$SkipAnalysis
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'OverlayTrace.Common.ps1')

$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputPath)) { $OutputPath = Join-Path $repoRoot 'out\perf\overlay-present-path.csv' }
if ([string]::IsNullOrWhiteSpace($PresentMonPath)) { $PresentMonPath = Join-Path $repoRoot 'out\perf\PresentMon-2.5.1.exe' }
$probe = Join-Path $repoRoot 'x64\Release\IMaoOverlayPresentProbe.exe'

Assert-FrameTraceElevated
if (-not (Test-Path -LiteralPath $PresentMonPath)) { throw "PresentMon not found: $PresentMonPath" }
if (-not (Test-Path -LiteralPath $probe)) { throw "Probe not built: $probe. Build the IMaoOverlayPresentProbe target first." }
if (-not (Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($ProcessName)) -ErrorAction SilentlyContinue)) {
    throw "The game ($ProcessName) is not running. Start it and leave it in the foreground."
}

# The probe holds itself for this long; the capture runs a little past it so every phase has the same
# recorded length even if a launch is slow.
if ($ProbeHoldSeconds -le 0) { $ProbeHoldSeconds = $PhaseSeconds }
$totalSeconds = $PhaseSeconds * 3 + 30

$phaseLog = [Collections.Generic.List[string]]::new()
$record = {
    param([string]$Line)
    $stamp = (Get-Date).ToString('HH:mm:ss')
    $text = "$stamp  $Line"
    $phaseLog.Add($text)
    Write-Host $text -ForegroundColor Cyan
}

& $record "phases: none -> dcomp -> layered, about $PhaseSeconds s each, presentHz=$PresentHz"
& $record "on screen: phase 1 shows NOTHING (that is the baseline); dcomp is RED; layered is GREEN"
& $record "trace:  $OutputPath"
& $record ('-' * 60)

$captureRequestedAt = Get-Date
$capture = Start-FrameTrace -PresentMonPath $PresentMonPath -ProcessName $ProcessName `
    -OutputPath $OutputPath -Seconds $totalSeconds

Start-Sleep -Seconds 3

$phases = @([pscustomobject]@{ name = 'none'; mode = 'none'; note = 'no window at all' })
if (-not $SkipDcomp) { $phases += [pscustomobject]@{ name = 'dcomp'; mode = 'dcomp'; note = 'RED block' } }
if (-not $SkipLayered) { $phases += [pscustomobject]@{ name = 'layered'; mode = 'layered'; note = 'GREEN block' } }
if ($IncludePlain) { $phases += [pscustomobject]@{ name = 'plain'; mode = 'plain'; note = 'BLUE block' } }

$marks = [Collections.Generic.List[object]]::new()
try {
    foreach ($phase in $phases) {
        $start = Get-Date
        & $record "PHASE START $($phase.name) (mode=$($phase.mode), $($phase.note))"
        if ($phase.mode -eq 'none') {
            Start-Sleep -Seconds $PhaseSeconds
        }
        else {
            # The probe holds the window itself, so the phase length is enforced in one place.
            & $probe "--mode=$($phase.mode)" "--hold=$ProbeHoldSeconds" "--hz=$PresentHz" |
                ForEach-Object { if ($_) { Write-Host "    probe: $_" } }
        }
        $end = Get-Date
        & $record "PHASE END   $($phase.name)"
        $marks.Add([pscustomobject]@{ Name = $phase.name; Start = $start; End = $end })
        Start-Sleep -Seconds 2
    }
}
finally {
    if (-not $capture.HasExited) { $capture.WaitForExit(($totalSeconds + 60) * 1000) | Out-Null }
}

$logPath = [IO.Path]::ChangeExtension($OutputPath, '.phases.txt')
$phaseLog | Set-Content -LiteralPath $logPath -Encoding utf8
& $record "phase marks written to $logPath"

if (-not (Test-Path -LiteralPath $OutputPath) -or (Get-Item -LiteralPath $OutputPath).Length -eq 0) {
    throw 'PresentMon produced no frames. Re-run elevated and confirm no other ETW session holds PresentMon.'
}
if ($SkipAnalysis) { return }

$offset = Get-PresentMonDateTimeOffset -CsvPath $OutputPath -CaptureRequestedAt $captureRequestedAt
& $record ("recorded timestamps are {0:+#;-#;0} h from local time" -f $offset.TotalHours)
$stats = Get-PhaseFrameStats -CsvPath $OutputPath -Phases $marks.ToArray() -RecordedOffset $offset
Write-PhaseStats -Stats $stats -ReportPath ([IO.Path]::ChangeExtension($OutputPath, '.stats.txt'))

Write-Host 'How to read it:' -ForegroundColor Yellow
Write-Host '  * dcomp against layered is the comparison that matters; compare >20ms and PresentMode.'
Write-Host '  * every phase with a window must print the same geometry: block= numbers must match.'
Write-Host '  * if both report Independent Flip, no fast path is being lost, so a presentation rewrite is'
Write-Host '    capped at the measured difference.'
