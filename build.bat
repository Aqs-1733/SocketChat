@echo off
setlocal
cd /d "%~dp0"
where cl >nul 2>nul
if errorlevel 1 (
    echo [ERROR] MSVC cl.exe not found. Run this from x64 Native Tools Command Prompt.
    exit /b 1
)
if not exist bin mkdir bin
pushd bin
cl /nologo /EHsc /std:c++17 /W4 /utf-8 /Fe:server.exe ..\src\server.cpp ..\src\protocol.cpp /link Ws2_32.lib
if errorlevel 1 (popd & exit /b 1)
cl /nologo /EHsc /std:c++17 /W4 /utf-8 /Fe:client.exe ..\src\client.cpp ..\src\protocol.cpp /link Ws2_32.lib
if errorlevel 1 (popd & exit /b 1)
cl /nologo /EHsc /std:c++17 /W4 /utf-8 /Fe:protocol_tests.exe ..\tests\protocol_tests.cpp ..\src\protocol.cpp /link Ws2_32.lib
if errorlevel 1 (popd & exit /b 1)
cl /nologo /EHsc /std:c++17 /W4 /utf-8 /Fe:integration_tests.exe ..\tests\integration_tests.cpp ..\src\protocol.cpp /link Ws2_32.lib
if errorlevel 1 (popd & exit /b 1)
protocol_tests.exe
if errorlevel 1 (popd & exit /b 1)
popd
echo [PASS] Build and protocol unit tests complete.
endlocal
