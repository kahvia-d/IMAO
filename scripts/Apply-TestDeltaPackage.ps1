# Compatible with Windows PowerShell 5.1. Extract the ZIP to a separate folder first.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$PackageRoot,
    [string]$DeltaRoot = (Split-Path -Parent $PSScriptRoot),
    [switch]$VerifyOnly
)
. (Join-Path $PSScriptRoot 'TestDeltaPackage.Common.ps1')
$PackageRoot = Get-DeltaRoot $PackageRoot
$DeltaRoot = Get-DeltaRoot $DeltaRoot
Assert-DeltaStopped
$manifest = Get-Content -LiteralPath (Get-DeltaRoot (Join-Path $DeltaRoot '_test-delta/manifest.json')) -Raw | ConvertFrom-Json
if ($manifest.schema -ne 1 -or $manifest.mode -ne 'overlay-no-removals') { throw 'Unsupported delta manifest.' }
$baseMap = Get-DeltaMap $manifest.baseFiles
$targetMap = Get-DeltaMap $manifest.targetFiles
if ((Get-DeltaIdentity $manifest.baseFiles) -ne $manifest.baseIdentity -or (Get-DeltaIdentity $manifest.targetFiles) -ne $manifest.targetIdentity) { throw 'Manifest identity mismatch.' }
foreach ($path in $baseMap.Keys) { if (-not $targetMap.ContainsKey($path)) { throw 'Overlay delta cannot contain removals.' } }
$expected = @($manifest.targetFiles | Where-Object { $_.path -eq 'resources.pri' -or -not $baseMap.ContainsKey($_.path) -or $baseMap[$_.path].sha256 -ne $_.sha256 })
$payloadMap = Get-DeltaMap $manifest.payloadFiles
if ($payloadMap.Count -ne $expected.Count) { throw 'Incomplete payload manifest.' }
foreach ($file in $expected) {
    if (-not $payloadMap.ContainsKey($file.path) -or $payloadMap[$file.path].sha256 -ne $file.sha256 -or $payloadMap[$file.path].length -ne $file.length) { throw "Payload/target mismatch: $($file.path)" }
}
if ($VerifyOnly) {
    Assert-DeltaPackage $PackageRoot $manifest.targetFiles $manifest.targetVersion
    Write-Host "Verified target $($manifest.targetVersion): differing=0; package files=$($manifest.targetFiles.Count). User extra files preserved."
    return
}
Assert-DeltaPackage $PackageRoot $manifest.baseFiles $manifest.baseVersion
# Complete validation and preflight before the first write. New files never overwrite user extras.
foreach ($file in $expected) {
    Assert-DeltaFile $DeltaRoot $file
    $destination = Get-DeltaRoot (Join-Path $PackageRoot $file.path)
    if (Test-Path -LiteralPath $destination) {
        if (-not $baseMap.ContainsKey($file.path)) { throw "New package file collides with user extra: $($file.path)" }
        if ((Get-Item -LiteralPath $destination).IsReadOnly) { throw "Read-only destination: $($file.path)" }
        $handle = [IO.File]::Open($destination, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        $handle.Dispose()
    }
    $ancestor = Split-Path -Parent $destination
    while (-not (Test-Path -LiteralPath $ancestor)) { $ancestor = Split-Path -Parent $ancestor }
    if (-not (Test-Path -LiteralPath $ancestor -PathType Container)) { throw "Destination ancestor is a file: $ancestor" }
}
Assert-DeltaStopped
foreach ($file in $expected) {
    $destination = Get-DeltaRoot (Join-Path $PackageRoot $file.path)
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $destination))
    [IO.File]::Copy((Join-Path $DeltaRoot $file.path), $destination, $true)
}
Assert-DeltaPackage $PackageRoot $manifest.targetFiles $manifest.targetVersion
Write-Host "Applied $($manifest.baseVersion) -> $($manifest.targetVersion): differing=0. Fully restart IMao."
