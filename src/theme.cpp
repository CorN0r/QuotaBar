// theme.cpp — 设计 token 与主题原语实现（见 theme.hpp）。
// 按钮 = 手动子类化(SetProp 保存原过程,不依赖 comctl32 v6 的 SetWindowSubclass)，含键盘焦点环；
// 窗口圆角/深色标题栏 = DWM 属性(Win11 生效,旧系统静默忽略)；
// 暗色菜单 = uxtheme SetPreferredAppMode(1809+, undocumented,失败静默)。
#include "theme.hpp"
#include <objidl.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <cwchar>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif
#ifndef UISF_HIDEFOCUS
#define UISF_HIDEFOCUS 0x1
#endif

namespace qb {
namespace theme {

namespace {

// ---- 双主题调色板（F1 迭代 2026-09-15）----
// 深色 = stitch 设计稿原值（旧默认），浅色 = Material3 亮色系映射（对比度同阶）。
int g_theme_gen = 0;
bool g_dark = true;

void ApplyDarkPalette() {
    kWindowBg = RGB(30, 31, 36);
    kCardBg = RGB(38, 40, 46);
    kPanelBg = RGB(24, 25, 29);
    kBorder = RGB(58, 62, 72);
    kText = RGB(232, 234, 237);
    kSubText = RGB(154, 160, 170);
    kAccent = RGB(96, 141, 255);
    kOk = RGB(64, 201, 180);
    kWarn = RGB(255, 176, 32);
    kBad = RGB(255, 92, 92);
    kBtnNormal = RGB(43, 46, 53);
    kBtnHover = RGB(55, 59, 69);
    kBtnDown = RGB(32, 35, 41);
    kBtnBorder = RGB(58, 62, 72);
    kBtnTextOff = RGB(108, 112, 121);
    kBtnDefNormal = RGB(60, 96, 190);
    kBtnDefHover = RGB(74, 112, 214);
    kBtnDefDown = RGB(52, 84, 168);
    // ---- stitch_minimalist 深色（稿件原值，2026-09-17）----
    kSbg = RGB(0x0B, 0x0F, 0x17);
    kSSurfLow = RGB(0x0E, 0x13, 0x1F);
    kSSurf = RGB(0x14, 0x1B, 0x2B);
    kSSurfHigh = RGB(0x18, 0x22, 0x36);
    kSSurfHigh2 = RGB(0x20, 0x2B, 0x44);
    kSSurfLow2 = RGB(0x07, 0x0B, 0x12);
    kSOnSurf = RGB(0xED, 0xF1, 0xF7);
    kSOnSurfVar = RGB(0x9A, 0xA6, 0xBC);
    kSOutline = RGB(0x64, 0x74, 0x8B);
    kSOutlineVar = RGB(0x33, 0x3D, 0x52);
    kSPrimary = RGB(0x60, 0xA5, 0xFA);
    kSPrimCont = RGB(0x25, 0x63, 0xEB);
    kSOnPrimCont = RGB(0xFF, 0xFF, 0xFF);
    kSPrimaryFix = RGB(0xD8, 0xE2, 0xFF);
    kSPrimFixHover = RGB(0xC3, 0xD6, 0xFF);
    kSPrimFixDown = RGB(0x9F, 0xBC, 0xEF);
    kSSecondary = RGB(0x34, 0xD3, 0x99);
    kSSecCont = RGB(0x05, 0x96, 0x69);
    kSTertiary = RGB(0xFB, 0xBF, 0x24);
    kSTertCont = RGB(0xB4, 0x53, 0x09);
    kSError = RGB(0xF8, 0x71, 0x71);
    kSErrCont = RGB(0x7F, 0x1D, 0x1D);
    kSCardBorder = RGB(0x27, 0x2E, 0x3E);
    kSBorderSoft = RGB(0x1D, 0x24, 0x34);
    kSInset = RGB(0x0C, 0x12, 0x20);
    kSGridLine = RGB(0x1C, 0x22, 0x30);
}

void ApplyLightPalette() {
    kWindowBg = RGB(243, 245, 249);
    kCardBg = RGB(252, 253, 255);
    kPanelBg = RGB(255, 255, 255);
    kBorder = RGB(214, 219, 228);
    kText = RGB(31, 34, 40);
    kSubText = RGB(95, 102, 114);
    kAccent = RGB(43, 90, 208);
    kOk = RGB(0, 148, 128);
    kWarn = RGB(178, 116, 0);
    kBad = RGB(211, 47, 47);
    kBtnNormal = RGB(240, 242, 247);
    kBtnHover = RGB(228, 232, 239);
    kBtnDown = RGB(216, 221, 231);
    kBtnBorder = RGB(214, 219, 228);
    kBtnTextOff = RGB(163, 168, 178);
    kBtnDefNormal = RGB(43, 90, 208);
    kBtnDefHover = RGB(56, 105, 224);
    kBtnDefDown = RGB(36, 78, 186);
    kSbg = RGB(0xF7, 0xF9, 0xFC);
    kSSurfLow = RGB(0xEF, 0xF2, 0xF8);
    kSSurf = RGB(0xFF, 0xFF, 0xFF);
    kSSurfHigh = RGB(0xF2, 0xF5, 0xFB);
    kSSurfHigh2 = RGB(0xE2, 0xE8, 0xF3);
    kSSurfLow2 = RGB(0xFF, 0xFF, 0xFF);
    kSOnSurf = RGB(0x17, 0x20, 0x2F);
    kSOnSurfVar = RGB(0x5A, 0x65, 0x80);
    kSOutline = RGB(0x6E, 0x78, 0x91);
    kSOutlineVar = RGB(0xCB, 0xD3, 0xE4);
    kSPrimary = RGB(0x2B, 0x55, 0xC4);
    kSPrimCont = RGB(0xAD, 0xC6, 0xFF);
    kSOnPrimCont = RGB(0x00, 0x28, 0x5D);
    kSPrimaryFix = RGB(0xD8, 0xE2, 0xFF);
    kSPrimFixHover = RGB(0xC7, 0xD9, 0xFE);
    kSPrimFixDown = RGB(0xB3, 0xCB, 0xF5);
    kSSecondary = RGB(0x04, 0x78, 0x57);
    kSSecCont = RGB(0xA7, 0xF3, 0xD0);
    kSTertiary = RGB(0x92, 0x61, 0x0A);
    kSTertCont = RGB(0xFD, 0xE6, 0x8A);
    kSError = RGB(0xDC, 0x26, 0x26);
    kSErrCont = RGB(0xFE, 0xE2, 0xE2);
    kSCardBorder = RGB(0xE1, 0xE6, 0xF0);
    kSBorderSoft = RGB(0xEC, 0xF0, 0xF7);
    kSInset = RGB(0xF2, 0xF5, 0xFA);
    kSGridLine = RGB(0xE3, 0xE8, 0xF1);
}

// 主题相关静态画刷按代数重建（否则主题切换后 EDIT 底色留在旧色）。
// gen 必须随 cache 成对存储：函数级 static gen 服务多个缓存时，主题切换后先到的
// 调用方会把 gen 盖到最新值，后到的缓存看到"gen 已新"而永不重建（输入框残留旧底色）。
struct ThemedBrushCache {
    HBRUSH brush = nullptr;
    int gen = -1;
    COLORREF color = 0;
};
HBRUSH ThemedSolidBrush(ThemedBrushCache& cache, COLORREF color) {
    if (cache.gen != g_theme_gen || cache.color != color || !cache.brush) {
        if (cache.brush) DeleteObject(cache.brush);
        cache.brush = CreateSolidBrush(color);
        cache.gen = g_theme_gen;
        cache.color = color;
    }
    return cache.brush;
}

ULONG_PTR GdiplusOnce() {
    static ULONG_PTR token = 0;
    static bool done = false;
    if (!done) {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&token, &input, nullptr);
        done = true;
    }
    return token;
}

struct GdiplusSession {
    GdiplusSession() { GdiplusOnce(); }
};

Gdiplus::Color Gac(COLORREF c, int alpha) {
    return Gdiplus::Color((BYTE)alpha, GetRValue(c), GetGValue(c), GetBValue(c));
}

void RoundRectPath(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rc, Gdiplus::REAL r) {
    Gdiplus::REAL d = 2 * r;
    if (d > rc.Width) d = rc.Width;
    if (d > rc.Height) d = rc.Height;
    path.AddArc(rc.X, rc.Y, d, d, 180, 90);
    path.AddArc(rc.X + rc.Width - d, rc.Y, d, d, 270, 90);
    path.AddArc(rc.X + rc.Width - d, rc.Y + rc.Height - d, d, d, 0, 90);
    path.AddArc(rc.X, rc.Y + rc.Height - d, d, d, 90, 90);
    path.CloseFigure();
}

void FillRoundRect(Gdiplus::Graphics& g, const Gdiplus::RectF& rc, Gdiplus::REAL r,
                   const Gdiplus::Brush& brush) {
    Gdiplus::GraphicsPath path;
    RoundRectPath(path, rc, r);
    g.FillPath(&brush, &path);
}

void StrokeRoundRect(Gdiplus::Graphics& g, const Gdiplus::RectF& rc, Gdiplus::REAL r,
                     const Gdiplus::Pen& pen) {
    Gdiplus::GraphicsPath path;
    RoundRectPath(path, rc, r);
    g.DrawPath(&pen, &path);
}

Gdiplus::Font MakeFont(float px, bool bold, const wchar_t* face = nullptr) {
    return Gdiplus::Font(face ? face : L"Segoe UI", px,
                         bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular,
                         Gdiplus::UnitPixel);
}

// StringFormat 拷贝构造为 protected，只能就地克隆泛型格式后再调 setter
void ApplyFormat(Gdiplus::StringFormat* sf, const TextStyle& ts) {
    switch (ts.align) {
        case 1: sf->SetAlignment(Gdiplus::StringAlignmentCenter); break;
        case 2: sf->SetAlignment(Gdiplus::StringAlignmentFar); break;
        default: break;
    }
    if (!ts.wrap) sf->SetFormatFlags(sf->GetFormatFlags() | Gdiplus::StringFormatFlagsNoWrap);
    sf->SetTrimming(ts.ellipsis ? Gdiplus::StringTrimmingEllipsisCharacter
                                : Gdiplus::StringTrimmingNone);
}

// ---- 自绘按钮 ----
struct BtnTheme {
    WNDPROC old = nullptr;
    bool hover = false;
    bool pressed = false;
};

constexpr wchar_t kBtnProp[] = L"qb_theme_btn";

bool IsDefaultButton(HWND h) {
    return (GetWindowLongPtrW(h, GWL_STYLE) & BS_DEFPUSHBUTTON) != 0;
}

void PaintButtonTo(HWND h, HDC hdc) {
    GdiplusSession session;
    RECT rc{};
    GetClientRect(h, &rc);
    UINT dpi = GetDpiForWindow(h);
    if (!dpi) dpi = 96; // GetDpiForWindow 失败返回 0：不得让后续字号/圆角归零
    int r = 6 * (int)dpi / 96;
    if (r < 3) r = 3;

    BtnTheme* st = (BtnTheme*)GetPropW(h, kBtnProp);
    const bool hover = st && st->hover;
    const bool pressed = st && st->pressed;
    const bool enabled = IsWindowEnabled(h) != FALSE;
    const bool def = IsDefaultButton(h);

    COLORREF fill = pressed ? kBtnDown : (hover ? kBtnHover : kBtnNormal);
    if (def && enabled) fill = pressed ? kBtnDefDown : (hover ? kBtnDefHover
                                                              : kBtnDefNormal);
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::RectF rrf((Gdiplus::REAL)rc.left + 1.0f, (Gdiplus::REAL)rc.top + 1.0f,
                       (Gdiplus::REAL)(rc.right - rc.left) - 2.0f,
                       (Gdiplus::REAL)(rc.bottom - rc.top) - 2.0f);
    Gdiplus::SolidBrush fill_br(Gac(fill, enabled ? 255 : 160));
    FillRoundRect(g, rrf, (Gdiplus::REAL)r, fill_br);
    Gdiplus::Pen border_pen(Gac(def && enabled ? kAccent : kBtnBorder,
                                enabled ? (def ? 255 : 190) : 90), 1.0f);
    StrokeRoundRect(g, rrf, (Gdiplus::REAL)r, border_pen);

    // 键盘焦点环：仅键盘导航时显示（跟随系统"隐藏焦点指示"设置）
    const bool focused = GetFocus() == h &&
                         (SendMessageW(h, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) == 0;
    if (focused && enabled) {
        Gdiplus::Pen focus_pen(Gac(RGB(150, 176, 255), 235), 2.0f);
        Gdiplus::RectF inner(rrf.X + 2.5f, rrf.Y + 2.5f, rrf.Width - 5.0f, rrf.Height - 5.0f);
        StrokeRoundRect(g, inner, (Gdiplus::REAL)r - 2.0f > 1.0f ? (Gdiplus::REAL)r - 2.0f : 1.0f,
                        focus_pen);
    }

    wchar_t text[128]{};
    GetWindowTextW(h, text, 128);
    // 像素字号（12 DIP）：UnitPoint 依赖 Graphics DPI，跨 DC（PrintWindow 等）不可靠
    Gdiplus::Font font = MakeFont(12.0f * (Gdiplus::REAL)dpi / 96.0f, false);
    Gdiplus::StringFormat sf(Gdiplus::StringFormat::GenericTypographic());
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    // 禁用灰;默认键强调整键文字更亮
    COLORREF tcolor = !enabled ? kBtnTextOff : (def ? RGB(238, 242, 252) : kText);
    Gdiplus::SolidBrush text_br(Gac(tcolor, 255));
    if (text[0]) {
        g.DrawString(text, (INT)wcslen(text), &font,
                     Gdiplus::RectF((Gdiplus::REAL)rc.left, (Gdiplus::REAL)rc.top,
                                    (Gdiplus::REAL)(rc.right - rc.left),
                                    (Gdiplus::REAL)(rc.bottom - rc.top)), &sf, &text_br);
    }
}

void PaintButton(HWND h) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(h, &ps);
    PaintButtonTo(h, hdc);
    EndPaint(h, &ps);
}

LRESULT CALLBACK ThemedBtnProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    BtnTheme* st = (BtnTheme*)GetPropW(h, kBtnProp);
    if (!st) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
        case WM_MOUSEMOVE:
            if (!st->hover) {
                st->hover = true;
                InvalidateRect(h, nullptr, TRUE);
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                TrackMouseEvent(&tme);
            }
            break;
        case WM_MOUSELEAVE:
            if (st->hover) {
                st->hover = false;
                InvalidateRect(h, nullptr, TRUE);
            }
            break;
        case WM_LBUTTONDOWN:
            st->pressed = true;
            SetCapture(h);
            InvalidateRect(h, nullptr, TRUE);
            return 0; // 不转发:原生按钮的按下处理会抢走捕获,导致抬起被吞、点击丢失(实测)

