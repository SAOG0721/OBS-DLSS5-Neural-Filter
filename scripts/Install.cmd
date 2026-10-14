@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install.ps1" -Apply
set "result=%errorlevel%"
if not "%result%"=="0" echo Installation failed. Read INSTALL.md and close OBS before trying again.
pause
exit /b %result%
