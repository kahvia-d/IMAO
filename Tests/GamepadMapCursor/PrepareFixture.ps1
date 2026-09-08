param([Parameter(Mandatory = $true)][string]$SourcePath)
$ErrorActionPreference = 'Stop'
$cursorRepo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$cursorRunner = Join-Path $cursorRepo 'out\gamepad-map-cursor\GamepadMapCursorRuntime.exe'
if (-not (Test-Path -LiteralPath $cursorRunner)) { & (Join-Path $PSScriptRoot 'Build.ps1') }
$cursorFixture = Join-Path $cursorRepo 'Tests\MapUi\controller-cursor-assistant.png'
if (Test-Path -LiteralPath $cursorFixture) { throw 'The prepared fixture already exists; inspect it instead of replacing evidence.' }
& $cursorRunner ([IO.Path]::GetFullPath($SourcePath)) --prepare $cursorFixture
if ($LASTEXITCODE -ne 0) { throw 'Cursor fixture preparation failed.' }
Get-FileHash -LiteralPath $cursorFixture -Algorithm SHA256
