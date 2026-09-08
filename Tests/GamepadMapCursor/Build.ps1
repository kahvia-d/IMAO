$ErrorActionPreference = 'Stop'
$cursorRepo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$cursorOutput = Join-Path $cursorRepo 'out\gamepad-map-cursor'
[IO.Directory]::CreateDirectory($cursorOutput) | Out-Null
$cursorVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$cursorVcvars = & $cursorVswhere -latest -products * -find 'VC\Auxiliary\Build\vcvars64.bat' | Select-Object -First 1
if (-not $cursorVcvars) { throw 'Visual Studio desktop C++ tools are required.' }
$cursorLog = Join-Path $cursorOutput 'build.log'
$cursorStart = [Diagnostics.ProcessStartInfo]::new()
$cursorStart.FileName = "$env:SystemRoot\System32\cmd.exe"
$cursorStart.Arguments = '/d /c call "' + $cursorVcvars + '" >nul && cl /nologo /std:c++20 /EHsc /O2 /W4 /utf-8' +
    ' /DNOMINMAX /I IMao-Core\src /external:I third_party\build\opencv-4.11.0 /external:I third_party\src\opencv-4.11.0\include' +
    ' /external:I third_party\src\opencv-4.11.0\modules\core\include /external:I third_party\src\opencv-4.11.0\modules\imgproc\include' +
    ' /external:I third_party\src\opencv-4.11.0\modules\imgcodecs\include /external:W0' +
    ' IMao-Core\tests\GamepadMapCursorRuntime.cpp IMao-Core\src\App\GamepadMapCursorDetector.cpp IMao-Core\src\App\MapUiVisualDetector.cpp' +
    ' /Foout\gamepad-map-cursor\ /Feout\gamepad-map-cursor\GamepadMapCursorRuntime.exe' +
    ' /link third_party\build\opencv-4.11.0\lib\opencv_world4110.lib user32.lib > "' + $cursorLog + '" 2>&1'
$cursorStart.UseShellExecute = $false; $cursorStart.CreateNoWindow = $true; $cursorStart.WorkingDirectory = $cursorRepo
[void]$cursorStart.EnvironmentVariables.Remove('Path'); [void]$cursorStart.EnvironmentVariables.Remove('PATH')
$cursorStart.EnvironmentVariables['PATH'] = [Environment]::GetEnvironmentVariable('Path', 'Process')
$cursorProcess = [Diagnostics.Process]::Start($cursorStart)
try {
    $cursorProcess.WaitForExit(); Get-Content -LiteralPath $cursorLog
    if ($cursorProcess.ExitCode -ne 0) { throw "Cursor detector fixture build failed: $cursorLog" }
}
finally { $cursorProcess.Dispose() }
Copy-Item -LiteralPath (Join-Path $cursorRepo 'third_party\build\opencv-4.11.0\bin\opencv_world4110.dll') -Destination $cursorOutput -Force
