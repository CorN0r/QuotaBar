// ui_icons.cpp — 图标矢量库实现（自 tray_ui.cpp 原样迁移；改动需带回归依据）。
#include "ui_icons.hpp"
#include "theme.hpp"
#include <objidl.h> // GDI+ 头的前置要求（IStream）
#include <gdiplus.h>
#include <algorithm>

namespace qb {

namespace {
int g_icon_dpi = 96;
inline float IconLsf(float v) { return v * g_icon_dpi / 96.0f; }
} // namespace

void SetIconsDpi(int dpi) {
    if (dpi > 0) g_icon_dpi = dpi;
}

// =========================================================
// 图标：Material Symbols 的几何近似（24 单位栅格，圆头描边）
// =========================================================

void Icon(HDC hdc, int id, const RECT& box, COLORREF color, int alpha, float stroke_dip) {
    if (id == IcNone || box.right <= box.left || box.bottom <= box.top) return;
    theme::EnsureGdiplus();
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float u = (box.right - box.left) / 24.0f;
    const float ox = (float)box.left, oy = (float)box.top;
    Gdiplus::Pen pen(Gdiplus::Color((BYTE)alpha, GetRValue(color), GetGValue(color),
                                    GetBValue(color)),
                     std::max(IconLsf(stroke_dip), 1.0f));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    auto X = [&](float v) { return ox + v * u; };
    auto Y = [&](float v) { return oy + v * u; };
    auto L = [&](float x1, float y1, float x2, float y2) {
        g.DrawLine(&pen, X(x1), Y(y1), X(x2), Y(y2));
    };
    auto A = [&](float l, float t, float w, float h, float start, float sweep) {
        g.DrawArc(&pen, X(l), Y(t), w * u, h * u, start, sweep);
    };
    auto RR = [&](float l, float t, float w, float h, float r) {
        Gdiplus::GraphicsPath p;
        float d = 2 * r * u;
        p.AddArc(X(l), Y(t), d, d, 180, 90);
        p.AddArc(X(l + w - r), Y(t), d, d, 270, 90);
        p.AddArc(X(l + w - r), Y(t + h - r), d, d, 0, 90);
        p.AddArc(X(l), Y(t + h - r), d, d, 90, 90);
        p.CloseFigure();
        g.DrawPath(&pen, &p);
    };
    Gdiplus::SolidBrush br(Gdiplus::Color((BYTE)alpha, GetRValue(color), GetGValue(color),
                                          GetBValue(color)));
    auto Idot = [&](float cx, float cy, float r) {
        g.FillEllipse(&br, X(cx - r), Y(cy - r), 2 * r * u, 2 * r * u);
    };
    auto FRR = [&](float l, float t, float w, float h, float r) {
        Gdiplus::GraphicsPath p;
        float d = 2 * r * u;
        p.AddArc(X(l), Y(t), d, d, 180, 90);
        p.AddArc(X(l + w - r), Y(t), d, d, 270, 90);
        p.AddArc(X(l + w - r), Y(t + h - r), d, d, 0, 90);
        p.AddArc(X(l), Y(t + h - r), d, d, 90, 90);
        p.CloseFigure();
        g.FillPath(&br, &p);
    };
    switch (id) {
        case IcSync:
            A(4.5f, 4.5f, 15, 15, -50, 280);
            L(19.5f, 2.5f, 19.5f, 7.5f); L(19.5f, 7.5f, 15, 7.5f);
            break;
        case IcMin: L(6, 12.5f, 18, 12.5f); break;
        case IcMax: RR(6, 6, 12, 12, 1); break;
        case IcRestore: RR(7, 9, 9, 9, 1); L(9.5f, 7, 16.5f, 7); L(16.5f, 7, 16.5f, 14); break;
        case IcClose: L(7, 7, 17, 17); L(17, 7, 7, 17); break;
        case IcDonut: A(5, 5, 14, 14, 30, 290); break;
        case IcTune:
            L(4, 8.5f, 20, 8.5f); Idot(9, 8.5f, 2.4f);
            L(4, 15.5f, 20, 15.5f); Idot(15, 15.5f, 2.4f);
            break;
        case IcEye:
            A(3.5f, 8, 17, 8, 0, 360);
            Idot(12, 12, 2.6f);
            break;
        case IcCopy:
            RR(9, 9, 10.5f, 10.5f, 1.5f);
            L(15.5f, 5.5f, 6.5f, 5.5f); L(6.5f, 5.5f, 6.5f, 14.5f);
            break;
        case IcClear:
            RR(6.5f, 7, 13.5f, 10, 1.5f);
            L(10.5f, 10, 16, 14); L(16, 10, 10.5f, 14);
            break;
        case IcLock:
            RR(7, 11, 10, 8, 1.5f);
            A(9, 4.5f, 6, 8, 180, 180);
            Idot(12, 15, 1.2f);
            break;
        case IcRestart:
            A(5, 5, 14, 14, -40, 280);
            L(19.5f, 3, 19.5f, 8); L(19.5f, 8, 15, 8);
            break;
        case IcInfo:
            A(5, 5, 14, 14, 0, 360);
            L(12, 11, 12, 16.5f); Idot(12, 7.8f, 1.2f);
            break;
        case IcBell:
            A(6.5f, 4, 11, 12, 180, 180);
            L(6.5f, 10, 6.5f, 14); L(17.5f, 10, 17.5f, 14);
            L(5, 15.8f, 19, 15.8f);
            Idot(12, 19, 1.4f);
            break;
        case IcCheck: L(5.5, 12.5f, 10, 17); L(10, 17, 18.5f, 7); break;
        case IcMinus: L(6, 12, 18, 12); break;
        case IcPlus: L(6, 12, 18, 12); L(12, 6, 12, 18); break;
        case IcSave:
            RR(4.5f, 4.5f, 15, 15, 2);
            L(8, 4.5f, 8, 10); L(8, 10, 16, 10); L(16, 10, 16, 4.5f);
            FRR(8, 13, 8, 6.5f, 1);
            break;
        case IcTrash:
            L(4.5f, 7, 19.5f, 7);
            L(9.5f, 7, 9.5f, 5); L(9.5f, 5, 14.5f, 5); L(14.5f, 5, 14.5f, 7);
            RR(7, 7, 10, 12.5f, 1.5f);
            L(10.2f, 10, 10.2f, 16.5f); L(13.8f, 10, 13.8f, 16.5f);
            break;
        case IcGrid:
            FRR(4.5f, 4.5f, 6.4f, 6.4f, 1); FRR(13.1f, 4.5f, 6.4f, 6.4f, 1);
            FRR(4.5f, 13.1f, 6.4f, 6.4f, 1); FRR(13.1f, 13.1f, 6.4f, 6.4f, 1);
            break;
        case IcKeyboard:
            RR(3.5f, 7, 17, 10.5f, 1.5f);
            for (float kx = 6.2f; kx <= 17.8f; kx += 2.32f) Idot(kx, 10.4f, 0.75f);
            L(7, 14.2f, 17, 14.2f);
            break;
        case IcMonitor:
            RR(4, 5, 16, 11, 1.5f);
            L(12, 16, 12, 19.5f); L(9, 19.5f, 15, 19.5f);
            break;
        case IcCode:
            L(9, 8, 5, 12); L(5, 12, 9, 16);
            L(15, 8, 19, 12); L(19, 12, 15, 16);
            break;
        default:
            break;
    }
}

// 应用 logo（stitch_minimalist：青→靛→紫渐变圆环 + 深底 + 青蓝渐变圆心），24 栅格
void DrawLogo(HDC hdc, const RECT& box) {
    const int width = box.right - box.left;
    const int height = box.bottom - box.top;
    if (width <= 0 || height <= 0) return;
    // Shared module resource: Windows owns the cached icon handle.
    HICON icon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(102),
                                  IMAGE_ICON, width, height, LR_SHARED);
    if (icon) DrawIconEx(hdc, box.left, box.top, icon, width, height, 0, nullptr, DI_NORMAL);
}

} // namespace qb
