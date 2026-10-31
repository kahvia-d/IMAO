@echo off
setlocal
set "ocrAdapter=0"
if not "%~1"=="" set "ocrAdapter=%~1"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-OcrTest.ps1" -Backend directml -Adapter "%ocrAdapter%"
set "result=%errorlevel%"
if not "%result%"=="0" pause
exit /b %result%
