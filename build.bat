@echo off
rem EVE1G - one-click build. Double-click this file.
rem Everything it prints is also saved to build.log next to it.
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "cmd /c tools\build_inner.bat 2>&1 | Tee-Object -FilePath build.log"
echo.
pause
