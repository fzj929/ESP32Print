@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_flash.ps1" %*
set "result=%ERRORLEVEL%"
if not "%result%"=="0" echo Build or flash failed. See the error above.
pause
exit /b %result%
