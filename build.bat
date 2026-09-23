@echo off
rem QuotaBar 主构建脚本：需 VS2022 BuildTools 的 MSVC（cl.exe），无 CMake 也可用。GBK+CRLF 编码，勿存为 UTF-8。
setlocal enabledelayedexpansion
set ROOT=%~dp0
set BT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
if not exist "%BT%\VC\Auxiliary\Build\vcvars64.bat" (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set BT=%%i
)
if not exist "%BT%\VC\Auxiliary\Build\vcvars64.bat" (
  echo [build] 未找到 MSVC 编译器：需 VS 2022 BuildTools 与 C++ 生成工具
  exit /b 1
)
call "%BT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

if not exist "%ROOT%build" mkdir "%ROOT%build"
cd /d "%ROOT%build"

set ICON_RC=
if exist "%ROOT%assets\quota_bar.rc" set ICON_RC=%ROOT%assets\quota_bar.rc
if exist "%ROOT%assets\app.manifest" (
  set MANIFEST=/MANIFEST:EMBED /MANIFESTINPUT:%ROOT%assets\app.manifest
) else (
  set MANIFEST=/MANIFEST:EMBED
)

set RES=
if not "%ICON_RC%"=="" (
  rem /utf-8: RC source is UTF-8 (FileDescription has Chinese); ANSI would garble it
  rc /nologo /utf-8 /fo quota_bar.res %ICON_RC%
  if errorlevel 1 (
    echo [build] rc 资源编译失败：本次跳过图标（主构建继续）
    del /q quota_bar.res 2>nul
  )
)
if exist quota_bar.res set RES=quota_bar.res

cl /nologo /W4 /permissive- /Zc:preprocessor /EHsc /O1 /GL /GF /Gy /Gw /Zc:inline /utf-8 /std:c++20 ^
  /MT /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS ^
  %ROOT%src\main.cpp %ROOT%src\util.cpp %ROOT%src\json.cpp %ROOT%src\log.cpp ^
  %ROOT%src\storage.cpp %ROOT%src\state.cpp %ROOT%src\providers.cpp %ROOT%src\provider_icons.cpp %ROOT%src\http.cpp ^
  %ROOT%src\scheduler.cpp %ROOT%src\overlay.cpp %ROOT%src\taskbar_slot.cpp %ROOT%src\theme.cpp %ROOT%src\ui_icons.cpp %ROOT%src\ui_layout.cpp %ROOT%src\quota_layout.cpp %ROOT%src\disp_layout.cpp %ROOT%src\quota_page.cpp %ROOT%src\display_page.cpp %ROOT%src\ui_draw.cpp %ROOT%src\detail_shell.cpp %ROOT%src\tray_area.cpp %ROOT%src\notify.cpp %ROOT%src\app.cpp %ROOT%src\tests.cpp ^
  %RES% ^
  /link /LTCG /OPT:REF /OPT:ICF /NXCOMPAT /DYNAMICBASE %MANIFEST% /SUBSYSTEM:WINDOWS ^
  /ENTRY:wWinMainCRTStartup /OUT:QuotaBar.exe ^
  user32.lib gdi32.lib shell32.lib advapi32.lib crypt32.lib winhttp.lib gdiplus.lib shcore.lib wtsapi32.lib comctl32.lib ole32.lib d2d1.lib dwrite.lib d3d11.lib dxgi.lib dcomp.lib windowsapp.lib
if errorlevel 1 (
  echo [build] FAILED
  exit /b 1
)
echo [build] OK: %ROOT%build\QuotaBar.exe
endlocal
