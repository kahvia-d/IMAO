@echo off
setlocal
tasklist /FI "IMAGENAME eq IMao-WinUI.exe" /NH | find /I "IMao-WinUI.exe" >nul
if not errorlevel 1 goto running
tasklist /FI "IMAGENAME eq IMao-CoreHost.exe" /NH | find /I "IMao-CoreHost.exe" >nul
if not errorlevel 1 goto running
set "IMAO_OCR_BACKEND=cpu"
set "IMAO_OCR_ADAPTER="
start "" "%~dp0IMao-WinUI.exe"
exit /b 0
:running
echo Please fully exit IMao before switching OCR mode.
pause
exit /b 1
