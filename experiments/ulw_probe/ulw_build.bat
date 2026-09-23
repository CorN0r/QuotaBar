@echo off
rem Build ulw_probe.exe - same flat style as probe_build.bat.
setlocal enabledelayedexpansion
set ROOT=%~dp0
set BT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
if exist "%BT%\VC\Auxiliary\Build\vcvars64.bat" goto :found
set BT=!ProgramFiles(x86)!\Microsoft Visual Studio\2022\BuildTools
if exist "%BT%\VC\Auxiliary\Build\vcvars64.bat" goto :found
set BT=!ProgramFiles!\Microsoft Visual Studio\2022\BuildTools
if exist "%BT%\VC\Auxiliary\Build\vcvars64.bat" goto :found
echo [ulw] MSVC not found - install VS2022 BuildTools C++ workload
exit /b 1
:found
call "%BT%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d "%ROOT%"
cl /nologo /W4 /permissive- /EHsc /O1 /utf-8 /std:c++20 ^
  /MT /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS ^
  ulw_probe.cpp ^
  /link /SUBSYSTEM:CONSOLE /OUT:ulw_probe.exe ^
  user32.lib gdi32.lib gdiplus.lib dwmapi.lib
if errorlevel 1 goto :failed
echo [ulw] OK: %ROOT%ulw_probe.exe
endlocal
exit /b 0
:failed
echo [ulw] FAILED
exit /b 1
