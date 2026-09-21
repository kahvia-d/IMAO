# Attributes the overlay's frame-rate cost to individual pieces of per-frame work, instead of to
# "capture and localization" as one lump.
#
# The whole overlay costs about 26 fps with frames over 20 ms rising from 0.37% to 10.18% (see
# Docs/GameFrameCostAnalysis_20260918.md section 15). That group contains four independent pieces of
# work with very different sizes, and the point of this run is to rank them so the work targets the
# biggest one rather than the most recently discussed one.
#
# The overlay cannot be configured from here - it lives in the WinUI app - so this prompts you to set
# the isolation mask in the tool's diagnostics page at each phase. Everything else is automated.
#
# PresentMon needs its own ETW session: run this from an elevated PowerShell.
#
# Usage:  .\Measure-WorkIsolation.ps1
#         .\Measure-WorkIsolation.ps1 -PhaseSeconds 15      # shorter, noisier
#         .\Measure-WorkIsolation.ps1 -Experiment status-bar    # what the in-game status bar costs
#         .\Measure-WorkIsolation.ps1 -Experiment full-client   # minimap window against full-client
#
# -Experiment status-bar drives the player setting rather than a mask bit, A/B/A/B, because the bar and
# the minimap status ball are two independent switches in 设置 -> 地图显示. With the bar off the overlay
# window covers the minimap alone; with it on the window is widened to reach the bar. The overlay has to
# be showing the minimap for either phase to mean anything - 开始探索 pressed, big map closed.
#
# -Experiment full-client is the small-window question from
# Docs/MiniMapMarkerRenderPerfPlan_zh-Hans.md: the same harness, run as A/B/A/B, where A is the default
# minimap-sized window and B sets 256, which forces a window the size of the whole game client.
#
# Before starting: game running, in the foreground, on one scene, and the tool already running the
# overlay (开始探索 pressed). Leave the mouse and keyboard alone once a phase begins.
[CmdletBinding()]
param(
    [string]$ProcessName = 'Client-Win64-Shipping.exe',
    [int]$PhaseSeconds = 45,
    [int]$WarmupSeconds = 5,
    # Time allowed per phase for reading the prompt and changing the mask in the tool, so the capture
    # budget covers the whole run rather than ending early.
    [int]$PromptAllowanceSeconds = 45,
    [string]$OutputPath,
    [string]$PresentMonPath,
    [ValidateSet('work-isolation', 'status-bar', 'full-client')][string]$Experiment = 'work-isolation'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'OverlayTrace.Common.ps1')

$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputPath)) { $OutputPath = Join-Path $repoRoot 'out\perf\work-isolation.csv' }
if ([string]::IsNullOrWhiteSpace($PresentMonPath)) { $PresentMonPath = Join-Path $repoRoot 'out\perf\PresentMon-2.5.1.exe' }

Assert-FrameTraceElevated
if (-not (Test-Path -LiteralPath $PresentMonPath)) { throw "PresentMon not found: $PresentMonPath" }
if (-not (Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($ProcessName)) -ErrorAction SilentlyContinue)) {
    throw "The game ($ProcessName) is not running. Start it, put it in the foreground, then re-run."
}