        case WM_LBUTTONUP: {
            bool was_pressed = st->pressed;
            st->pressed = false;
            bool have_cap = GetCapture() == h;
            if (have_cap) ReleaseCapture();
            InvalidateRect(h, nullptr, TRUE);
            if (was_pressed) {
                RECT rc{};
                GetClientRect(h, &rc);
                POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                bool inrect = pt.x >= rc.left && pt.x < rc.right && pt.y >= rc.top &&
                              pt.y < rc.bottom;
                if (inrect) {
                    SendMessageW(GetParent(h), WM_COMMAND,
                                 MAKEWPARAM(GetDlgCtrlID(h), BN_CLICKED), (LPARAM)h);
                }
            }
            return 0; // 同上:抬起不再转发原生过程
        }
        case WM_CAPTURECHANGED:
            st->pressed = false;
            InvalidateRect(h, nullptr, TRUE);
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            PaintButton(h);
            return 0;
        case WM_PRINTCLIENT:
            // PrintWindow/WM_PRINT 路径：自绘直画到目标 DC（截图/打印可见）
            PaintButtonTo(h, (HDC)wp);
            return 0;
        case WM_ENABLE:
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(h, nullptr, TRUE);
            break;
        case WM_NCDESTROY: {
            WNDPROC old_proc = st->old; // 先取出再释放：break 落到统一转发会读 st->old
            RemovePropW(h, kBtnProp);
            delete st;
            return CallWindowProcW(old_proc, h, msg, wp, lp);
        }
        default:
            break;
    }
    return CallWindowProcW(st->old, h, msg, wp, lp);
}

