# Shared by the generator and the Windows PowerShell 5.1 checked apply tool.
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
function Assert-DeltaPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or $Path -match '[\\:*?"<>|\x00-\x1f]' -or $Path.StartsWith('/')) { throw "Unsafe relative path: $Path" }
    foreach ($part in $Path.Split('/')) {
        if ($part -eq '' -or $part -eq '.' -or $part -eq '..' -or $part -match '[. ]$' -or $part -match '^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])($|\.)') { throw "Unsafe path component: $Path" }
    }
    if ($Path -eq '_test-delta' -or $Path.StartsWith('_test-delta/', [StringComparison]::OrdinalIgnoreCase)) { throw "Reserved delta metadata path: $Path" }
}
function Get-DeltaRoot([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    if ($full -eq 'C:\Dapps\IMao' -or $full.StartsWith('C:\Dapps\IMao\', [StringComparison]::OrdinalIgnoreCase)) { throw 'The player installation C:\Dapps\IMao is forbidden.' }
    $cursor = $full
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "Reparse point forbidden: $cursor" }
        }
        $cursor = Split-Path -Parent $cursor
    }
    return $full
}
function Get-DeltaFiles([string]$Root) {
    $Root = Get-DeltaRoot $Root
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) { throw "Missing directory: $Root" }
    $pending = New-Object 'System.Collections.Generic.Queue[string]'
    $pending.Enqueue($Root)
    while ($pending.Count -gt 0) {
        foreach ($item in Get-ChildItem -LiteralPath $pending.Dequeue() -Force) {
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "Reparse point forbidden: $($item.FullName)" }
            if ($item.PSIsContainer) { $pending.Enqueue($item.FullName) }
            else {
                $relative = $item.FullName.Substring($Root.Length + 1).Replace('\', '/')
                Assert-DeltaPath $relative
                [pscustomobject]@{ path = $relative; length = $item.Length; sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
            }
        }
    }
}
function Get-DeltaIdentity($Files) {
    $ordered = New-Object 'System.Collections.Generic.List[string]'
    foreach ($file in $Files) { $ordered.Add(('{0}|{1}|{2}' -f $file.path, $file.length, $file.sha256)) }
    $ordered.Sort([StringComparer]::Ordinal)
    $lines = $ordered -join "`n"
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($lines)))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
function Get-DeltaMap($Files) {
    $map = @{}
    foreach ($file in $Files) {
        Assert-DeltaPath $file.path
        if ($file.sha256 -notmatch '^[a-fA-F0-9]{64}$' -or $file.length -lt 0 -or $map.ContainsKey($file.path)) { throw "Invalid or duplicate manifest entry: $($file.path)" }
        $map[$file.path] = $file
    }
    return $map
}
function Assert-DeltaFile([string]$Root, $Entry) {
    Assert-DeltaPath $Entry.path
    $path = Get-DeltaRoot (Join-Path $Root $Entry.path)
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing package file: $($Entry.path)" }
    if ((Get-Item -LiteralPath $path).Length -ne $Entry.length -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $Entry.sha256) { throw "Package digest mismatch: $($Entry.path)" }
}
function Assert-DeltaPackage([string]$Root, $Files, [string]$Version) {
    foreach ($file in $Files) { Assert-DeltaFile $Root $file }
    if ((Get-Content -LiteralPath (Join-Path $Root 'build-info.json') -Raw | ConvertFrom-Json).appVersion -ne $Version) { throw 'Package version mismatch.' }
    $runtime = Get-Content -LiteralPath (Join-Path $Root 'IMao-WinUI.runtimeconfig.json') -Raw | ConvertFrom-Json
    if (-not $runtime.runtimeOptions.includedFrameworks) { throw 'Package must be self-contained (includedFrameworks required).' }
}
function Assert-DeltaStopped {
    $running = @(Get-Process -Name 'IMao-WinUI', 'IMao-Launcher', 'IMao-CoreHost' -ErrorAction SilentlyContinue)
    if ($running.Count) { throw 'Close IMao UI, Launcher and CoreHost completely before applying or verifying the delta.' }
}