# Each phase sets a mask, so each one is measured against the baseline. The baseline is measured twice,
# once at the start and once at the end: if the two disagree the scene or the machine drifted and the
# whole table is unsafe.
#
# Order matters because a run is long and may be abandoned partway. The first pair is the decisive test
# of how the overlay is presented - colorkey layered window against DirectComposition - because that is
# the one change with a mechanism behind it. Everything after is per-frame work.
$sequence = if ($Experiment -eq 'full-client') {
    # The comparison the small window was decided by, kept available now that it is the default: 0 is the
    # minimap-sized window, 256 forces a window the size of the whole game client. Use it to re-confirm
    # the gain on a heavy scene, where the absolute numbers differ from the light one it was measured on.
    @(
        [pscustomobject]@{ name = 'mini-window-1'; mask = 0;   label = '0    (小地图窗口)' }
        [pscustomobject]@{ name = 'full-client-1'; mask = 256; label = '256  (强制整屏)' }
        [pscustomobject]@{ name = 'mini-window-2'; mask = 0;   label = '0    (小地图窗口)' }
        [pscustomobject]@{ name = 'full-client-2'; mask = 256; label = '256  (强制整屏)' }
    )
} elseif ($Experiment -eq 'status-bar') {
    # What the in-game status bar costs. It is a player setting now, not a mask bit, so every phase keeps
    # the mask at 0 and the prompt asks for the settings toggle instead: off leaves the window on the
    # minimap alone, on widens it to hold the bar.
    @(
        [pscustomobject]@{ name = 'bar-off-1'; mask = 0; label = '0'; instruction = '设置 (Settings) -> 地图显示 -> 把「游戏内状态条」关掉' }
        [pscustomobject]@{ name = 'bar-on-1';  mask = 0; label = '0'; instruction = '设置 (Settings) -> 地图显示 -> 把「游戏内状态条」打开' }
        [pscustomobject]@{ name = 'bar-off-2'; mask = 0; label = '0'; instruction = '设置 (Settings) -> 地图显示 -> 把「游戏内状态条」关掉' }
        [pscustomobject]@{ name = 'bar-on-2';  mask = 0; label = '0'; instruction = '设置 (Settings) -> 地图显示 -> 把「游戏内状态条」打开' }
    )
} else {
    @(
        [pscustomobject]@{ name = 'baseline-colorkey'; mask = 0;  label = '0  (base)' }
        [pscustomobject]@{ name = 'composition';       mask = 64; label = '64' }
        [pscustomobject]@{ name = 'no-overlay-render'; mask = 8;  label = '8' }
        [pscustomobject]@{ name = 'no-window-sync';    mask = 32; label = '32' }
        [pscustomobject]@{ name = 'no-overlay-clear';  mask = 16; label = '16' }
        [pscustomobject]@{ name = 'baseline-again';    mask = 0;  label = '0  (base)' }
    )
}
# Which phases are the reference the others are read against differs per experiment.
$referencePattern = switch ($Experiment) {
    'status-bar' { 'bar-off*' }
    'full-client' { 'mini-window*' }
    default { 'baseline*' }
}

$phaseLog = [Collections.Generic.List[string]]::new()
$record = {
    param([string]$Line)
    $stamp = (Get-Date).ToString('HH:mm:ss')
    $text = "$stamp  $Line"
    $phaseLog.Add($text)
    Write-Host $text -ForegroundColor Cyan
}

# The capture must outlast the phases even when switching masks takes a while. An earlier version
# under-counted this - it ignored the warm-up and the player's time to change the mask - and
# PresentMon's --timed cut the recording off after the third of six phases, so the last three ran
# with nothing recording them.
$settleSeconds = 4
$totalSeconds = $sequence.Count * ($settleSeconds + $WarmupSeconds + $PhaseSeconds + $PromptAllowanceSeconds) + 90
& $record "work isolation: $($sequence.Count) phases of $PhaseSeconds s measured, each preceded by $settleSeconds s settling and $WarmupSeconds s warm-up that is not measured"
& $record ("capture budget: {0:N0} s for {1} phases" -f $totalSeconds, $sequence.Count)
& $record "trace: $OutputPath"
& $record ('-' * 60)