void ThemeButton(HWND h) {
    DWORD style = (DWORD)GetWindowLongPtrW(h, GWL_STYLE);
    DWORD type = style & BS_TYPEMASK;
    if (type != BS_PUSHBUTTON && type != BS_DEFPUSHBUTTON) return;
    if (GetPropW(h, kBtnProp)) return; // 已子类化
    BtnTheme* st = new BtnTheme();
    st->old = (WNDPROC)GetWindowLongPtrW(h, GWLP_WNDPROC);
    SetPropW(h, kBtnProp, st);
    SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)ThemedBtnProc);
}

// DarkMode 主题(文档化程度低,失败静默):给原生复选/单选/滚动条/边框暗色绘制
void TryDarkTheme(HWND h) {
    SetWindowTheme(h, L"DarkMode_Explorer", nullptr);
}

} // namespace

void EnsureGdiplus() { GdiplusOnce(); }

void SetUiTheme(bool dark) {
    if (dark == g_dark && g_theme_gen > 0) return;
    g_dark = dark;
    if (dark) ApplyDarkPalette();
    else ApplyLightPalette();
    ++g_theme_gen;
}
bool UiIsDark() { return g_dark; }
int UiThemeGen() { return g_theme_gen; }
void ApplyUiThemeToWindow(HWND wnd) { ApplyWindowChrome(wnd); }

