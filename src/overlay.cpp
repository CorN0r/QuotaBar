#include "overlay.hpp"
#include "log.hpp"
#include "provider_icons.hpp"
#include "theme.hpp"
#include "util.hpp"
#include <objidl.h> // gdiplus.h 依赖 IStream/PROPID（WIN32_LEAN_AND_MEAN 下不自动带入）
#include <gdiplus.h>
#include <cstring>
#include <cwchar>
#include <cmath>
#include <algorithm>

#pragma comment(lib, "gdiplus.lib")

namespace qb {

// Shell_TrayWnd 等系统窗口类识别隔离在此（§10.2：视为待验证实现细节，失败即降级）
static RECT GetPrimaryTaskbarRect(bool& ok) {
    ok = false;
    RECT rc{};
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray) {
        if (GetWindowRect(tray, &rc)) {
            HMONITOR mon = MonitorFromWindow(tray, MONITOR_DEFAULTTONEAREST);
            POINT pt0{0, 0};
            HMONITOR primary = MonitorFromPoint(pt0, MONITOR_DEFAULTTONEAREST);
            if (mon == primary) {
                ok = true;
                return rc;
            }
        }
    }
    // 降级：主屏底部一条 48px 高的假想任务栏（由工作区推算）
    HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(primary, &mi);
    rc = mi.rcWork;
    rc.top = rc.bottom - 48;
    return rc;
}

struct Overlay::GdiPlusToken {
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    GdiPlusToken() {
        Gdiplus::GdiplusStartup(&token, &input, nullptr);
    }
    ~GdiPlusToken() {
        if (token) Gdiplus::GdiplusShutdown(token);
    }
};

namespace {
constexpr wchar_t kClassName[] = L"QuotaBarOverlayWnd";
constexpr UINT_PTR kRingAnimTimer = 1; // 圆环弧值过渡动画的唯一计时器
constexpr UINT_PTR kGrabTimer = 2;     // Ctrl+悬停抓取轮询（浮条拖动入口）
constexpr DWORD kRingAnimMs = 400;     // 过渡时长：临界阻尼感（无过冲），只发生在刷新瞬间
constexpr UINT kGrabPollMs = 120;      // 抓取轮询周期：键态延迟上限，CPU 代价可忽略

LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Overlay* self = (Overlay*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
        case WM_NCHITTEST:
            // 抓取态（Ctrl+悬停）与拖动中接收鼠标；其余恒穿透
            if (self && (self->IsGrabbable() || self->IsDragging())) return HTCLIENT;
            return HTTRANSPARENT;
        case WM_LBUTTONDOWN:
            if (self && self->BeginDrag(l)) return 0;
            return 0;
        case WM_MOUSEMOVE:
            if (self) self->DragMove(l);
            return 0;
        case WM_LBUTTONUP:
            if (self && self->EndDrag()) return 0;
            return 0;
        case WM_CAPTURECHANGED:
            if (self) self->OnCaptureLost();
            return 0;
        case WM_SETCURSOR:
            // 抓取态给移动光标提示可拖（普通态穿透收不到本消息）
            if (self && (self->IsGrabbable() || self->IsDragging())) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                return TRUE;
            }
            return DefWindowProcW(h, m, w, l);
        case WM_TIMER:
            if (self && w == kRingAnimTimer) {
                self->TickRingAnimation();
                return 0;
            }
            if (self && w == kGrabTimer) {
                self->TickGrab();
                return 0;
            }
            return DefWindowProcW(h, m, w, l);
        case WM_DPICHANGED:
            // Per-Monitor V2：DPI 变化 → 重新测量定位（§10.3）
            if (self) {
                int dpi = HIWORD(w);
                self->OnDpiChanged(dpi);
                RECT* rc = (RECT*)l;
                if (rc) SetWindowPos(h, nullptr, rc->left, rc->top, 0, 0,
                                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        case WM_ERASEBKGND:
            return 1; // 自绘
        default:
            return DefWindowProcW(h, m, w, l);
    }
}

// 顶向下 32bpp PARGB DIB + 兼容 DC（Paint/PaintRings 共用的表面帮手，RAII 释放）。
// 预乘像素由 GDI+ 直接绘制；背景清零 = Alpha 0 全透明。构造失败 ok=false（调用方直接返回）。
struct UlwSurface {
    HDC memdc = nullptr;
    HBITMAP dib = nullptr;
    void* bits = nullptr;
    int w = 0, h = 0;
    HGDIOBJ old = nullptr;
    bool ok = false;
    explicit UlwSurface(int width, int height) : w(width), h(height) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = width;
        bi.bmiHeader.biHeight = -height; // 顶向下
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC screen = GetDC(nullptr);
        dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        memdc = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        if (dib && bits && memdc) {
            old = SelectObject(memdc, dib);
            memset(bits, 0, (size_t)width * height * 4); // 背景 Alpha=0
            ok = true;
        } else {
            Release();
        }
    }
    UlwSurface(const UlwSurface&) = delete;
    UlwSurface& operator=(const UlwSurface&) = delete;
    void Release() {
        if (memdc) {
            if (old) SelectObject(memdc, old);
            DeleteDC(memdc);
            memdc = nullptr;
        }
        if (dib) {
            DeleteObject(dib);
            dib = nullptr;
        }
        bits = nullptr;
        old = nullptr;
    }
    ~UlwSurface() { Release(); }
};
} // namespace

