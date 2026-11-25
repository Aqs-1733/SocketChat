@echo off
setlocal EnableExtensions
cd /d "%~dp0"
call setup_env.bat
if errorlevel 1 exit /b 1
if not exist bin\obj-tests mkdir bin\obj-tests
cl /nologo /EHsc /std:c++17 /MT /W4 /utf-8 /Fo:bin\obj-tests\ ^
    /Fe:"bin\ethernet_parser_tests.exe" ^
    tests\ethernet_parser_tests.cpp src\ethernet_parser.cpp
if errorlevel 1 exit /b 1
"bin\ethernet_parser_tests.exe"
exit /b %errorlevel%