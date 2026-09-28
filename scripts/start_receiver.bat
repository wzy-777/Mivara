@echo off
cd /d "%~dp0"
taskkill /f /im receiver.exe >nul 2>&1
timeout /t 1 /nobreak >nul
start "" "%~dp0..\bin\receiver.exe"
