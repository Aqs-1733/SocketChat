@echo off
setlocal EnableExtensions
cd /d "%~dp0"
call setup_env.bat
if errorlevel 1 exit /b 1
if not exist bin\obj-cli mkdir bin\obj-cli
if not exist bin\obj-gui mkdir bin\obj-gui
cl /nologo /EHsc /std:c++17 /MT /W4 /utf-8 /I"%NPCAP_SDK%\Include" ^
    /Fo:bin\obj-cli\ /Fe:"bin\packet_capture.exe" ^
    src\main.cpp src\display.cpp src\capture_service.cpp src\ethernet_parser.cpp ^
    /link /LIBPATH:"%NPCAP_SDK%\Lib\x64" /DELAYLOAD:wpcap.dll ^
    delayimp.lib wpcap.lib Packet.lib Ws2_32.lib
if errorlevel 1 exit /b 1
cl /nologo /EHsc /std:c++17 /MT /W4 /utf-8 /DUNICODE /D_UNICODE /I"%NPCAP_SDK%\Include" ^
    /Fo:bin\obj-gui\ /Fe:"bin\packet_capture_gui.exe" ^
    src\gui_main.cpp src\capture_service.cpp src\ethernet_parser.cpp ^
    /link /SUBSYSTEM:WINDOWS /LIBPATH:"%NPCAP_SDK%\Lib\x64" /DELAYLOAD:wpcap.dll ^
    delayimp.lib wpcap.lib Packet.lib Ws2_32.lib User32.lib Gdi32.lib Comctl32.lib
if errorlevel 1 exit /b 1
call run_tests.bat
if errorlevel 1 exit /b 1
echo [PASS] Both capture programs and the parser tests compiled.
exit /b 0