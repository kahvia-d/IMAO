[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$TileManifest,
    [Parameter(Mandatory = $true)]
    [string]$Output,
    [Parameter(Mandatory = $true)]
    [string]$Report,
    [string]$VerifyReference,
    [Nullable[double]]$AnchorWorldX,
    [Nullable[double]]$AnchorWorldY,
    [string]$PaddleLib = $env:IMAO_PADDLE_LIB,
    [string]$OpenCvDir = $env:IMAO_OPENCV_DIR
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$preflight = Join-Path $PSScriptRoot 'Test-BuildPrerequisites.ps1'
& $preflight -PaddleLib $PaddleLib -OpenCvDir $OpenCvDir -Dotnet (Join-Path $repoRoot 'tools\dotnet-sdk-8.0.424\dotnet.exe')
if (-not $?) { throw 'Build prerequisite validation failed.' }

$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$cmake = & $vswhere -latest -products * -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' | Select-Object -First 1
$ninja = & $vswhere -latest -products * -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe' | Select-Object -First 1
$vcvars = & $vswhere -latest -products * -find 'VC\Auxiliary\Build\vcvars64.bat' | Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($cmake) -or [string]::IsNullOrWhiteSpace($ninja) -or [string]::IsNullOrWhiteSpace($vcvars)) {
    throw 'Unable to locate the Visual Studio CMake, Ninja, or x64 developer environment.'
}

function Invoke-VisualStudioCommand([string]$CommandLine) {
    $logPath = Join-Path $env:TEMP ("imao-kuro-builder-" + [guid]::NewGuid().ToString('N') + '.log')
    $cmdCommand = 'call "' + $vcvars + '" >nul && (' + $CommandLine + ') > "' + $logPath + '" 2>&1'
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.UseShellExecute = $false
    [void]$startInfo.EnvironmentVariables
    [void]$startInfo.EnvironmentVariables.Remove('Path')
    [void]$startInfo.EnvironmentVariables.Remove('PATH')
    $startInfo.EnvironmentVariables.Add('PATH', (Split-Path -Parent $ninja) + ';' + [System.Environment]::GetEnvironmentVariable('Path', 'Process'))
    $startInfo.FileName = "$env:SystemRoot\System32\cmd.exe"
    $startInfo.Arguments = '/d /c ' + $cmdCommand
    $startInfo.WorkingDirectory = $repoRoot
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    [void]$process.Start()
    $process.WaitForExit()
    if (Test-Path -LiteralPath $logPath) {
        Get-Content -LiteralPath $logPath
        Remove-Item -LiteralPath $logPath -Force
    }
    if ($process.ExitCode -ne 0) { throw "Command failed with exit code $($process.ExitCode): $CommandLine" }
}

if (-not (Test-Path -LiteralPath $TileManifest)) { throw "Tile manifest is missing: $TileManifest" }
$outputParent = Split-Path -Parent $Output
$reportParent = Split-Path -Parent $Report
New-Item -ItemType Directory -Force -Path $outputParent, $reportParent | Out-Null

Push-Location $repoRoot
try {
    Invoke-VisualStudioCommand ('"' + $cmake + '" -UOpenCV_DIR --preset windows-x64-relwithdebinfo-diagnostics "-DPADDLE_LIB=' + $PaddleLib + '" "-DOPENCV_DIR=' + $OpenCvDir + '"')
    Invoke-VisualStudioCommand ('"' + $cmake + '" --build --preset windows-x64-relwithdebinfo-diagnostics-kuro-builder')
    Write-Host 'KuroMapFeatureBuilder compilation completed.'
    $builder = Join-Path $repoRoot 'x64\RelWithDebInfo\KuroMapFeatureBuilder.exe'
    if (-not (Test-Path -LiteralPath $builder)) { throw "Feature builder was not produced: $builder" }
    $openCvDll = Get-ChildItem -LiteralPath $OpenCvDir -Recurse -Filter 'opencv_world*.dll' | Select-Object -First 1
    if ($null -eq $openCvDll) { throw "Unable to locate an OpenCV runtime DLL under $OpenCvDir" }
    $originalPath = $env:PATH
    try {
        # The helper is intentionally not installed next to the UI. Give this
        # one invocation the OpenCV runtime directory instead of copying DLLs.
        $env:PATH = $openCvDll.DirectoryName + ';' + $originalPath
        $builderArguments = @('--input', $TileManifest, '--output', $Output, '--report', $Report)
        if (-not [string]::IsNullOrWhiteSpace($VerifyReference)) {
            if ($null -eq $AnchorWorldX -or $null -eq $AnchorWorldY) { throw 'Reference verification requires both anchor coordinates.' }
            $invariant = [Globalization.CultureInfo]::InvariantCulture
            $builderArguments += @('--verify-reference', $VerifyReference, '--anchor-x', [Convert]::ToString($AnchorWorldX, $invariant), '--anchor-y', [Convert]::ToString($AnchorWorldY, $invariant))
        }
        Write-Host 'Running KuroMapFeatureBuilder against the validated temporary tiles.'
        & $builder @builderArguments
        if ($LASTEXITCODE -ne 0) { throw "KuroMapFeatureBuilder failed with exit code $LASTEXITCODE." }
    }
    finally {
        $env:PATH = $originalPath
    }
}
finally {
    Pop-Location
}
