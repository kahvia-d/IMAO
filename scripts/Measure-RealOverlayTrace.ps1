# Measures the REAL overlay with PresentMon, on the same three-phase structure as
# Test-OverlayPresentPath.ps1: tool off, tool running, tool off again.
#
# Why it exists: the probe showed that an equivalent overlay surface costs about 1.8 fps and that both
# window types stay in Hardware: Independent Flip, while the real overlay window measured about 10 fps
# in the capture/no-capture comparison. The real overlay has never had its PresentMode measured, and
# that measurement decides whether the presentation path is worth rewriting at all.
#
# It cannot start the overlay itself - the overlay lives in the WinUI app - so this prompts at each
# transition and you drive the app. Everything else is automated.
#
# PresentMon needs its own ETW session: run this from an elevated PowerShell.
#
# Usage:  .\Measure-RealOverlayTrace.ps1
#         .\Measure-RealOverlayTrace.ps1 -PhaseSeconds 60
#
# Settings to use, so the result is comparable with the probe and with the group 2/3 experiment:
#   * diagnostics page: BOTH diagnostic switches OFF (no "hide overlay window", no "hold present")
#   * the game in the foreground, on the same scene, for the whole run
[CmdletBinding()]
param(
    [string]$ProcessName = 'Client-Win64-Shipping.exe',
    [int]$PhaseSeconds = 45,
    [int]$WarmupSeconds = 5,
    [string]$OutputPath,
    [string]$PresentMonPath,
    [switch]$SkipAnalysis
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'OverlayTrace.Common.ps1')

$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputPath)) { $OutputPath = Join-Path $repoRoot 'out\perf\real-overlay-trace.csv' }
if ([string]::IsNullOrWhiteSpace($PresentMonPath)) { $PresentMonPath = Join-Path $repoRoot 'out\perf\PresentMon-2.5.1.exe' }

Assert-FrameTraceElevated
if (-not (Test-Path -LiteralPath $PresentMonPath)) { throw "PresentMon not found: $PresentMonPath" }
$gameName = [IO.Path]::GetFileNameWithoutExtension($ProcessName)
if (-not (Get-Process -Name $gameName -ErrorAction SilentlyContinue)) {
    throw "The game ($ProcessName) is not running. Start it, put it in the foreground, then re-run."
}

$phaseLog = [Collections.Generic.List[string]]::new()
$record = {
    param([string]$Line)
    $stamp = (Get-Date).ToString('HH:mm:ss')
    $text = "$stamp  $Line"
    $phaseLog.Add($text)
    Write-Host $text -ForegroundColor Cyan
}

# Three phases, each recording a warm-up it does not measure, plus generous slack for the manual
# transitions. The capture is stopped once the phases are done, so over-budgeting costs nothing.
$totalSeconds = ($PhaseSeconds + $WarmupSeconds + 60) * 3 + 90
& $record "phases: off -> ON -> off, about $PhaseSeconds s each"
& $record "make sure both diagnostic switches are OFF before continuing"
& $record "trace: $OutputPath"
& $record ('-' * 60)

$captureRequestedAt = Get-Date
$capture = Start-FrameTrace -PresentMonPath $PresentMonPath -ProcessName $ProcessName `
    -OutputPath $OutputPath -Seconds $totalSeconds

$phases = [Collections.Generic.List[object]]::new()
function Wait-Phase {
    param([string]$Name, [int]$Warmup = 0)
    # The warm-up is recorded and excluded from the measured window rather than being cut short on
    # screen, so the marks below are already the window the statistics use.
    for ($remaining = $Warmup; $remaining -gt 0; --$remaining) {
        Write-Host ("`r    warm-up {0,3} s  ({1})   " -f $remaining, $Name) -NoNewline
        Start-Sleep -Seconds 1
    }
    $start = Get-Date
    & $record "PHASE START $Name"
    for ($remaining = $PhaseSeconds; $remaining -gt 0; --$remaining) {
        Write-Host ("`r    measuring {0,3} s  ({1})   " -f $remaining, $Name) -NoNewline
        Start-Sleep -Seconds 1
    }
    Write-Host ''
    $end = Get-Date
    & $record "PHASE END   $Name"
    $phases.Add([pscustomobject]@{ Name = $Name; Start = $start; End = $end })
}

try {
    & $record 'keep the game in the foreground for the whole run'
    Write-Host ''
    Write-Host 'Make sure the tool says it is NOT running, then press Enter to start the baseline.' -ForegroundColor Yellow
    [void](Read-Host)
    Wait-Phase -Name 'off1' -Warmup $WarmupSeconds

    Write-Host 'Now click 开始探索 in the tool. Wait until the overlay is actually up (markers or status bar visible), then press Enter.' -ForegroundColor Yellow
    [void](Read-Host)
    Wait-Phase -Name 'toolOn' -Warmup $WarmupSeconds

    Write-Host 'Now click 停止探索. Wait until the overlay is gone, then press Enter.' -ForegroundColor Yellow
    [void](Read-Host)
    Wait-Phase -Name 'off2' -Warmup $WarmupSeconds
}
finally {
    # Everything is measured, so the capture is stopped here rather than being waited out.
    if (-not $capture.HasExited) { $capture.Kill(); $capture.WaitForExit(30000) | Out-Null }
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
$stats = Get-PhaseFrameStats -CsvPath $OutputPath -Phases $phases.ToArray() -RecordedOffset $offset
# A separate report path: the phase marks file is the input this table was computed from, so writing
# the table over it would destroy the timestamps a re-analysis needs.
Write-PhaseStats -Stats $stats -ReportPath ([IO.Path]::ChangeExtension($OutputPath, '.stats.txt'))

Write-Host 'How to read it:' -ForegroundColor Yellow
Write-Host '  * toolOn in Hardware: Independent Flip  -> the overlay is not costing the fast path, and a'
Write-Host '    presentation rewrite is capped at the probe-measured gain (about 5 fps, fewer >20 ms frames).'
Write-Host '  * toolOn in Composed: Flip               -> the probe is missing something about the real'
Write-Host '    overlay; find that difference before rewriting anything.'
Write-Host '  * off1/off2 should agree. If they do not, the scene changed and the comparison is unsafe.'
