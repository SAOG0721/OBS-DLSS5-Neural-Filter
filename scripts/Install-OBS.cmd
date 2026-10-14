@echo off
setlocal
if "%~1"=="" (
    echo Drag the OBS installation folder onto this file, or use:
    echo Install-OBS.cmd "D:\SteamLibrary\steamapps\common\OBS Studio"
    pause
    exit /b 1
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-OBS.ps1" -ObsRoot "%~1" -Apply
set "result=%errorlevel%"
if not "%result%"=="0" echo Installation failed. Read INSTALL.md and close OBS before trying again.
pause
exit /b %result%
