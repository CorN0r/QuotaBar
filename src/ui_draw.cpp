// ui_draw.cpp — 主窗口共享绘制原语/控件族/交互态全局（2026-09-19 拆分终局：
// tray_ui.cpp 解体为 ui_draw/detail_shell/tray_area 三 TU）。
// 所有权：g_* 共享全局与 Draw*/Blend/Ts 等原语的【唯一定义】在此；
// 页面绘制在 quota_page/display_page；窗口壳在 detail_shell；托盘在 tray_area。
// 默认实参只存在于 tray_ui_internal.hpp（定义处不重复）。
#include "tray_ui_internal.hpp"
#include "util.hpp"
#include "providers.hpp"
#include <shellscalingapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <stdio.h>

#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "gdiplus.lib")

namespace qb {

// =========================================================
// 布局 DPI 缩放（Per-Monitor V2 感知进程，系统不会自动缩放窗口）
// =========================================================
int g_layout_dpi = 96;
void SetLayoutDpi(HWND wnd) {
    UINT dpi = GetDpiForWindow(wnd);
    if (dpi) {
        g_layout_dpi = (int)dpi;
        SetIconsDpi(g_layout_dpi); // 图标描边随同一 DPI 缩放（ui_icons 模块注入）
    }
}

HFONT g_edit_mono = nullptr; // 输入框字体：等宽（Key/数值）
int g_edit_font_dpi = 0;
void RefreshEditFonts() {
    if (g_edit_font_dpi == g_layout_dpi && g_edit_mono) return;
    if (g_edit_mono) DeleteObject(g_edit_mono);
    int h = -MulDiv(10, g_layout_dpi, 72);
    g_edit_mono = CreateFontW(h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                              theme::kFontMono);
    g_edit_font_dpi = g_layout_dpi;
}
void ApplyEditFonts(HWND root) {
    RefreshEditFonts();
    EnumChildWindows(root, [](HWND h, LPARAM) -> BOOL {
        SendMessageW(h, WM_SETFONT, (LPARAM)g_edit_mono, TRUE);
        return TRUE;
    }, 0);
}

// ---- 子控件/文本小工具 ----
std::wstring EditTextW(HWND edit) {
    if (!edit) return std::wstring();
    int n = GetWindowTextLengthW(edit);
    std::wstring s((size_t)n + 1, L'\0');
    GetWindowTextW(edit, s.data(), n + 1);
    s.resize(n);
    return s;
}
std::string EditTextUtf8(HWND edit) { return WideToUtf8(TrimW(EditTextW(edit))); }
int GetEditInt(HWND edit, int def) {
    std::wstring s = TrimW(EditTextW(edit));
    return s.empty() ? def : (int)_wtoi(s.c_str());
}
double GetEditDouble(HWND edit, double def) {
    std::wstring s = TrimW(EditTextW(edit));
    return s.empty() ? def : _wtof(s.c_str());
}
void DestroyChildren(HWND h) {
    HWND c;
    while ((c = FindWindowExW(h, nullptr, nullptr, nullptr)) != nullptr)
        DestroyWindow(c);
}
bool EnsureClass(const wchar_t* cls, WNDPROC proc, HINSTANCE inst) {
    WNDCLASSW wc{};
    if (GetClassInfoW(inst, cls, &wc)) return true;
    wc = {};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.lpszClassName = cls;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(102));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    return RegisterClassW(&wc) != 0;
}

