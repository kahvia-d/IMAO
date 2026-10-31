@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-OcrTest.ps1" -Backend cpu
set "result=%errorlevel%"
if not "%result%"=="0" pause
exit /b %result%
