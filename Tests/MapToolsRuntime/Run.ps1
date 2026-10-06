param([switch]$RouteControls)
$ErrorActionPreference = 'Stop'
$mapToolsRepo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$mapToolsOutput = Join-Path $mapToolsRepo 'out\map-tools-runtime'
$mapToolsInfo = [Diagnostics.ProcessStartInfo]::new()
$mapToolsInfo.FileName = Join-Path $mapToolsOutput 'MapToolsRuntime.exe'
$mapToolsInfo.WorkingDirectory = $mapToolsOutput
if($RouteControls){$mapToolsInfo.Arguments="--route-controls"}
$mapToolsInfo.UseShellExecute = $false
$mapToolsInfo.CreateNoWindow = $true
$mapToolsInfo.EnvironmentVariables['LOCALAPPDATA'] = Join-Path $mapToolsOutput 'local-app-data'
$mapToolsProcess = [Diagnostics.Process]::Start($mapToolsInfo)
try {
    $mapToolsProcess.WaitForExit()
    $mapToolsPattern=if($RouteControls){'route-controls-*'}else{'evidence-*'}
    $mapToolsEvidence = Get-ChildItem -LiteralPath $mapToolsOutput -Directory -Filter $mapToolsPattern |
        Sort-Object Name -Descending | Select-Object -First 1
    if ($mapToolsEvidence) { Get-Content -LiteralPath (Join-Path $mapToolsEvidence.FullName 'results.log') }
    if ($mapToolsProcess.ExitCode -ne 0) { throw "Map tools window regression failed; see $($mapToolsEvidence.FullName)" }
}
finally { $mapToolsProcess.Dispose() }
