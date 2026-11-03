[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$probeRoot = Split-Path -Parent $PSScriptRoot
$probeExe = Join-Path $probeRoot 'out/build/cloud-capture-probe/IMaoCloudCaptureProbe.exe'
$probeOutput = Join-Path $probeRoot 'out/evidence/cloud-wuthering/manual'
if (-not (Test-Path -LiteralPath $probeExe)) {
    throw 'Build the probe first with Build-CloudCaptureProbe.ps1 -DependencyRoot <existing dependency checkout>.'
}
# Launching only shows instructions. The user explicitly chooses the foreground
# browser with the hotkey; this script does not activate or control any browser.
$probeProcess = Start-Process -FilePath $probeExe -ArgumentList @('--output', ('"' + $probeOutput + '"')) -WindowStyle Hidden -PassThru
Write-Output "Manual capture probe started (PID $($probeProcess.Id)). Follow the dialog; no capture starts automatically."
Write-Output "Reports: $probeOutput"