$captureRequestedAt = Get-Date
$capture = Start-FrameTrace -PresentMonPath $PresentMonPath -ProcessName $ProcessName `
    -OutputPath $OutputPath -Seconds $totalSeconds

$marks = [Collections.Generic.List[object]]::new()
try {
    foreach ($phase in $sequence) {
        Write-Host ''
        Write-Host ('=' * 70) -ForegroundColor Yellow
        Write-Host "PHASE $($phase.name)" -ForegroundColor Yellow
        $step = if ($phase.PSObject.Properties['instruction']) { $phase.instruction }
            else { "In the tool: 诊断 (Diagnostics) -> 诊断工具 -> 隔离开关, set it to:  $($phase.label)" }
        Write-Host "  $step" -ForegroundColor Yellow
        Write-Host '  Then press Enter here and stay off the mouse and keyboard until the phase ends.' -ForegroundColor Yellow
        Write-Host ('=' * 70) -ForegroundColor Yellow
        [void](Read-Host)

        # Settling time so the switch itself and whatever work was in flight do not land in the window.
        for ($remaining = $settleSeconds; $remaining -gt 0; --$remaining) {
            Write-Host ("`r    settling {0} s " -f $remaining) -NoNewline
            Start-Sleep -Seconds 1
        }
        # The warm-up is recorded and excluded from the measured window instead of being cut short on
        # screen, so the phase marks below are already the window the statistics use.
        for ($remaining = $WarmupSeconds; $remaining -gt 0; --$remaining) {
            Write-Host ("`r    warm-up  {0} s " -f $remaining) -NoNewline
            Start-Sleep -Seconds 1
        }
        $start = Get-Date
        & $record "PHASE START $($phase.name) mask=$($phase.mask)"
        for ($remaining = $PhaseSeconds; $remaining -gt 0; --$remaining) {
            Write-Host ("`r    measuring {0,3} s  ({1})   " -f $remaining, $phase.name) -NoNewline
            Start-Sleep -Seconds 1
        }
        Write-Host ''
        $end = Get-Date
        & $record "PHASE END   $($phase.name)"
        $marks.Add([pscustomobject]@{
            Name = $phase.name; Start = $start; End = $end; Mask = $phase.mask
        })
    }
}
finally {
    # Everything is measured, so the capture is stopped here rather than being waited out: a run that
    # finishes early should not sit idle until the budget expires.
    if (-not $capture.HasExited) { $capture.Kill(); $capture.WaitForExit(30000) | Out-Null }
}

$logPath = [IO.Path]::ChangeExtension($OutputPath, '.phases.txt')
$phaseLog | Set-Content -LiteralPath $logPath -Encoding utf8
& $record "phase marks written to $logPath"

if (-not (Test-Path -LiteralPath $OutputPath) -or (Get-Item -LiteralPath $OutputPath).Length -eq 0) {
    throw 'PresentMon produced no frames. Re-run elevated and confirm no other ETW session holds PresentMon.'
}

$offset = Get-PresentMonDateTimeOffset -CsvPath $OutputPath -CaptureRequestedAt $captureRequestedAt
$stats = Get-PhaseFrameStats -CsvPath $OutputPath -Phases $marks.ToArray() -RecordedOffset $offset
Write-PhaseStats -Stats $stats -ReportPath ([IO.Path]::ChangeExtension($OutputPath, '.stats.txt'))

# The attribution table is the point of the run: each phase's difference from the baseline is what that
# piece costs the game.
$baseline = $stats | Where-Object { $_.Phase -like $referencePattern } | Select-Object -First 1
if ($null -ne $baseline -and $baseline.Fps -gt 0) {
    Write-Host "Attribution against $($baseline.Phase):" -ForegroundColor Yellow
    $table = foreach ($stat in $stats) {
        if ($stat.Fps -le 0) { continue }
        if ($stat.Phase -eq $baseline.Phase) { continue }
        $delta = $baseline.Fps - $stat.Fps
        $tail = $baseline.Over20Percent - $stat.Over20Percent
        [pscustomobject]@{
            Phase = $stat.Phase
            'fps' = $stat.Fps
            'vs baseline' = [math]::Round($delta, 1)
            '>20ms' = $stat.Over20Percent
            '>20ms saved' = [math]::Round($tail, 2)
        }
    }
    Write-Host ($table | Format-Table -AutoSize | Out-String)

    $check = $stats | Where-Object { $_.Phase -like $referencePattern -and $_.Phase -ne $baseline.Phase } |
        Select-Object -Last 1
    if ($null -ne $check -and $check.Fps -gt 0) {
        $drift = [math]::Abs($baseline.Fps - $check.Fps)
        $verdict = if ($drift -le 3) { 'consistent' } else { 'DRIFTED - treat the table as unsafe' }
        Write-Host ("{0} {1} fps, {2} {3} fps, drift {4} fps: {5}" -f `
            $baseline.Phase, $baseline.Fps, $check.Phase, $check.Fps, [math]::Round($drift, 1), $verdict) `
            -ForegroundColor $(if ($drift -le 3) { 'Green' } else { 'Red' })
    }
}

Write-Host 'Remember to set the isolation mask back to 0 when you are done.' -ForegroundColor Yellow
