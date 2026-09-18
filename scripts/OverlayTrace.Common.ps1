# Shared PresentMon handling for the overlay cost traces. Dot-source it: it defines functions only.
#
# Why a helper: the frame-time split has to be identical for the probe trace and the real-overlay
# trace, or the two results cannot be put in the same table. It has one implementation, here.
#
# PresentMon's --date_time column runs 8 hours ahead of local time on this machine (recorded in
# Docs/GameFrameDropAnalysis_20260917.md), which is why every comparison shifts the wall-clock phase
# marks rather than the recorded timestamps.

# PresentMon needs its own ETW session, so a trace started without elevation fails silently and only
# produces an empty file. Failing here instead is the difference between a clear error and a mystery.
function Assert-FrameTraceElevated {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'PresentMon needs an ETW session, so this must run from an elevated PowerShell.'
    }
}

function Get-PresentMonDateTimeOffset {
    <#
      The recorded timestamps are ahead of local time. If PresentMon ever ships a build that aligns
      them, the offset is discovered from the trace itself instead of assumed: the first recorded
      frame cannot be later than the moment the capture was requested.
    #>
    param(
        [string]$CsvPath,
        [datetime]$CaptureRequestedAt
    )
    $first = Get-Content -LiteralPath $CsvPath -TotalCount 2 | Select-Object -Last 1
    if (-not $first) { return [timespan]::Zero }
    try { $recorded = [datetime](($first -split ',')[8]) } catch { return [timespan]::Zero }
    # Only a whole-hour offset is plausible for a timezone/clock-reporting difference; anything else
    # means the column is not what this helper expects, and zero is the safe assumption.
    $difference = $recorded - $CaptureRequestedAt
    $hours = [math]::Round($difference.TotalHours)
    if ([math]::Abs($difference.TotalHours - $hours) -gt 0.5) { return [timespan]::Zero }
    return [timespan]::FromHours($hours)
}

function Start-FrameTrace {
    <#
      Starts PresentMon in the background. --terminate_after_timed is required: without it the process
      stays alive after the timed capture and the next session cannot be created.
    #>
    param(
        [Parameter(Mandatory)][string]$PresentMonPath,
        [Parameter(Mandatory)][string]$ProcessName,
        [Parameter(Mandatory)][string]$OutputPath,
        [Parameter(Mandatory)][int]$Seconds
    )
    Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($PresentMonPath)) -ErrorAction SilentlyContinue |
        Stop-Process -Force
    Start-Sleep -Seconds 2
    Start-Process -FilePath $PresentMonPath -ArgumentList @(
        '--process_name', $ProcessName,
        '--output_file', $OutputPath,
        '--timed', $Seconds,
        '--no_console_stats',
        '--date_time',
        '--terminate_after_timed',
        '--stop_existing_session'
    ) -PassThru -WindowStyle Hidden
}

