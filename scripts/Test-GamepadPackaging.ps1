[CmdletBinding()]
param([Parameter(Mandatory)][string]$AppRoot, [string]$ManualZip, [string]$ShardRoot)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$dep = (Get-Content (Join-Path $repo '.github/dependencies.lock.json') -Raw | ConvertFrom-Json).dependencies | Where-Object name -eq sdl3
$receipt = Get-Content (Join-Path $AppRoot 'SDL3-BUILD.json') -Raw | ConvertFrom-Json
if ($receipt.version -cne $dep.version -or $receipt.sourceSha256 -cne $dep.sha256 -or $receipt.patchSha256 -cne $dep.patchSha256) { throw 'Packaged SDL provenance differs from dependency lock.' }
$expected = @{
    'SDL3.dll' = $receipt.dllSha256
    'SDL3-LICENSE.txt' = $receipt.licenseSha256
    'SDL3-BUILD.json' = (Get-FileHash (Join-Path $AppRoot 'SDL3-BUILD.json') -Algorithm SHA256).Hash.ToLowerInvariant()
}
foreach ($name in $expected.Keys) {
    if ((Get-FileHash (Join-Path $AppRoot $name) -Algorithm SHA256).Hash.ToLowerInvariant() -cne $expected[$name]) { throw "Packaged dependency differs: $name" }
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
function Assert-Zips([string[]]$Paths, [bool]$AllowPrefix) {
    $found = @{}
    foreach ($path in $Paths) {
        $zip = [IO.Compression.ZipFile]::OpenRead($path)
        try {
            foreach ($entry in $zip.Entries) {
                $name = $entry.FullName.Replace('\','/')
                if ($AllowPrefix) { $name = ($name -split '/')[-1] }
                if (-not $expected.ContainsKey($name)) { continue }
                if ($found.ContainsKey($name)) { throw "Duplicate packaged dependency: $name" }
                $input = $entry.Open(); $sha = [Security.Cryptography.SHA256]::Create()
                try { $hash = ([BitConverter]::ToString($sha.ComputeHash($input))).Replace('-','').ToLowerInvariant() }
                finally { $input.Dispose(); $sha.Dispose() }
                if ($hash -cne $expected[$name]) { throw "Archive dependency differs: $name in $path" }
                $found[$name] = $true
            }
        } finally { $zip.Dispose() }
    }
    foreach ($name in $expected.Keys) { if (-not $found.ContainsKey($name)) { throw "Archive dependency missing: $name" } }
}
if ($ManualZip) { Assert-Zips @($ManualZip) $true }
if ($ShardRoot) {
    $pointers = @(Get-ChildItem -LiteralPath $ShardRoot -Filter '*-shards.json' -File)
    if ($pointers.Count -ne 1) { throw 'Exactly one program shard descriptor required.' }
    $pointer = Get-Content $pointers[0].FullName -Raw | ConvertFrom-Json
    $paths = @($pointer.shards | ForEach-Object {
        if ([IO.Path]::GetFileName($_.name) -cne $_.name) { throw 'Invalid shard asset name.' }
        $path = Join-Path $ShardRoot $_.name
        if ((Get-Item $path).Length -ne $_.size -or (Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $_.sha256) { throw "Shard digest differs: $path" }
        $path
    })
    Assert-Zips $paths $false
}
Write-Host 'SDL output, provenance and requested archive checks passed.'