// ---- 系统集成 ----
bool AnimationsEnabled() {
    BOOL enabled = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0)) return true;
    return enabled != FALSE;
}

void EnableDarkSystemChrome() {
    // uxtheme 序数:135=SetPreferredAppMode 136=FlushMenuThemes(Win10 1809+;失败静默)
    // 按当前主题取模式：深色 ForceDark；浅色回 Default（浅色菜单），不残留暗色
    enum class PreferredAppMode : int { Default = 0, AllowDark = 1, ForceDark = 2 };
    using FnSetMode = void(WINAPI*)(PreferredAppMode);
    using FnFlush = void(WINAPI*)();
    HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
    if (!ux) return;
    if (auto set_mode = (FnSetMode)GetProcAddress(ux, MAKEINTRESOURCEA(135)))
        set_mode(g_dark ? PreferredAppMode::ForceDark : PreferredAppMode::Default);
    if (auto flush = (FnFlush)GetProcAddress(ux, MAKEINTRESOURCEA(136))) flush();
}

void ApplyWindowChrome(HWND wnd) {
    if (!wnd) return;
    BOOL round = TRUE; // DWMWCP_ROUND
    DwmSetWindowAttribute(wnd, DWMWA_WINDOW_CORNER_PREFERENCE, &round, sizeof(round));
    COLORREF caption = kWindowBg;
    DwmSetWindowAttribute(wnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    COLORREF border = kBorder;
    DwmSetWindowAttribute(wnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    COLORREF text = kText;
    DwmSetWindowAttribute(wnd, DWMWA_TEXT_COLOR, &text, sizeof(text));
}

void ModernizeDialog(HWND dlg) {
    if (!dlg) return;
    ApplyWindowChrome(dlg);
    // WNDENUMPROC 不接受捕获 lambda：DarkMode 判定走 UiIsDark()（capture-less 才可转换）
    EnumChildWindows(dlg, [](HWND h, LPARAM) -> BOOL {
        wchar_t cls[32]{};
        GetClassNameW(h, cls, 32);
        const bool dark = UiIsDark();
        if (wcscmp(cls, L"Button") == 0) {
            DWORD style = (DWORD)GetWindowLongPtrW(h, GWL_STYLE);
            DWORD type = style & BS_TYPEMASK;
            if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON) ThemeButton(h);
            else if (dark && (type == BS_AUTOCHECKBOX || type == BS_AUTORADIOBUTTON ||
                              type == BS_GROUPBOX)) TryDarkTheme(h);
        } else if (wcscmp(cls, L"Edit") == 0 && dark) {
            TryDarkTheme(h); // 浅色主题用系统默认（本就浅色）
        }
        return TRUE;
    }, 0);
}

bool HandleDialogMessage(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* out) {
    (void)wnd;
    (void)lp;
    switch (msg) {
        case WM_ERASEBKGND: {
            HDC hdc = (HDC)wp;
            RECT rc{};
            GetClientRect(wnd, &rc);
            SetDCBrushColor(hdc, kWindowBg);
            HBRUSH br = (HBRUSH)GetStockObject(DC_BRUSH);
            FillRect(hdc, &rc, br);
            if (out) *out = 1;
            return true;
        }
        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wp;
            SetTextColor(hdc, kText);
            SetBkColor(hdc, kWindowBg);
            SetDCBrushColor(hdc, kWindowBg);
            if (out) *out = (LRESULT)GetStockObject(DC_BRUSH);
            return true;
        }
        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wp;
            SetTextColor(hdc, kText);
            SetBkColor(hdc, kPanelBg);
            static ThemedBrushCache panel; // 主题代数驱动重建（ThemedSolidBrush）
            if (out) *out = (LRESULT)ThemedSolidBrush(panel, kPanelBg);
            return true;
        }
        case WM_CTLCOLORBTN: {
            SetDCBrushColor((HDC)wp, kWindowBg);
            if (out) *out = (LRESULT)GetStockObject(DC_BRUSH);
            return true;
        }
        default:
            return false;
    }
}

