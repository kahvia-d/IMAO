$ErrorActionPreference = 'Stop'
$cursorRepo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
& (Join-Path $PSScriptRoot 'Build.ps1')
Push-Location $cursorRepo
try {
    & .\out\gamepad-map-cursor\GamepadMapCursorRuntime.exe Tests/MapUi *> out/gamepad-map-cursor/tests.log
    $cursorExit = $LASTEXITCODE
    Get-Content -LiteralPath out/gamepad-map-cursor/tests.log
    if ($cursorExit -ne 0) { throw "Cursor detector regression failed: exit $cursorExit" }
}
finally { Pop-Location }
