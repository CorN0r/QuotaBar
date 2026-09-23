// provider_icons.hpp — 原厂供应商图标（RCDATA 内嵌 128px 圆角芯片 PNG）。
// 资源：PVID_KIMI / PVID_CODEX / PVID_GLM / PVID_DEEPSEEK（assets/quota_bar.rc）。
// 两条绘制路径共用一次解码：
//   - GDI+（详情页卡片徽章 / 圆环徽章）：DrawProviderIcon
//   - D2D（嵌入条名称行）：ProviderIconPixels 取预乘 BGRA，rt->CreateBitmap 每帧重建
// 「原厂图标替代名称」显示选项开关由调用方判定（cfg.display.embed_brand_icons）。
#pragma once
#include <windows.h>
#include <objidl.h> // GDI+ 头的前置要求（IStream；WIN32_LEAN_AND_MEAN 下不自动带入）
#include <gdiplus.h>
#include "config.hpp"

namespace qb {

// 幂等初始化：解码全部四枚图标（需在 UI 线程；GDI+ 由 theme::EnsureGdiplus 保证）
void ProviderIconsInit();

// 该类型图标是否已就绪（资源缺失/解码失败 = false，调用方回落文字）
bool ProviderIconReady(AccountType t);

// GDI+ 直绘：图标铺满 box（box 应为正方形；插值模式在函数内设置）
void DrawProviderIcon(HDC hdc, AccountType t, const RECT& box);

// GDI+ 位图本体（圆环徽章等需按状态调 alpha 的自绘场景）；未就绪返回 nullptr
Gdiplus::Bitmap* ProviderGpBitmap(AccountType t);

// D2D 用：预乘 BGRA 像素（正方形，stride = side*4）；未就绪返回 nullptr
const BYTE* ProviderIconPixels(AccountType t, UINT& side);

} // namespace qb
