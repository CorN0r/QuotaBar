// ui_icons.hpp — 图标矢量库与应用 logo（Material Symbols 几何近似，24 单位栅格）。
// 2026-09-18 从 tray_ui.cpp 抽出（God Module 拆分第四步）：图标绘制零全局依赖，
// 只需 DPI 缩放（SetIconsDpi 与 tray_ui 的 SetLayoutDpi 同步调用），任何页面/控件
// 绘制模块都可复用。描边宽度按 DPI 换算，其余参数为纯几何。
#pragma once
#include <windows.h>

namespace qb {

enum Ic {
    IcNone = 0, IcSync, IcMin, IcMax, IcRestore, IcClose, IcDonut, IcTune, IcEye, IcCopy, IcClear, IcLock, IcRestart, IcInfo, IcBell, IcCheck, IcMinus, IcPlus,
    IcSave, IcTrash, IcGrid, IcKeyboard, IcMonitor, IcCode
};

// 图标：圆头描边几何近似（stroke_dip 为 DIP 单位线宽，按注入 DPI 换算像素）
void Icon(HDC hdc, int id, const RECT& box, COLORREF color, int alpha,
          float stroke_dip = 1.6f);

// 应用 logo（stitch_minimalist：青→靛→紫渐变圆环 + 深底 + 青蓝渐变圆心）
void DrawLogo(HDC hdc, const RECT& box);

// DPI 注入：必须与窗口布局 DPI 同步（默认 96）；描边随 DPI 缩放
void SetIconsDpi(int dpi);

} // namespace qb
