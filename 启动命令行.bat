@echo off
cd /d "%~dp0"
if not exist bin\packet_capture.exe call build.bat
if not exist bin\packet_capture.exe exit /b 1
"%~dp0bin\packet_capture.exe" %*
if "%~1"=="" pause