bool Overlay::Create(HINSTANCE hinst, HWND msg_wnd) {
    inst_ = hinst;
    msg_wnd_ = msg_wnd;
    gdiplus_ = std::make_unique<GdiPlusToken>();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = hinst;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&wc)) return false;

    // 隐藏创建；Update() 后再显示
    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kClassName, L"QuotaBar", WS_POPUP,
        CW_USEDEFAULT, CW_USEDEFAULT, 10, 10,
        nullptr, nullptr, hinst, this);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);
    UINT dpi = GetDpiForWindow(hwnd_);
    if (dpi) dpi_ = (int)dpi;
    SetTimer(hwnd_, kGrabTimer, kGrabPollMs, nullptr); // Ctrl+悬停抓取轮询（常开）
    return true;
}

void Overlay::Destroy() {
    if (hwnd_) {
        KillTimer(hwnd_, kRingAnimTimer);
        KillTimer(hwnd_, kGrabTimer);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    ring_animating_ = false;
    gdiplus_.reset();
}

Overlay::Overlay() = default;
Overlay::~Overlay() = default;

int Overlay::Scaled(int dip) const {
    return dip * dpi_ / 96;
}

void Overlay::ApplyWindowStyle() {
    if (!hwnd_) return;
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    if (grabbable_ || dragging_) {
        ex &= ~WS_EX_TRANSPARENT; // 抓取/拖动中临时取消穿透
    } else {
        ex |= WS_EX_TRANSPARENT;
    }
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
    // 抓取提示边框在绘制层处理（Paint 内虚线框）
}

RECT Overlay::TaskbarRect() {
    bool ok = false;
    return GetPrimaryTaskbarRect(ok);
}

// 拖放落位判定（纯函数）：近任务栏上缘 → 吸附（右缘偏移/间隙按松手位置反推 DIP）；
// 否则 → 自由位置（工作区 DIP 坐标）。吸附判定：任务栏在底部、窗口底缘距其上缘
// ≤32 DIP、且与任务栏水平有交集。反推公式与 ApplyPosition 锚定式严格互逆。
BarDrop ResolveBarDrop(const RECT& win, const RECT& work, const RECT& monitor,
                       const RECT& taskbar, int dpi) {
    BarDrop out;
    if (dpi <= 0) dpi = 96;
    auto dip = [&](int px) { return px * 96 / dpi; };
    const bool tb_bottom = taskbar.bottom > 0 && taskbar.top >= work.bottom - 8 &&
                           taskbar.top < work.bottom + 8;
    const bool x_overlap = win.right > taskbar.left && win.left < taskbar.right;
    const int snap_band = 32 * dpi / 96;
    const bool near_top = win.bottom <= taskbar.top + snap_band &&
                          win.bottom >= taskbar.top - snap_band;
    if (tb_bottom && x_overlap && near_top) {
        out.snap = true;
        // 锚定式 pos.x = monitor.right - w + Scaled(snap_x) → snap_x = win.right - right
        out.snap_x_dip = std::min(std::max(dip(win.right - monitor.right), -200), 200);
        // 锚定式 pos.y = tb.top - h - (4DIP + Scaled(snap_y)) → snap_y = 间隙 - 4DIP
        out.snap_y_dip = std::min(std::max(dip(taskbar.top - win.bottom) - 4, 0), 200);
        return out;
    }
    out.snap = false;
    out.free_pos_x_dip = std::max(0, dip(win.left - work.left));
    out.free_pos_y_dip = std::max(0, dip(win.top - work.top));
    return out;
}

void Overlay::ApplyPosition() {
    if (!hwnd_ || size_.cx <= 0) return;
    if (dragging_) return; // 拖动中跟手，不由锚定逻辑接管

    POINT pos{};
    if (cfg_.mode == DisplayMode::TrayOnly) {
        Hide();
        return;
    }
    HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(primary, &mi);
    // 吸附锚定：圆环恒吸附；浮条(Free)开 bar_snap 时吸附任务栏上缘右端（旧 Docked 语义）
    const bool anchored = cfg_.mode == DisplayMode::Rings ||
                          (cfg_.mode == DisplayMode::Free && cfg_.bar_snap);
    if (anchored) {
        RECT tb = TaskbarRect();
        int right = mi.rcMonitor.right;
        pos.x = right - size_.cx + Scaled(cfg_.bar_snap_x_dip);
        int gap = Scaled(4) + std::max(0, Scaled(cfg_.bar_snap_y_dip));
        if (tb.bottom > 0 && tb.top < mi.rcMonitor.bottom && tb.top >= mi.rcWork.bottom - 8) {
            // 任务栏在底部：吸附其上缘
            pos.y = tb.top - size_.cy - gap;
        } else {
            pos.y = mi.rcWork.bottom - size_.cy - gap; // 降级：工作区底
        }
        // 限制在可见范围内
        if (pos.y < mi.rcWork.top) pos.y = mi.rcWork.top;
        if (pos.x < mi.rcWork.left) pos.x = mi.rcWork.left;
    } else { // Free 自由位置
        pos.x = mi.rcWork.left + Scaled(cfg_.free_pos_x_dip);
        pos.y = mi.rcWork.top + Scaled(cfg_.free_pos_y_dip);
        // 限制在可见范围内
        if (pos.x + size_.cx > mi.rcWork.right) pos.x = mi.rcWork.right - size_.cx;
        if (pos.y + size_.cy > mi.rcWork.bottom) pos.y = mi.rcWork.bottom - size_.cy;
        if (pos.x < mi.rcWork.left) pos.x = mi.rcWork.left;
        if (pos.y < mi.rcWork.top) pos.y = mi.rcWork.top;
    }
    saved_pos_ = pos;
    // 显示标志按 visible_ 决定：隐藏态重定位不得点亮窗口，否则 visible_ 与窗口态失配，
    // Hide() 从此失效（嵌入模式幽灵浮条根因，2026-09-16 实测修复）
    SetWindowPos(hwnd_, HWND_TOPMOST, pos.x, pos.y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE |
                     (visible_ ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
}

// ---------- 文本测量与绘制 ----------
std::pair<int, int> Overlay::MeasureModel(const DisplayModel& model, int font_px, int dpi) {
    (void)dpi;
    // GDI+ 已由成员 gdiplus_ 常驻持有（Create/Destroy 配对），无需每次 Startup/Shutdown
    Gdiplus::Font font(L"Segoe UI", (Gdiplus::REAL)font_px * 72.0f / 96.0f,
                       Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
    Gdiplus::Bitmap bmp(1, 1, PixelFormat32bppPARGB);
    Gdiplus::Graphics g(&bmp);
    int max_w = 0, line_h = 0;
    for (const auto& line : model.lines) {
        Gdiplus::REAL w = 0;
        for (const auto& seg : line) {
            Gdiplus::RectF box;
            g.MeasureString(seg.text.c_str(), (INT)seg.text.size(), &font,
                            Gdiplus::RectF(0, 0, 4096, 512), &box);
            w += box.Width;
            line_h = std::max(line_h, (int)box.Height);
        }
        max_w = std::max(max_w, (int)std::ceil(w));
    }
    int total_h = (int)std::ceil((double)line_h * model.lines.size() * 1.15);
    return {max_w, std::max(total_h, 1)};
}

void Overlay::Paint() {
    if (cfg_.mode == DisplayMode::Rings) {
        PaintRings();
        return;
    }
    if (!hwnd_) return;
    int font_px = std::max(12, Scaled(cfg_.font_size_pt * 96 / 72));
    const auto [w, h] = MeasureModel(model_, font_px, dpi_);
    int pad_x = Scaled(6), pad_y = Scaled(2);
    int total_w = w + pad_x * 2, total_h = h + pad_y * 2;
    if (total_w <= 0 || total_h <= 0) return;
    total_w = std::min(total_w, 2048);
    total_h = std::min(total_h, 512);
    size_ = {total_w, total_h};

    // 32bpp 顶向下 DIB，PARGB（预乘）由 GDI+ 直接绘制
    UlwSurface surf(total_w, total_h);
    if (!surf.ok) return;

    {
        Gdiplus::Bitmap bmp(surf.w, surf.h, 4 * surf.w, PixelFormat32bppPARGB,
                            (BYTE*)surf.bits);
        Gdiplus::Graphics g(&bmp);
        // 灰度抗锯齿（非 ClearType：避免透明背景上的子像素彩边，§10.1）
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        // 字号与 MeasureModel 同源（font_px 已含 dpi 缩放，px→pt 回退）：DIB Graphics
        // 恒为 96 DPI，裸 UnitPoint 只会渲染 pt*4/3 像素——高 DPI 下字比测量的窗口小
        // dpi/96 倍（150% 缩到 2/3、200% 缩到 1/2，实测口径差）
        Gdiplus::Font font(L"Segoe UI", (Gdiplus::REAL)font_px * 72.0f / 96.0f,
                           Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
        Gdiplus::StringFormat sf(Gdiplus::StringFormat::GenericTypographic());

        Gdiplus::REAL y = (Gdiplus::REAL)pad_y;
        // 行高直接取上方 MeasureModel 的结果（避免同参数重复测量）
        Gdiplus::REAL line_h = (Gdiplus::REAL)h / (Gdiplus::REAL)std::max(1, (int)model_.lines.size());
        // 文字浮在任意壁纸上：柔和投影保底可读性（圆环模式已有垫盘，文字模式无衬底）
        Gdiplus::SolidBrush shadow_br(Gdiplus::Color(70, 0, 0, 0));
        const Gdiplus::REAL shadow_dy = (Gdiplus::REAL)std::max(1, Scaled(1));
        for (const auto& line : model_.lines) {
            Gdiplus::REAL x = (Gdiplus::REAL)pad_x;
            for (const auto& seg : line) {
                Gdiplus::RectF box;
                g.MeasureString(seg.text.c_str(), (INT)seg.text.size(), &font,
                                Gdiplus::RectF(0, 0, 4096, 512), &box);
                Gdiplus::SolidBrush br(Gdiplus::Color(255, GetRValue(seg.color),
                                                       GetGValue(seg.color), GetBValue(seg.color)));
                Gdiplus::RectF layout(x, y, box.Width + 4, box.Height + 4);
                Gdiplus::RectF shadow_layout(x, y + shadow_dy, box.Width + 4, box.Height + 4);
                g.DrawString(seg.text.c_str(), (INT)seg.text.size(), &font, shadow_layout,
                             &sf, &shadow_br);
                g.DrawString(seg.text.c_str(), (INT)seg.text.size(), &font, layout, &sf, &br);
                x += box.Width;
            }
            y += line_h * 1.15f;
        }
        if (grabbable_ || dragging_) {
            // 抓取/拖动提示：虚线边框（拖完即存，无其他仪式 UI）
            Gdiplus::Pen pen(Gdiplus::Color(220, 80, 160, 255), 1.5f);
            Gdiplus::REAL dash[] = {4, 3};
            pen.SetDashPattern(dash, 2);
            g.DrawRectangle(&pen, 1, 1, total_w - 3, total_h - 3);
        }
    }

    // UpdateLayeredWindow 只提交小窗大小的图像
    PresentSurface_(surf.memdc);
}

// ---------- 圆环模式绘制 ----------
namespace {
// 圆角矩形路径（GDI+ 无原语）
void MakeRoundRect(Gdiplus::GraphicsPath& p, Gdiplus::REAL x, Gdiplus::REAL y,
                   Gdiplus::REAL w, Gdiplus::REAL h, Gdiplus::REAL r) {
    Gdiplus::REAL d = 2 * r;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}
Gdiplus::Color Gac(COLORREF c, int alpha) {
    return Gdiplus::Color((BYTE)alpha, GetRValue(c), GetGValue(c), GetBValue(c));
}
} // namespace

// ULW 提交共用段：整帧 BitBlt 语义上屏；返回是否成功
bool Overlay::PresentSurface_(HDC memdc) {
    POINT src_pt{0, 0};
    POINT dst = saved_pos_;
    if (dst.x == 0 && dst.y == 0) dst = {100, 100};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    HDC hdc = GetDC(hwnd_);
    BOOL posted = UpdateLayeredWindow(hwnd_, hdc, &dst, &size_, memdc, &src_pt, 0, &blend,
                                      ULW_ALPHA);
    ReleaseDC(hwnd_, hdc);
    // 提交失败不置位：保持 has_bitmap_=false 让下一次 Update 强制重画自愈，
    // 否则失败帧后一直显示陈旧表面
    if (posted) has_bitmap_ = true;
    return posted != FALSE;
}

void Overlay::PaintRings() {
    if (!hwnd_ || rings_.empty()) return;
    const int n = (int)rings_.size();
    const int pad = Scaled(4);
    const int stick = Scaled(10);         // 徽章中心压在环顶:上探 stick 像素
    const int ring_d = Scaled(44);        // 环外径
    const int stroke = Scaled(6);         // 环厚
    const int col_w = Scaled(58);
    const int col_gap = Scaled(6);
    const int test_h = test_label_ ? Scaled(15) : 0;
    int total_w = std::min(n * col_w + (n - 1) * col_gap + pad * 2, 2048);
    int total_h = std::min(test_h + stick + ring_d + Scaled(6), 512);
    if (total_w <= 0 || total_h <= 0) return;
    size_ = {total_w, total_h};

    UlwSurface surf(total_w, total_h);
    if (!surf.ok) return;

    {
        Gdiplus::Bitmap bmp(surf.w, surf.h, 4 * surf.w, PixelFormat32bppPARGB,
                            (BYTE*)surf.bits);
        Gdiplus::Graphics g(&bmp);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::StringFormat csf(Gdiplus::StringFormat::GenericTypographic());
        csf.SetAlignment(Gdiplus::StringAlignmentCenter);
        csf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        // 环心数值用:无对齐(位置手动计算),配合 MeasureString 避免矩形裁剪
        Gdiplus::StringFormat plain(Gdiplus::StringFormat::GenericTypographic());

        for (int i = 0; i < n; ++i) {
            const RingItem& it = rings_[i];
            const int alpha = (it.stale || it.paused) ? 130 : 255;
            // 动画期间的显示值（无过渡时与 arc_percent 一致）
            const float arc = (i < (int)ring_disp_.size()) ? ring_disp_[i]
                                                           : (float)it.arc_percent;
            const Gdiplus::REAL cx = (Gdiplus::REAL)(pad + i * (col_w + col_gap) + col_w / 2);
            const Gdiplus::REAL ry = (Gdiplus::REAL)(test_h + stick);
            const Gdiplus::RectF rr((Gdiplus::REAL)cx - ring_d / 2.0f + stroke / 2.0f, ry + stroke / 2.0f,
                                    (Gdiplus::REAL)(ring_d - stroke), (Gdiplus::REAL)(ring_d - stroke));
            // 垫底圆盘:圆环自包含,任何壁纸(亮/暗)上都清晰
            Gdiplus::SolidBrush disc(Gdiplus::Color(120, 10, 10, 12));
            Gdiplus::REAL disc_r = ring_d / 2.0f + Scaled(5);
            g.FillEllipse(&disc, cx - disc_r, ry - (Gdiplus::REAL)Scaled(5), disc_r * 2,
                          disc_r * 2);
            // 轨道
            Gdiplus::Pen track_pen(Gac(it.track_color, 90), (Gdiplus::REAL)stroke);
            g.DrawEllipse(&track_pen, rr);
            // 状态弧:自环顶顺时针,扫过比例=剩余
            if (arc > 0) {
                Gdiplus::Pen vp(Gac(it.value_color, alpha), (Gdiplus::REAL)stroke);
                g.DrawArc(&vp, rr, -90.0f, 360.0f * arc / 100.0f);
            } else if (it.full_ring) {
                Gdiplus::Pen vp(Gac(it.value_color, alpha), (Gdiplus::REAL)stroke);
                g.DrawEllipse(&vp, rr);
            }
            // 环心数值:自适应字号——统一基准起步,宽/高超出内圆安全区(内径×0.70)就
            // 逐级降到放得下(下限 6 DIP)。旧">2 字符用小号"一刀切让相邻环字号参差
            // ("1%"比"¥21"大),"¥21"等三字符在部分 DPI 还会触到环内缘(实测 2026-09-22)。
            if (!it.center.empty()) {
                const float inner_d = (float)(ring_d - 2 * stroke);
                const float safe = inner_d * 0.70f;
                const float min_px = (float)Scaled(6);
                float cpx = (float)Scaled(10); // 基准字号(渲染 px,DIB Graphics 恒 96 DPI)
                Gdiplus::RectF box;
                for (;;) { // Font 不可赋值:循环内构造测,出循环后按落定字号重建
                    Gdiplus::Font cfont(L"Segoe UI", (Gdiplus::REAL)cpx * 72.0f / 96.0f,
                                        Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
                    g.MeasureString(it.center.c_str(), (INT)it.center.size(), &cfont,
                                    Gdiplus::RectF(0, 0, 4096, 512), &plain, &box);
                    if ((box.Width <= safe && box.Height <= safe) || cpx <= min_px) break;
                    cpx = std::max(min_px, cpx - (float)Scaled(1) * 0.5f);
                }
                const Gdiplus::Font cfont(L"Segoe UI", (Gdiplus::REAL)cpx * 72.0f / 96.0f,
                                          Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
                Gdiplus::SolidBrush cb(Gac(it.value_color, alpha));
                g.DrawString(it.center.c_str(), (INT)it.center.size(), &cfont,
                             Gdiplus::PointF(cx - box.Width / 2.0f,
                                             ry + ring_d / 2.0f - box.Height / 2.0f),
                             &plain, &cb);
            }
            // 产品徽章:圆角方块压环顶
            const Gdiplus::REAL b = (Gdiplus::REAL)Scaled(20);
            const Gdiplus::REAL bx = cx - b / 2.0f, by = (Gdiplus::REAL)test_h + (Gdiplus::REAL)stick - b / 2.0f;
            Gdiplus::GraphicsPath rrp;
            MakeRoundRect(rrp, bx, by, b, b, b / 3.2f);
            Gdiplus::SolidBrush badge_br(Gac(it.accent, alpha));
            g.FillPath(&badge_br, &rrp);
            Gdiplus::Pen outline(Gac(it.value_color, 110), 1.0f);
            g.DrawPath(&outline, &rrp);
            // 原厂图标模式：芯片内改绘厂商图标（随状态 alpha；未就绪回落字母）
            Gdiplus::Bitmap* pic = cfg_.embed_brand_icons
                                       ? ProviderGpBitmap(it.type)
                                       : nullptr;
            if (pic) {
                const Gdiplus::REAL inset = b * 0.15f;
                const Gdiplus::REAL side = b - inset * 2.0f;
                const Gdiplus::REAL am = (Gdiplus::REAL)alpha / 255.0f;
                Gdiplus::ColorMatrix amx{{
                    {1, 0, 0, 0, 0},
                    {0, 1, 0, 0, 0},
                    {0, 0, 1, 0, 0},
                    {0, 0, 0, am, 0},
                }};
                Gdiplus::ImageAttributes ia;
                ia.SetColorMatrix(&amx);
                g.DrawImage(pic, Gdiplus::RectF(bx + inset, by + inset, side, side),
                            0, 0, (Gdiplus::REAL)pic->GetWidth(),
                            (Gdiplus::REAL)pic->GetHeight(), Gdiplus::UnitPixel, &ia);
            } else {
                wchar_t letter[2] = {it.badge, 0};
                Gdiplus::Font bfont(L"Segoe UI", (Gdiplus::REAL)Scaled(11) * 72.0f / 96.0f,
                                    Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
                Gdiplus::SolidBrush lb(Gdiplus::Color((BYTE)alpha, 255, 255, 255));
                g.DrawString(letter, 1, &bfont, Gdiplus::RectF(bx, by - Scaled(1), b, b), &csf, &lb);
            }
        }
        if (test_label_) {
            Gdiplus::Font tf(L"Segoe UI", (Gdiplus::REAL)Scaled(9) * 72.0f / 96.0f,
                             Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
            Gdiplus::SolidBrush tb(Gdiplus::Color(255, 255, 176, 32));
            g.DrawString(L"[测试]", 4, &tf,
                         Gdiplus::RectF((Gdiplus::REAL)pad, 0,
                                        (Gdiplus::REAL)Scaled(60), (Gdiplus::REAL)test_h),
                         &csf, &tb);
        }
        if (grabbable_ || dragging_) {
            Gdiplus::Pen pen(Gdiplus::Color(220, 80, 160, 255), 1.5f);
            Gdiplus::REAL dash[] = {4, 3};
            pen.SetDashPattern(dash, 2);
            g.DrawRectangle(&pen, 1, 1, total_w - 3, total_h - 3);
        }
    }

    PresentSurface_(surf.memdc);
}

void Overlay::Update(const DisplayModel& model, const DisplayConfig& cfg) {
    DisplayConfig old = cfg_;
    cfg_ = cfg;
    bool model_changed = !(model == model_);
    model_ = model;
    if (!hwnd_ || suppressed_ || cfg_.mode == DisplayMode::TrayOnly) {
        Hide();
        return;
    }
    bool style_changed = old.mode != cfg_.mode || old.font_size_pt != cfg_.font_size_pt ||
                         old.two_lines != cfg_.two_lines;
    if (model_changed || style_changed || !has_bitmap_) {
        Paint();
    }
    ApplyPosition();
    if (!visible_) Show();
}

void Overlay::UpdateRings(const std::vector<RingItem>& items, const DisplayConfig& cfg,
                          bool test_label) {
    DisplayConfig old = cfg_;
    cfg_ = cfg;
    bool changed = !(items == rings_) || test_label != test_label_;
    rings_ = items;
    test_label_ = test_label;
    SetupRingAnimation(items);
    if (!hwnd_ || suppressed_ || cfg_.mode == DisplayMode::TrayOnly || rings_.empty()) {
        Hide();
        return;
    }
    bool style_changed = old.mode != cfg_.mode || old.font_size_pt != cfg_.font_size_pt;
    if (changed || style_changed || !has_bitmap_ || ring_animating_) {
        Paint(); // Paint 内按模式路由到 PaintRings
    }
    ApplyPosition();
    if (!visible_) Show();
}

// ---------- 圆环弧值过渡动画 ----------
// 数据刷新时弧线从"当前显示值"出发滑向新值（而非瞬跳）：动画从呈现值出发，
// 结束值即新数据；跟随系统"减少动态效果"，关闭时直接落位。无比例语义项
// （arc_percent<0，如金额满环）不参与动画。

void Overlay::SetupRingAnimation(const std::vector<RingItem>& items) {
    const size_t n = items.size();
    if (!hwnd_ || ring_disp_.size() != n) {
        // 首次/数量变化（开关平台）：直接对齐目标，不做过渡
        if (hwnd_) KillTimer(hwnd_, kRingAnimTimer);
        ring_animating_ = false;
        ring_disp_.resize(n);
        ring_from_.resize(n);
        for (size_t i = 0; i < n; ++i)
            ring_disp_[i] = ring_from_[i] = (float)items[i].arc_percent;
        return;
    }
    if (!theme::AnimationsEnabled()) {
        KillTimer(hwnd_, kRingAnimTimer);
        ring_animating_ = false;
        for (size_t i = 0; i < n; ++i) ring_disp_[i] = ring_from_[i] = (float)items[i].arc_percent;
        return;
    }
    bool need = false;
    for (size_t i = 0; i < n; ++i) {
        const float target = (float)items[i].arc_percent;
        if (target >= 0 && ring_disp_[i] >= 0 && std::fabs(target - ring_disp_[i]) > 0.01f) {
            need = true;
            break;
        }
    }
    if (!need) {
        KillTimer(hwnd_, kRingAnimTimer);
        ring_animating_ = false;
        for (size_t i = 0; i < n; ++i) ring_disp_[i] = (float)items[i].arc_percent;
        return;
    }
    // 起点=当前呈现值；无比例语义项直接落位
    for (size_t i = 0; i < n; ++i) {
        ring_from_[i] = ring_disp_[i];
        if (items[i].arc_percent < 0 || ring_from_[i] < 0)
            ring_disp_[i] = (float)items[i].arc_percent;
    }
    ring_anim_tick_ = GetTickCount();
    if (!ring_animating_) {
        ring_animating_ = true;
        SetTimer(hwnd_, kRingAnimTimer, 16, nullptr);
    }
}

void Overlay::TickRingAnimation() {
    if (!hwnd_ || !ring_animating_) {
        if (hwnd_) KillTimer(hwnd_, kRingAnimTimer);
        ring_animating_ = false;
        return;
    }
    if (suppressed_ || cfg_.mode != DisplayMode::Rings || rings_.empty() ||
        rings_.size() != ring_disp_.size()) {
        KillTimer(hwnd_, kRingAnimTimer); // 抑制(嵌入)/模式切走/内容清空：过渡失去意义
        ring_animating_ = false;
        return;
    }
    const DWORD elapsed = GetTickCount() - ring_anim_tick_;
    const float t = elapsed >= kRingAnimMs ? 1.0f : (float)elapsed / (float)kRingAnimMs;
    const float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); // ease-out cubic
    for (size_t i = 0; i < rings_.size(); ++i) {
        const float target = (float)rings_[i].arc_percent;
        if (target < 0 || ring_from_[i] < 0) continue; // 已直接落位
        ring_disp_[i] = ring_from_[i] + (target - ring_from_[i]) * eased;
    }
    if (t >= 1.0f) {
        KillTimer(hwnd_, kRingAnimTimer);
        ring_animating_ = false;
    }
    Paint(); // Rings 模式下路由到 PaintRings，画当前显示值
}

void Overlay::Relayout() {
    if (!hwnd_) return;
    UINT dpi = GetDpiForWindow(hwnd_);
    if (dpi) dpi_ = (int)dpi;
    has_bitmap_ = false;
    // 抑制期(嵌入)/仅托盘：不绘制不定位，防止事件驱动的重定位把浮窗点亮
    if (suppressed_ || cfg_.mode == DisplayMode::TrayOnly) return;
    Paint();
    ApplyPosition();
}

void Overlay::OnDpiChanged(int dpi) {
    if (dpi > 0) dpi_ = dpi;
    Relayout();
}

void Overlay::OnTaskbarCreated() {
    // Explorer 重启：恢复 Z 序与位置；不重置查询退避
    Relayout();
}

void Overlay::OnDisplayChange() {
    Relayout();
}

void Overlay::OnFullscreenCheck() {
    if (!hwnd_ || dragging_ || grabbable_ || suppressed_ || !has_bitmap_) return;
    if (cfg_.mode == DisplayMode::TrayOnly) return;
    // 全屏判断：验证前台窗口实际覆盖监视器区域（§10.3），最大化不算
    POINT pt = saved_pos_;
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (!mon) return;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    HWND fg = GetForegroundWindow();
    if (!fg || fg == hwnd_) {
        Show();
        return;
    }
    RECT fr{};
    if (!GetWindowRect(fg, &fr)) return;
    bool fullscreen = fr.left <= mi.rcMonitor.left && fr.right >= mi.rcMonitor.right &&
                      fr.top <= mi.rcMonitor.top && fr.bottom >= mi.rcMonitor.bottom;
    // 排除桌面/Shell 窗口
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    if (fullscreen && wcscmp(cls, L"Progman") != 0 && wcscmp(cls, L"WorkerW") != 0 &&
        wcscmp(cls, L"Shell_TrayWnd") != 0) {
        // 必须走 Hide() 状态机：直调 ShowWindow(SW_HIDE) 不更新 visible_，
        // 之后 Show() 的守卫短路、Update 的 !visible_ 补显也短路，浮窗只能
        // 等下一次前台事件自救（visible_ 与窗口态失配，CLAUDE.md 状态机不变量）
        Hide();
    } else {
        Show();
    }
}

// ---------- Ctrl+拖动直接移动（2026-09-21 取代“进入调整”仪式流，拖完即存） ----------
// 穿透窗口收不到鼠标：轮询计时器在“Ctrl 按下且光标悬停浮条”时临时取消穿透并画
// 虚线框；按下即拖，松手按落位判定持久化（近任务栏自动吸附，否则自由位置）。
void Overlay::TickGrab() {
    if (!hwnd_ || !visible_ || suppressed_ || dragging_ ||
        cfg_.mode != DisplayMode::Free) {
        if (grabbable_) {
            grabbable_ = false;
            ApplyWindowStyle();
            Paint();
        }
        return;
    }
    bool want = false;
    if (GetAsyncKeyState(VK_CONTROL) < 0) {
        POINT pt{};
        RECT rc{};
        if (GetCursorPos(&pt) && GetWindowRect(hwnd_, &rc))
            want = PtInRect(&rc, pt) != FALSE;
    }
    if (want != grabbable_) {
        grabbable_ = want;
        ApplyWindowStyle();
        Paint();
    }
}

bool Overlay::BeginDrag(LPARAM lparam) {
    (void)lparam;
    if (!hwnd_ || !grabbable_ || dragging_ || !visible_ || suppressed_ ||
        cfg_.mode != DisplayMode::Free)
        return false;
    dragging_ = true;
    POINT cur{};
    RECT rc{};
    if (GetCursorPos(&cur) && GetWindowRect(hwnd_, &rc))
        grab_off_ = {cur.x - rc.left, cur.y - rc.top};
    else
        grab_off_ = {0, 0};
    SetCapture(hwnd_);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); // 拖动期保持最顶
    Paint(); // 虚线框提示
    return true;
}

void Overlay::DragMove(LPARAM lparam) {
    (void)lparam;
    if (!dragging_ || !hwnd_) return;
    POINT cur{};
    if (!GetCursorPos(&cur)) return;
    saved_pos_ = {cur.x - grab_off_.x, cur.y - grab_off_.y};
    SetWindowPos(hwnd_, nullptr, saved_pos_.x, saved_pos_.y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

bool Overlay::EndDrag() {
    if (!dragging_ || !hwnd_) return false;
    dragging_ = false;
    if (GetCapture() == hwnd_) ReleaseCapture(); // 同步派发 WM_CAPTURECHANGED（幂等复位）
    RECT win{}, tb{};
    GetWindowRect(hwnd_, &win);
    HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(primary, &mi);
    tb = TaskbarRect();
    const BarDrop drop = ResolveBarDrop(win, mi.rcWork, mi.rcMonitor, tb, dpi_);
    cfg_.bar_snap = drop.snap;
    cfg_.free_pos_x_dip = drop.free_pos_x_dip;
    cfg_.free_pos_y_dip = drop.free_pos_y_dip;
    cfg_.bar_snap_x_dip = drop.snap_x_dip;
    cfg_.bar_snap_y_dip = drop.snap_y_dip;
    grabbable_ = false; // Ctrl 仍按住时下一个轮询周期自然重回抓取态
    ApplyWindowStyle();
    Paint();
    ApplyPosition(); // 吸附落位立即呈现（自由位置=工作区钳制后的原地位移）
    if (adjust_cb_) adjust_cb_(true, cfg_);
    return true;
}

void Overlay::OnCaptureLost() {
    if (!dragging_) return;
    dragging_ = false;
    grabbable_ = false;
    ApplyWindowStyle();
    Paint();
}

void Overlay::SetSuppressed(bool on) {
    if (suppressed_ == on) return;
    suppressed_ = on;
    if (on) Hide(); // 进入抑制立即下屏；退出抑制不自动上屏，等下一次 Update
}

void Overlay::Show() {
    // 抑制期(嵌入模式)任何路径不得点亮浮窗；无位图时不得置位 visible_——
    // 否则"标志已真、窗口未亮"，Update 的 !visible_ 补显分支从此短路
    if (!hwnd_ || visible_ || suppressed_) return;
    if (cfg_.mode == DisplayMode::TrayOnly || !has_bitmap_) return;
    visible_ = true;
    SetWindowPos(hwnd_, HWND_TOPMOST, saved_pos_.x, saved_pos_.y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}
void Overlay::Hide() {
    // 以窗口实际可见性为准：visible_ 标志一旦与窗口态失配（历史缺陷：ApplyPosition 带
    // SWP_SHOWWINDOW 绕过状态机点亮窗口，标志仍为 false），守卫式写法会让 Hide 永久失效，
    // 浮窗在嵌入模式下再也无法隐藏
    if (!hwnd_) return;
    visible_ = false;
    if (IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_HIDE);
}

} // namespace qb