function Read-FrameTraceRow {
    <#
      Reads the trace once and yields only what the statistics need, so a multi-megabyte CSV is not
      held as fully materialised objects in memory.
    #>
    param([Parameter(Mandatory)][string]$CsvPath)
    $index = @{}
    $header = (Get-Content -LiteralPath $CsvPath -TotalCount 1) -split ','
    for ($column = 0; $column -lt $header.Count; ++$column) { $index[$header[$column].Trim()] = $column }
    foreach ($name in 'TimeInDateTime', 'MsBetweenPresents', 'PresentMode') {
        if (-not $index.ContainsKey($name)) { throw "PresentMon CSV has no $name column: $CsvPath" }
    }
    $timeColumn = $index['TimeInDateTime']
    $frameColumn = $index['MsBetweenPresents']
    $modeColumn = $index['PresentMode']
    $reader = [IO.File]::OpenText($CsvPath)
    try {
        [void]$reader.ReadLine()
        while ($null -ne ($line = $reader.ReadLine())) {
            if ($line.Length -eq 0) { continue }
            $fields = $line -split ','
            if ($fields.Count -le $modeColumn) { continue }
            $frameMs = 0.0
            $parsed = [double]::TryParse($fields[$frameColumn],
                [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$frameMs)
            $time = [datetime]::MinValue
            $timeParsed = [datetime]::TryParse($fields[$timeColumn],
                [Globalization.CultureInfo]::InvariantCulture,
                [Globalization.DateTimeStyles]::None, [ref]$time)
            Write-Output ([pscustomobject]@{
                Time = $time; Valid = $timeParsed; FrameMs = $frameMs; ValidFrame = $parsed; Mode = $fields[$modeColumn]
            })
        }
    }
    finally { $reader.Dispose() }
}

function Get-PhaseFrameStats {
    <#
      Splits a trace by wall-clock phase marks and reports the numbers the whole investigation rests
      on. Phases are given as objects with Name, Start and End local timestamps; an optional
      WarmupSeconds is dropped from the start of the window, because the seconds right after switching
      a subsystem off still contain work that was already in flight.
    #>
    param(
        [Parameter(Mandatory)][string]$CsvPath,
        [Parameter(Mandatory)][object[]]$Phases,
        [timespan]$RecordedOffset = [timespan]::FromHours(8)
    )
    $rows = @(Read-FrameTraceRow -CsvPath $CsvPath)
    if ($rows.Count -eq 0) { throw "Trace has no rows: $CsvPath" }
    $results = foreach ($phase in $Phases) {
        $warmup = if ($phase.PSObject.Properties.Name -contains 'WarmupSeconds') { [int]$phase.WarmupSeconds } else { 0 }
        $from = $phase.Start.AddSeconds($warmup) + $RecordedOffset
        $to = $phase.End + $RecordedOffset
        $frames = New-Object 'System.Collections.Generic.List[double]'
        $modeCounts = @{}
        foreach ($row in $rows) {
            if (-not $row.Valid -or -not $row.ValidFrame) { continue }
            if ($row.Time -lt $from -or $row.Time -ge $to) { continue }
            $frames.Add($row.FrameMs)
            if ($modeCounts.ContainsKey($row.Mode)) { $modeCounts[$row.Mode]++ } else { $modeCounts[$row.Mode] = 1 }
        }
        $count = $frames.Count
        if ($count -eq 0) {
            [pscustomobject]@{ Phase = $phase.Name; Frames = 0; Fps = 0; P50 = 0; P95 = 0; P99 = 0; Max = 0
                Over20Percent = 0; Over33Percent = 0; Modes = '' }
            continue
        }
        $sorted = $frames | Sort-Object
        $average = ($sorted | Measure-Object -Average).Average
        $percentile = {
            param([double]$q)
            $sorted[[math]::Max(0, [math]::Min($count - 1, [int][math]::Floor($q * $count)))]
        }
        $over20 = ($sorted | Where-Object { $_ -gt 20 }).Count
        $over33 = ($sorted | Where-Object { $_ -gt 33 }).Count
        $modes = ($modeCounts.GetEnumerator() | Sort-Object Value -Descending |
            ForEach-Object { "$($_.Key) $([math]::Round(100.0 * $_.Value / $count, 1))%" }) -join ' | '
        [pscustomobject]@{
            Phase = $phase.Name
            Frames = $count
            Fps = [math]::Round(1000.0 / $average, 1)
            P50 = [math]::Round((& $percentile 0.50), 2)
            P95 = [math]::Round((& $percentile 0.95), 2)
            P99 = [math]::Round((& $percentile 0.99), 2)
            Max = [math]::Round($sorted[$count - 1], 2)
            Over20Percent = [math]::Round(100.0 * $over20 / $count, 2)
            Over33Percent = [math]::Round(100.0 * $over33 / $count, 2)
            Modes = $modes
        }
    }
    return $results
}

function Format-PhaseFrameStats {
    param([Parameter(Mandatory)][object[]]$Stats)
    $Stats | Format-Table -AutoSize -Wrap @(
        'Phase', 'Frames', 'Fps',
        @{ Label = 'p50ms'; Expression = { $_.P50 } },
        @{ Label = 'p95ms'; Expression = { $_.P95 } },
        @{ Label = 'p99ms'; Expression = { $_.P99 } },
        @{ Label = 'maxms'; Expression = { $_.Max } },
        @{ Label = '>20ms'; Expression = { $_.Over20Percent } },
        @{ Label = '>33ms'; Expression = { $_.Over33Percent } },
        @{ Label = 'PresentMode'; Expression = { $_.Modes } }
    ) | Out-String
}

function Write-PhaseStats {
    <#
      Reports the per-phase table and, when asked, writes it next to the trace so the numbers survive
      the console window. The report path must not be the phase-mark file: an earlier version passed
      the same path for both, and writing the table silently replaced the timestamps the table had
      been derived from.
    #>
    param(
        [Parameter(Mandatory)][object[]]$Stats,
        [string]$ReportPath
    )
    $text = Format-PhaseFrameStats -Stats $Stats
    Write-Host $text
    if (-not [string]::IsNullOrWhiteSpace($ReportPath)) {
        ($Stats | Format-List | Out-String) + $text | Set-Content -LiteralPath $ReportPath -Encoding utf8
        Write-Host "phase report: $ReportPath" -ForegroundColor Cyan
    }
}
