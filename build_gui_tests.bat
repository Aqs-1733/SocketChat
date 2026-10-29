@echo off
setlocal
cd /d "%~dp0"
where cl >nul 2>nul
if errorlevel 1 exit /b 1
if not exist bin mkdir bin
pushd bin
cl /nologo /EHsc /std:c++17 /W4 /utf-8 /Fe:gui_layout_tests.exe ..\tests\gui_layout_tests.cpp ..\src\protocol.cpp /link /SUBSYSTEM:CONSOLE Ws2_32.lib User32.lib Gdi32.lib
set result=%errorlevel%
popd
exit /b %result%
