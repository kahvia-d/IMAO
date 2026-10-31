$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/../scripts/Enter-DevEnvironment.ps1"
$helper = "$PSScriptRoot/../scripts/ocr-test/Start-OcrTest.ps1"
if (-not (Test-Path $helper)) { throw 'Missing elevation-safe OCR launch helper' }
. $helper
# Execute the encoded elevated payload in a fresh process with deliberately wrong inherited env.
# The stand-in receives actual PowerShell bound arguments, including a path with spaces/apostrophe.
$temp = Join-Path ([IO.Path]::GetTempPath()) ("ocr launch ' " + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $temp | Out-Null
$previousBackend = $env:IMAO_OCR_BACKEND
$previousAdapter = $env:IMAO_OCR_ADAPTER
try {
    $probe = Join-Path $temp 'probe.ps1'
    $probeCode = @'
param([string]$Backend, [int]$Adapter, [switch]$Elevated)
if ($Backend -ne 'EXPECTED_BACKEND' -or $Adapter -ne EXPECTED_ADAPTER -or -not $Elevated) { exit 3 }
exit 0
'@
    $env:IMAO_OCR_BACKEND = 'cpu'
    $env:IMAO_OCR_ADAPTER = '0'
    foreach ($choice in @(@{ Backend = 'directml'; Adapter = 7 }, @{ Backend = 'cpu'; Adapter = 0 })) {
        $probeCode.Replace('EXPECTED_BACKEND', $choice.Backend).Replace('EXPECTED_ADAPTER', [string]$choice.Adapter) | Set-Content $probe
        $arguments = Get-OcrElevationArguments -ScriptPath $probe @choice
        $process = Start-Process powershell.exe -ArgumentList $arguments -Wait -PassThru -WindowStyle Hidden
        if ($process.ExitCode -ne 0) { throw "Explicit mode/adapter lost across elevation payload: $($process.ExitCode)" }
    }
    Write-Host 'PASS explicit mode, adapter, elevated marker and quoted path survive a fresh process'
} finally {
    $env:IMAO_OCR_BACKEND = $previousBackend
    $env:IMAO_OCR_ADAPTER = $previousAdapter
    # This exact GUID-named directory was created above beneath the OS temp folder.
    if ([IO.Path]::GetFullPath((Split-Path -Parent $temp)).TrimEnd('\') -ne
        [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')) { throw 'Unexpected test cleanup path' }
    Remove-Item -LiteralPath $temp -Recurse -Force
}
