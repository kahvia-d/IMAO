$ErrorActionPreference = 'Stop'
$overlayRepo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$overlayOutput = Join-Path $overlayRepo 'out\overlay-restart-runtime'
[IO.Directory]::CreateDirectory($overlayOutput) | Out-Null
$overlayVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$overlayVcvars = & $overlayVswhere -latest -products * -find 'VC\Auxiliary\Build\vcvars64.bat' | Select-Object -First 1
if (-not $overlayVcvars) { throw 'Visual Studio desktop C++ tools are required.' }
$overlayLog = Join-Path $overlayOutput 'build.log'
$overlayStart = [Diagnostics.ProcessStartInfo]::new()
$overlayStart.FileName = "$env:SystemRoot\System32\cmd.exe"
$overlayStart.Arguments = '/d /c call "' + $overlayVcvars + '" >nul && cl /nologo /std:c++20 /EHsc /W4 /WX /utf-8' +
    ' /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE /I IMao-Core\src' +
    ' IMao-Core\tests\OverlayRestartRuntime.cpp /Foout\overlay-restart-runtime\OverlayRestartRuntime.obj' +
    ' /Feout\overlay-restart-runtime\OverlayRestartRuntime.exe /link d3d11.lib dxgi.lib user32.lib' +
    ' > "' + $overlayLog + '" 2>&1'
$overlayStart.UseShellExecute = $false
$overlayStart.CreateNoWindow = $true
$overlayStart.WorkingDirectory = $overlayRepo
[void]$overlayStart.EnvironmentVariables.Remove('Path')
[void]$overlayStart.EnvironmentVariables.Remove('PATH')
$overlayStart.EnvironmentVariables['PATH'] = [Environment]::GetEnvironmentVariable('Path', 'Process')
$overlayProcess = [Diagnostics.Process]::Start($overlayStart)
try {
    $overlayProcess.WaitForExit()
    Get-Content -LiteralPath $overlayLog
    if ($overlayProcess.ExitCode -ne 0) { throw "Overlay runtime test build failed: $overlayLog" }
}
finally { $overlayProcess.Dispose() }
