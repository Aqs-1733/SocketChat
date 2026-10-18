@echo off
setlocal
cd /d "%~dp0"
where cl >nul 2>nul
if errorlevel 1 (
 echo [ERROR] Use x64 Native Tools Command Prompt for VS.
 exit /b 1
)
if not exist bin mkdir bin
pushd bin
cl /nologo /EHsc /std:c++17 /W4 /utf-8 /Fe:gui_client.exe ..\src\gui_client.cpp ..\src\protocol.cpp /link /SUBSYSTEM:WINDOWS Ws2_32.lib User32.lib Gdi32.lib
if errorlevel 1 (popd & exit /b 1)
popd
echo [PASS] Native Win32 GUI build.
endlocal
