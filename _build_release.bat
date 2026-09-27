@echo off
REM Production build + allowlisted Windows package. See RELEASING.md.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build-windows.ps1" %*
exit /b %ERRORLEVEL%
