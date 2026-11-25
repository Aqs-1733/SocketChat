@echo off
cd /d "%~dp0"
if not exist bin\packet_capture_gui.exe call build.bat
if not exist bin\packet_capture_gui.exe exit /b 1
start "" "%~dp0bin\packet_capture_gui.exe"
