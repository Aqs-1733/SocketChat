@echo off
setlocal
cd /d "%~dp0"
where cl >nul 2>nul
if errorlevel 1 (
    echo Please use x64 Native Tools Command Prompt for VS.
    exit /b 1
)
cl /nologo /EHsc /std:c++17 /W4 /O2 /utf-8 /Fe:reliable_udp.exe reliable_udp.cpp /link Ws2_32.lib
if errorlevel 1 exit /b 1
cl /nologo /EHsc /std:c++17 /W4 /O2 /utf-8 /Fe:reliable_udp_gui.exe gui.cpp /link /SUBSYSTEM:WINDOWS
exit /b %errorlevel%
