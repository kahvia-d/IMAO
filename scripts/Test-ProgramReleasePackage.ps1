[CmdletBinding()]
param([Parameter(Mandatory)][string]$PackageRoot,[Parameter(Mandatory)][string]$OutputRoot)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'PowerShell 7 or newer is required.' }
$PackageRoot = [IO.Path]::GetFullPath($PackageRoot)
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if ($OutputRoot.StartsWith($PackageRoot.TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Probe output must stay outside the package.' }
[IO.Directory]::CreateDirectory($OutputRoot) | Out-Null
$before = @(Get-ChildItem -LiteralPath $PackageRoot -Recurse -File | ForEach-Object { [pscustomobject]@{path=[IO.Path]::GetRelativePath($PackageRoot,$_.FullName);length=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash} })
$snapshot = Get-Content -LiteralPath (Join-Path $PackageRoot 'Assets/Updates/bundled-snapshot.json') -Raw | ConvertFrom-Json
$snapshot.baselineRoot = Join-Path $PackageRoot 'Assets'
$snapshot.mapDataRoot = Join-Path $PackageRoot "Assets/$($snapshot.mapDataRoot)"
foreach ($p in $snapshot.packages) { $p.directory = Join-Path $PackageRoot "Assets/$($p.directory)" }
$snapshotPath = Join-Path $OutputRoot 'snapshot.json'
[IO.File]::WriteAllText($snapshotPath,($snapshot | ConvertTo-Json -Depth 30),[Text.UTF8Encoding]::new($false))
function Start-Isolated([string[]]$Arguments) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = Join-Path $PackageRoot 'IMao-CoreHost.exe'
    foreach ($arg in $Arguments) { $start.ArgumentList.Add($arg) }
    $start.WorkingDirectory = $OutputRoot; $start.UseShellExecute = $false; $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
    $start.Environment['LOCALAPPDATA'] = Join-Path $OutputRoot 'LocalAppData'
    $start.Environment['APPDATA'] = Join-Path $OutputRoot 'AppData'
    [void]$start.Environment.Remove('Path')
    [void]$start.Environment.Remove('PATH')
    $start.Environment['PATH'] = "$env:SystemRoot/System32;$env:SystemRoot"
    return [Diagnostics.Process]::Start($start)
}
$check = Start-Isolated @('--check-resource-snapshot',$snapshotPath)
$stdout = $check.StandardOutput.ReadToEndAsync(); $stderr = $check.StandardError.ReadToEndAsync()
try {
    if (-not $check.WaitForExit(180000)) { $check.Kill($true); throw 'Snapshot validation timed out.' }
    [IO.File]::WriteAllText((Join-Path $OutputRoot 'resources.stdout.json'),$stdout.GetAwaiter().GetResult())
    [IO.File]::WriteAllText((Join-Path $OutputRoot 'resources.stderr.log'),$stderr.GetAwaiter().GetResult())
    if ($check.ExitCode -ne 0) { throw 'Snapshot validation failed.' }
} finally { $check.Dispose() }
$pipeName = 'IMao.ReleaseProbe.'+[guid]::NewGuid().ToString('N')
$process = Start-Isolated @('--pipe',$pipeName,'--resource-snapshot',$snapshotPath)
$processOutput = $process.StandardOutput.ReadToEndAsync(); $processError = $process.StandardError.ReadToEndAsync()
$pipe = [IO.Pipes.NamedPipeClientStream]::new('.',$pipeName,[IO.Pipes.PipeDirection]::InOut,[IO.Pipes.PipeOptions]::Asynchronous)
$reader=$null; $writer=$null; $observedSnapshot=$false
try {
    $pipe.Connect(15000)
    $reader=[IO.StreamReader]::new($pipe,[Text.UTF8Encoding]::new($false),$false,4096,$true)
    $writer=[IO.StreamWriter]::new($pipe,[Text.UTF8Encoding]::new($false),4096,$true); $writer.AutoFlush=$true
    foreach ($type in @('hello','stop','shutdown')) {
        $id=[guid]::NewGuid().ToString('N')
        $writer.WriteLine((@{version=1;type=$type;requestId=$id} | ConvertTo-Json -Compress))
        $deadline=[DateTime]::UtcNow.AddSeconds(20); $ack=$false
        while ([DateTime]::UtcNow -lt $deadline) {
            $task=$reader.ReadLineAsync(); if (-not $task.Wait([int][Math]::Max(1,($deadline-[DateTime]::UtcNow).TotalMilliseconds))) { throw "IPC timeout: $type" }
            if ($null -eq $task.Result) { throw 'IPC ended before acknowledgement.' }
            $event=$task.Result | ConvertFrom-Json
            if ($event.resourceSnapshotId -eq $snapshot.snapshotId -or $event.data.resourceSnapshotId -eq $snapshot.snapshotId) { $observedSnapshot=$true }
            if ($event.type -eq 'ack' -and $event.requestId -eq $id) { if (-not $event.accepted) { throw "IPC rejected: $type" }; $ack=$true; break }
        }
        if (-not $ack) { throw "IPC acknowledgement missing: $type" }
    }
    if (-not $process.WaitForExit(15000)) { throw 'CoreHost shutdown timed out.' }
    if ($process.ExitCode -ne 0) { throw 'CoreHost exited abnormally.' }
    if (-not $observedSnapshot) { throw 'CoreHost did not report the expected resource snapshot ID.' }
} finally {
    if ($writer) { $writer.Dispose() }; if ($reader) { $reader.Dispose() }; $pipe.Dispose()
    if (-not $process.HasExited) { $process.Kill($true); $process.WaitForExit() }
    [IO.File]::WriteAllText((Join-Path $OutputRoot 'ipc.stdout.log'),$processOutput.GetAwaiter().GetResult())
    [IO.File]::WriteAllText((Join-Path $OutputRoot 'ipc.stderr.log'),$processError.GetAwaiter().GetResult())
    $process.Dispose()
}
& (Join-Path $PSScriptRoot 'Test-ResourcePackagePicker.ps1') -OutputDirectory (Join-Path $OutputRoot 'resource-package-picker') -AssemblyPath (Join-Path $PackageRoot 'IMao-WinUI.dll')
if (-not $?) { throw 'Packaged resource picker probe failed.' }
$after = @(Get-ChildItem -LiteralPath $PackageRoot -Recurse -File | ForEach-Object { [pscustomobject]@{path=[IO.Path]::GetRelativePath($PackageRoot,$_.FullName);length=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash} })
if (Compare-Object $before $after -Property path,length,sha256) { throw 'Isolated probe modified package files.' }
[IO.File]::WriteAllText((Join-Path $OutputRoot 'package-probe.json'),(@{passed=$true;resourceSnapshotId=$snapshot.snapshotId;packageUnchanged=$true;isolatedUserData=$true;packagedResourcePicker=$true} | ConvertTo-Json))
Write-Host 'PASS complete resources, isolated IPC snapshot identity/shutdown, packaged native picker, and unchanged package contents.'
