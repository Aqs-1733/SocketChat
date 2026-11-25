@echo off
rem Called from build/test scripts; keep the environment in the caller.
if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" goto compiler_ready
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] Visual Studio C++ Build Tools were not found.
    exit /b 1
)
set "VS_ROOT="
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_ROOT=%%I"
if not defined VS_ROOT (
    echo [ERROR] Install the Desktop development with C++ workload.
    exit /b 1
)
call "%VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
:compiler_ready
where cl >nul 2>nul
if errorlevel 1 exit /b 1
if not defined NPCAP_SDK set "NPCAP_SDK=%~dp0dependencies\npcap-sdk-1.17"
if not exist "%NPCAP_SDK%\Include\pcap.h" (
    echo [ERROR] Npcap SDK header not found: %NPCAP_SDK%
    exit /b 1
)
if not exist "%NPCAP_SDK%\Lib\x64\wpcap.lib" exit /b 1
if not exist "%NPCAP_SDK%\Lib\x64\Packet.lib" exit /b 1
exit /b 0
