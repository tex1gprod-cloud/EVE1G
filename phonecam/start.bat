@echo off
cd /d "%~dp0"
set PY=python
where py >nul 2>nul && set PY=py
%PY% -c "import cv2" 2>nul || %PY% -m pip install -r requirements.txt
%PY% phonecam.py %*
pause
