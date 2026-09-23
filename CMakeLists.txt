# QuotaBar — Windows 额度插件（见 outputs/Windows额度插件-开发设计文档.md）
# 本机若无 CMake，可直接运行 build.bat（内部调用 MSVC cl.exe，效果等价）。
cmake_minimum_required(VERSION 3.20)
project(QuotaBar LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT MSVC)
    message(FATAL_ERROR "QuotaBar 需要 MSVC 构建")
endif()

add_executable(QuotaBar WIN32
    src/main.cpp
    src/util.cpp
    src/json.cpp
    src/log.cpp
    src/storage.cpp
    src/state.cpp
    src/providers.cpp
    src/provider_icons.cpp
    src/http.cpp
    src/scheduler.cpp
    src/overlay.cpp
    src/taskbar_slot.cpp
    src/theme.cpp
    src/ui_icons.cpp
    src/ui_layout.cpp
    src/quota_layout.cpp
    src/disp_layout.cpp
    src/quota_page.cpp
    src/display_page.cpp
    src/ui_draw.cpp
    src/detail_shell.cpp
    src/tray_area.cpp
    src/notify.cpp
    src/app.cpp
    src/tests.cpp
)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/assets/app.manifest)
    target_sources(QuotaBar PRIVATE assets/app.manifest)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/assets/quota_bar.rc)
    target_sources(QuotaBar PRIVATE assets/quota_bar.rc)
endif()

target_compile_definitions(QuotaBar PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS GDIPLUS_VERSION=1)
target_compile_options(QuotaBar PRIVATE
    /W4 /permissive- /Zc:preprocessor /O1 /GL /GF /Gy /Gw /Zc:inline /EHsc /utf-8)
target_link_options(QuotaBar PRIVATE
    /LTCG /OPT:REF /OPT:ICF /NXCOMPAT /DYNAMICBASE /MANIFEST:EMBED
    /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup)
if(MSVC AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/assets/app.manifest)
    target_link_options(QuotaBar PRIVATE /MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/assets/app.manifest)
endif()
set_target_properties(QuotaBar PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
    LINK_LIBRARIES "user32;gdi32;shell32;advapi32;crypt32;winhttp;gdiplus;shcore;wtsapi32;comctl32;ole32;d2d1;dwrite;d3d11;dxgi;dcomp;windowsapp")
