@echo off
setlocal
set "SCRIPT=%~dp0installer\Manage-Install.ps1"
if not exist "%SCRIPT%" set "SCRIPT=%~dp0Manage-Install.ps1"
if "%~1"=="" (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" -Mode Uninstall -Apply
) else (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" -Mode Uninstall -ObsRoot "%~1" -Apply
)
set "RESULT=%errorlevel%"
echo.
pause
exit /b %RESULT%