// ---- 文本原语 ----
void DrawTextPx(HDC hdc, const RECT& rc, const wchar_t* text, const TextStyle& ts) {
    if (!text || !*text || rc.right <= rc.left || rc.bottom <= rc.top) return;
    GdiplusSession session;
    Gdiplus::Graphics g(hdc);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    Gdiplus::Font font = MakeFont(ts.px, ts.bold, ts.face);
    Gdiplus::StringFormat sf(Gdiplus::StringFormat::GenericTypographic());
    ApplyFormat(&sf, ts);
    Gdiplus::RectF box((Gdiplus::REAL)rc.left, (Gdiplus::REAL)rc.top,
                       (Gdiplus::REAL)(rc.right - rc.left),
                       (Gdiplus::REAL)(rc.bottom - rc.top));
    if (ts.valign == 1) {
        // 垂直居中：先测量行高再平移（MeasureString 与 DrawString 同一格式保证一致）
        Gdiplus::RectF measured;
        g.MeasureString(text, (INT)wcslen(text), &font,
                        Gdiplus::RectF(0, 0, box.Width, 4096), &sf, &measured);
        box.Y += (box.Height - measured.Height) / 2.0f;
    }
    Gdiplus::SolidBrush br(Gac(ts.color, ts.alpha));
    g.DrawString(text, (INT)wcslen(text), &font, box, &sf, &br);
}