// =========================================================
// 设计 token 辅助（stitch 调色板见 theme.hpp 的 kS* 常量）
// =========================================================
COLORREF Blend(COLORREF top, COLORREF base, int alpha) {
    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;
    return RGB((GetRValue(top) * alpha + GetRValue(base) * (255 - alpha)) / 255,
               (GetGValue(top) * alpha + GetGValue(base) * (255 - alpha)) / 255,
               (GetBValue(top) * alpha + GetBValue(base) * (255 - alpha)) / 255);
}
theme::TextStyle Ts(theme::TypeSize size, bool bold, COLORREF color, int alpha, int align,
                    int valign, bool wrap, const wchar_t* face) {
    theme::TextStyle t;
    t.px = LSf(theme::TypeSizeDip(size));
    t.bold = bold;
    t.color = color;
    t.alpha = alpha;
    t.align = align;
    t.valign = valign;
    t.wrap = wrap;
    t.face = face;
    return t;
}
SIZE Measure(HDC hdc, const wchar_t* text, theme::TypeSize size, bool bold, COLORREF color,
             const wchar_t* face) {
    return theme::MeasureTextPx(hdc, text, Ts(size, bold, color, 255, 0, 0, false, face));
}
void Txt(HDC hdc, const RECT& rc, const wchar_t* text, theme::TypeSize size, bool bold,
         COLORREF color, int align, int valign, const wchar_t* face, int alpha) {
    theme::DrawTextPx(hdc, rc, text, Ts(size, bold, color, alpha, align, valign, false, face));
}
void FillRr(HDC hdc, const RECT& rc, int radius_dip, COLORREF color, int alpha) {
    theme::FillRoundRectPx(hdc, rc, LS(radius_dip), color, alpha);
}
void StrokeRr(HDC hdc, const RECT& rc, int radius_dip, COLORREF color, int alpha,
              float w_dip) {
    theme::StrokeRoundRectPx(hdc, rc, LS(radius_dip), color, alpha, LSf(w_dip));
}
void HLinePx(HDC hdc, int x1, int x2, int y, COLORREF color) {
    RECT r{x1, y, x2 > x1 ? x2 : x1 + 1, y + 1};
    SetDCBrushColor(hdc, color);
    FillRect(hdc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}
// 状态点（cx,cy 圆心；d 直径 DIP）
void Dot(HDC hdc, int cx, int cy, int d_dip, COLORREF color, int alpha) {
    theme::FillEllipsePx(hdc, (float)cx, (float)cy, (float)LS(d_dip), color, alpha);
}

// ---- 设计系统：行规格与流式纵向布局 ----
// 流式布局：从 y 起占 h DIP 行高并原地推进 gap DIP（无返回值，y 原地累进）。
// Compute* 与 Paint* 共用同一推进序列，替代固定偏移硬排（修文字重叠的根因）。
void RowDown(int& y, int h, int gap) {
    y += LS(h) + LS(gap);
}



// 额度页动态输入框 id（别名/Key；CM_CTLCOLOREDIT/EN 通知分派用）
bool IsQuotaEditId(int id) {
    return id == kEditAddAlias || id == kEditAddKey ||
           (id >= kEditAccAlias0 && id < kEditAccAlias0 + kAccMaxCount) ||
           (id >= kEditAccKey0 && id < kEditAccKey0 + kAccMaxCount);
}

std::vector<PageLink> g_links;

std::vector<ui::HitW> g_hits;
// 命中区域规则：滚动列表项只在列表视口内可点（滚出视口的行不可误触）；
// chrome 区（侧栏/标题栏，位于内容区之外）与页脚动作条恒可点。g_hit_content 为空=不限制。
// g_hit_clip 非空时命中注册裁到可见部分（额度页滚动视口，与 PaintQuota 的 DC 裁剪同矩形）。
// 核心判定在 ui_layout.cpp（纯函数，selftest 覆盖）；此处只喂全局状态。
RECT g_hit_content{};
RECT g_hit_vp{};
RECT g_hit_extra{};
RECT g_hit_clip{};
int HitAt(POINT pt) {
    return ui::HitTestAt(g_hits, pt, g_hit_content, g_hit_vp, g_hit_extra);
}
RECT RectOfHit(int id) {
    for (const auto& h : g_hits)
        if (h.id == id) return h.rc;
    return RECT{0, 0, 0, 0};
}

int g_hover = -1;        // 悬停控件 id
int g_pressed = -1;      // 按下控件 id（down 高亮、up 命中才提交、拖离取消）
POINT g_cursor_pt{-1, -1}; // 最近鼠标客户区坐标（卡片整体 hover 提亮用）
int g_card_hover = -1;   // 光标所在卡片可见序（-1=不在卡上；跨卡进出需单独触发重绘）
int g_slide_drag = 0;    // 正在拖动的滑杆 id（0=无）
bool g_tracked = false;  // TME_LEAVE 已注册
bool g_maximized = false;

// 卡片拖拽排序状态（F3；会话内）
int g_drag_press_vis = -1;  // 按下待起拖的可见序（未超过阈值时只是普通按压）
int g_drag_from = -1;       // 拖拽中的源可见序
int g_drag_target = -1;     // 当前目标插入位（其余卡片序 0..n-1）
POINT g_drag_start{};       // 按下起点（客户区）

Draft g_draft;

QuotaUi g_q;

// ---- 动作回显（第十一轮）：单槽消息 + 定时器过期；绘制在 PaintEchoPill ----
EchoState g_echo;
SaveFbState g_save_fb;
static void EchoArmTimer(HWND w) {
    // 200ms 粒度轮询到期（回显窗口极短、极少并发，开销可忽略；全部过期后停表）
    if (w) SetTimer(w, kTimerEcho, 200, nullptr);
}
void UiEcho(HWND w, int kind, const wchar_t* text) {
    if (!text || !*text) return;
    g_echo.text = text;
    g_echo.kind = kind;
    g_echo.until = NowMono() + (kind == kEchoErr ? 3200 : 2400); // 失败多留 0.8s 阅读
    EchoArmTimer(w);
    InvalidateRect(w, nullptr, FALSE);
}
void UiSaveFeedback(HWND w, bool ok) {
    g_save_fb.kind = ok ? 1 : 2;
    g_save_fb.until = NowMono() + 2200;
    EchoArmTimer(w);
    InvalidateRect(w, nullptr, FALSE);
}


// =========================================================
// 通用绘制元件
// =========================================================
BtnStyle StyleGhost() {
    // 幽灵键（放弃变更/取消/已隐藏）：低层面 + 细描边，悬浮提亮
    return {theme::kSSurf, theme::kSSurfHigh, theme::kSSurfHigh2, theme::kSOnSurfVar,
            theme::kSCardBorder};
}
BtnStyle StyleSoft() {
    // 柔和键（设置 ▾/显示）：卡面高一层 + 描边
    return {theme::kSSurfHigh, theme::kSSurfHigh2,
            Blend(theme::kSSurfHigh2, theme::kSOnSurf, 30), theme::kSOnSurfVar,
            theme::kSCardBorder};
}
BtnStyle StylePrimary() {
    // 实心强调键（添加卡片/保存设置）：蓝-600 白字，悬浮=偏 亮，按下=偏 暗
    return {theme::kSPrimCont, Blend(theme::kSPrimCont, theme::kSOnSurf, 45),
            Blend(theme::kSPrimCont, theme::kSbg, 45), theme::kSOnPrimCont};
}

// 按钮：图标(可选) + 文本。radius_dip=8。
void DrawBtn(HDC hdc, const RECT& rc, const wchar_t* label, int icon, const BtnStyle& st,
             int id, theme::TypeSize text_size, bool semibold, int radius_dip, int icon_dip,
             int pad_dip) {
    const bool hov = Hot(id), dwn = Down(id);
    COLORREF bg = dwn ? st.bg_down : (hov ? st.bg_hover : st.bg);
    FillRr(hdc, rc, radius_dip, bg);
    if (st.border)
        StrokeRr(hdc, rc, radius_dip,
                 dwn || hov ? Blend(st.border, theme::kSOnSurf, 55) : st.border, 210, 1.0f);
    int pad = LS(pad_dip);
    int x = rc.left + pad;
    int cy = (rc.top + rc.bottom) / 2;
    int text_x = x;
    if (icon != IcNone) {
        RECT ib = RC(x, cy - LS(icon_dip) / 2, LS(icon_dip), LS(icon_dip));
        Icon(hdc, icon, ib, st.fg, 255);
        text_x = ib.right + LS(5);
    }
    if (label && *label) {
        RECT tr{text_x, rc.top, rc.right - pad, rc.bottom};
        Txt(hdc, tr, label, text_size, semibold, st.fg);
    }
    AddHit(rc, id);
}
// 纯图标按钮（min/max/close/输入行图标等）
void DrawIconBtn(HDC hdc, const RECT& rc, int icon, COLORREF fg, int id, COLORREF hover_bg,
                 COLORREF hover_fg, int icon_dip) {
    const bool hov = Hot(id), dwn = Down(id);
    if (dwn) FillRr(hdc, rc, 6, Blend(hover_bg, theme::kSSurfLow2, 220));
    else if (hov) FillRr(hdc, rc, 6, hover_bg);
    RECT ib = RC((rc.left + rc.right - LS(icon_dip)) / 2,
                 (rc.top + rc.bottom - LS(icon_dip)) / 2, LS(icon_dip), LS(icon_dip));
    Icon(hdc, icon, ib, hov || dwn ? hover_fg : fg, 255);
    AddHit(rc, id);
}
// 开关（w-11 h-6）：on_color=选中底色
void DrawToggle(HDC hdc, const RECT& rc, bool on, COLORREF on_color, int id, bool enabled) {
    COLORREF track = on ? on_color : theme::kSSurfHigh2;
    FillRr(hdc, rc, 12, track, enabled ? 255 : 90);
    int d = rc.bottom - rc.top - LS(4);
    int kx = on ? rc.right - LS(2) - d / 2 : rc.left + LS(2) + d / 2;
    theme::FillEllipsePx(hdc, (float)kx, (float)rc.top + LS(2) + (float)d / 2, (float)d,
                         RGB(255, 255, 255), enabled ? 255 : 140);
    AddHit(rc, id);
}
// 滑杆：rc 为整个命中区（含上下留白），轨道居中
void DrawSlider(HDC hdc, const RECT& rc, int vmin, int vmax, int val, COLORREF accent, int id,
                bool enabled) {
    RECT track{rc.left + LS(7), (rc.top + rc.bottom) / 2 - LS(3),
               rc.right - LS(7), (rc.top + rc.bottom) / 2 + LS(3)};
    FillRr(hdc, track, 3, theme::kSSurfHigh2, enabled ? 255 : 90);
    float t = (vmax > vmin) ? (float)(val - vmin) / (float)(vmax - vmin) : 0.f;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int fx = track.left + (int)((track.right - track.left) * t);
    if (fx > track.left) {
        RECT fill{track.left, track.top, fx, track.bottom};
        FillRr(hdc, fill, 3, accent, enabled ? 255 : 90);
    }
    int d = LS(14);
    // FillEllipsePx 取圆心坐标：圆球压在轨道中心线上(曾误按左上角语义减 d/2,
    // 圆球整体抬高/左移半个直径,实测与轨道脱开)
    theme::FillEllipsePx(hdc, (float)fx, (float)(rc.top + rc.bottom) / 2, (float)d, accent,
                         enabled ? 255 : 120);
    AddHit(rc, id);
}
// 文本链接（下划线交互色）；注册命中。悬停/按下色随主题 token 派生（F1）
void DrawLink(HDC hdc, const RECT& rc, const wchar_t* text, theme::TypeSize size, bool accounts,
              const char* url) {
    const int idx = (int)g_links.size();
    COLORREF color = theme::kSPrimary;
    if (g_pressed == kWLink0 + idx) color = theme::kSOnSurf;
    else if (g_hover == kWLink0 + idx) color = Blend(theme::kSPrimary, theme::kSOnSurf, 150);
    Txt(hdc, rc, text, size, false, color);
    AddHit(rc, kWLink0 + idx);
    g_links.push_back({rc, accounts, url});
}

// ---- 卡片 ⓘ 说明的悬停气泡（口径说明不再长显；气泡在卡片绘制内就地画出，
//      与命中判定同帧同坐标系——延迟到 PaintQuota 末尾统一画曾因跨布局命中
//      错位把气泡画到页头，实测废弃） ----

// 气泡文字按像素宽换行：优先空格断词（保住 API/Key 等英文词），无空格逐字断
std::vector<std::wstring> WrapNoteText(HDC hdc, const std::wstring& text, int max_px) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t ch : text) {
        if (ch == L'\n') {
            out.push_back(cur);
            cur.clear();
            continue;
        }
        std::wstring cand = cur + ch;
        if (!cur.empty() &&
            Measure(hdc, cand.c_str(), theme::TypeSize::Cap, false, 0).cx > max_px) {
            size_t sp = cur.find_last_of(L' ');
            if (sp != std::wstring::npos && sp > 0) {
                out.push_back(cur.substr(0, sp));
                cur = cur.substr(sp + 1) + ch;
            } else {
                out.push_back(cur);
                cur.assign(1, ch);
            }
        } else {
            cur = std::move(cand);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    if (out.empty()) out.push_back(text);
    return out;
}

// 悬停气泡：anchor=ⓘ 右下角；水平钳在卡片内，底部放不下翻到图标上方
void DrawNoteBubble(HDC m, const RECT& card, const POINT& anchor, const std::wstring& text) {
    const int pad = LS(8), line_h = LS(18), maxw = LS(280);
    std::vector<std::wstring> lines;
    int bw;
    SIZE one = Measure(m, text.c_str(), theme::TypeSize::Cap, false, 0);
    if (one.cx + pad * 2 <= maxw) {
        lines.push_back(text);
        bw = one.cx + pad * 2;
    } else {
        lines = WrapNoteText(m, text, maxw - pad * 2);
        int lw = 0;
        for (const auto& l : lines)
            lw = std::max(lw,
                          (int)Measure(m, l.c_str(), theme::TypeSize::Cap, false, 0).cx);
        bw = std::min(lw + pad * 2, maxw);
    }
    int bh = (int)lines.size() * line_h + LS(10);
    int bx = std::min(std::max((int)anchor.x - LS(8), (int)card.left + LS(8)),
                      (int)card.right - bw - LS(8));
    int by = anchor.y + LS(6);
    if (by + bh > (int)card.bottom - LS(8)) by = anchor.y - bh - LS(6);
    RECT br{bx, by, bx + bw, by + bh};
    FillRr(m, br, 8, theme::kSSurfHigh);
    StrokeRr(m, br, 8, theme::kSOutlineVar, 170, 1.0f);
    int ty = by + LS(5);
    for (const auto& l : lines) {
        Txt(m, RECT{br.left + pad, ty, br.right - pad, ty + line_h}, l.c_str(),
            theme::TypeSize::Cap, false, theme::kSOnSurf);
        ty += line_h;
    }
}

// =========================================================
// 数据辅助
// =========================================================
// "还有 3小时25分" → "余 3小时25分"（设计稿口径）
std::wstring RemainText(const std::wstring& s) {
    const wchar_t* p = L"还有 ";
    size_t n = wcslen(p);
    if (s.compare(0, n, p) == 0) return L"余 " + s.substr(n);
    return s;
}
// 语义 tone → stitch 色
COLORREF ToneColor(DetailTone t) {
    switch (t) {
        case DetailTone::Ok: return theme::kSSecondary;
        case DetailTone::Info: return theme::kSPrimary;
        case DetailTone::Warn: return theme::kSTertiary;
        case DetailTone::Bad: return theme::kSError;
        case DetailTone::Muted: return theme::kSOutline;
        case DetailTone::Neutral: return theme::kSOnSurf;
    }
    return theme::kSOnSurf;
}
using ui::QuotaLayout;

// 最近一次布局与卡片数据（Paint 产出；点击处理/Relayout 按同坐标消费）
QuotaLayout g_qlayout;
std::vector<DetailCard> g_cards;



// 品牌三件套：徽章底（品牌色低透明叠卡面）、描边、字色（按主题取可读变体）。
// 深色主题字色取品牌色向白靠 51%（近似 tailwind 300/400 章）；浅色主题向黑靠 24%。
void BrandLook(wchar_t badge, COLORREF& bg, COLORREF& border, COLORREF& fg) {
    COLORREF base = ProviderOpsByBadge(badge).brand_color; // 品牌色单点在 providers 分派表
    const COLORREF card = theme::kSSurf;
    bg = Blend(base, card, theme::UiIsDark() ? 46 : 36);
    border = Blend(base, card, theme::UiIsDark() ? 150 : 130);
    fg = theme::UiIsDark() ? Blend(base, RGB(255, 255, 255), 130)
                           : Blend(base, RGB(0, 0, 0), 60);
}

// 类型章文案（stitch 卡头 tag；按平台字母派生，单点在 providers 分派表）
const wchar_t* TypeTag(wchar_t badge) {
    return ProviderOpsByBadge(badge).type_tag;
}

// 侧栏页脚版本号：读 exe 版本资源（assets/quota_bar.rc 为单一来源；失败回退 v1.0）。
// 读数实现在 util::ExeVersion（HTTP 请求头 User-Agent 同源），这里只加 "v" 前缀。
std::wstring AppVersionText() {
    static const std::wstring cached = [] {
        const std::wstring& v = ExeVersion();
        return v.empty() ? std::wstring(L"v1.0") : L"v" + v;
    }();
    return cached;
}

// 纯 ASCII 文本才允许等宽字体（Cascadia Mono 无 CJK 字形，Measure/Draw 回退宽度不同）
bool IsAsciiText(const std::wstring& s) {
    for (wchar_t ch : s)
        if (ch > 0x7F) return false;
    return true;
}


int g_dscroll = 0; // 显示设置页滚动偏移（px；ComputeDisp 钳制后统一应用）

} // namespace qb