SIZE MeasureTextPx(HDC hdc, const wchar_t* text, const TextStyle& ts) {
    SIZE out{};
    if (!text || !*text) return out;
    GdiplusSession session;
    Gdiplus::Graphics g(hdc);
    Gdiplus::Font font = MakeFont(ts.px, ts.bold, ts.face);
    Gdiplus::StringFormat sf(Gdiplus::StringFormat::GenericTypographic());
    ApplyFormat(&sf, ts);
    Gdiplus::RectF box;
    g.MeasureString(text, (INT)wcslen(text), &font,
                    Gdiplus::RectF(0, 0, 4096, 512), &sf, &box);
    out.cx = (LONG)(box.Width + 0.99f);
    out.cy = (LONG)(box.Height + 0.99f);
    return out;
}

// ---- 形状原语 ----
void FillRoundRectPx(HDC hdc, const RECT& rc, int radius_px, COLORREF color, int alpha) {
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;
    GdiplusSession session;
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::RectF rrf((Gdiplus::REAL)rc.left, (Gdiplus::REAL)rc.top,
                       (Gdiplus::REAL)(rc.right - rc.left),
                       (Gdiplus::REAL)(rc.bottom - rc.top));
    Gdiplus::SolidBrush br(Gac(color, alpha));
    FillRoundRect(g, rrf, (Gdiplus::REAL)radius_px, br);
}

void StrokeRoundRectPx(HDC hdc, const RECT& rc, int radius_px, COLORREF color, int alpha,
                       float width_px) {
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;
    GdiplusSession session;
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::RectF rrf((Gdiplus::REAL)rc.left, (Gdiplus::REAL)rc.top,
                       (Gdiplus::REAL)(rc.right - rc.left),
                       (Gdiplus::REAL)(rc.bottom - rc.top));
    Gdiplus::Pen pen(Gac(color, alpha), width_px);
    StrokeRoundRect(g, rrf, (Gdiplus::REAL)radius_px, pen);
}

void FillEllipsePx(HDC hdc, float cx, float cy, float diameter, COLORREF color, int alpha) {
    if (diameter <= 0) return;
    GdiplusSession session;
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush br(Gac(color, alpha));
    g.FillEllipse(&br, cx - diameter / 2.0f, cy - diameter / 2.0f, diameter, diameter);
}

void FillGradientPx(HDC hdc, const RECT& rc, COLORREF left, COLORREF right) {
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;
    GdiplusSession session;
    Gdiplus::Graphics g(hdc);
    Gdiplus::RectF full((Gdiplus::REAL)rc.left, (Gdiplus::REAL)rc.top,
                        (Gdiplus::REAL)(rc.right - rc.left),
                        (Gdiplus::REAL)(rc.bottom - rc.top));
    Gdiplus::LinearGradientBrush brush(full, Gac(left, 255), Gac(right, 255), 0.0f);
    g.FillRectangle(&brush, full);
}

void DrawRingPx(HDC hdc, float cx, float cy, float diameter, float stroke,
                float sweep_percent, COLORREF track, COLORREF value, int alpha) {
    if (diameter <= 0 || stroke <= 0) return;
    GdiplusSession session;
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::RectF rr(cx - diameter / 2.0f + stroke / 2.0f,
                      cy - diameter / 2.0f + stroke / 2.0f,
                      diameter - stroke, diameter - stroke);
    Gdiplus::Pen track_pen(Gac(track, alpha * 20 / 100), stroke);
    g.DrawEllipse(&track_pen, rr);
    if (sweep_percent < 0) return; // 无比例语义：只画轨道
    if (sweep_percent >= 100) {
        Gdiplus::Pen vp(Gac(value, alpha), stroke);
        g.DrawEllipse(&vp, rr);
    } else if (sweep_percent > 0) {
        Gdiplus::Pen vp(Gac(value, alpha), stroke);
        g.DrawArc(&vp, rr, -90.0f, 360.0f * sweep_percent / 100.0f); // 自环顶顺时针
    }
}

void DrawBadgePx(HDC hdc, const RECT& rc, int radius_px, COLORREF fill, int alpha,
                 wchar_t letter, float letter_px, bool letter_bold,
                 COLORREF letter_color, int letter_alpha) {
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;
    GdiplusSession session;
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::RectF rrf((Gdiplus::REAL)rc.left, (Gdiplus::REAL)rc.top,
                       (Gdiplus::REAL)(rc.right - rc.left),
                       (Gdiplus::REAL)(rc.bottom - rc.top));
    Gdiplus::SolidBrush br(Gac(fill, alpha));
    FillRoundRect(g, rrf, (Gdiplus::REAL)radius_px, br);
    if (!letter) return;
    Gdiplus::Font font = MakeFont(letter_px, letter_bold); // 像素字号必须 UnitPixel（UnitPoint 依赖 Graphics DPI，实测会漂移）
    Gdiplus::StringFormat sf(Gdiplus::StringFormat::GenericTypographic());
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    Gdiplus::SolidBrush lb(Gac(letter_color, letter_alpha));
    g.DrawString(&letter, 1, &font, rrf, &sf, &lb);
}

} // namespace theme
} // namespace qb
