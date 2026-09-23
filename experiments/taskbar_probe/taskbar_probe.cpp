// taskbar_probe.cpp —— 任务栏内嵌原型实验(独立于 QuotaBar 主程序)
//
// 目的:验证「GDI+ UpdateLayeredWindow 文字窗 SetParent 到 Shell_TrayWnd」在
// Win11 XAML 任务栏上的可行性。渲染管线与 QuotaBar overlay 完全相同
// (32bpp PARGB DIB + ULW + 灰度抗锯齿),唯一变量是窗口被寄宿进任务栏。
//
// 观察点(日志逐条打点):
//   1. 显示/合成 —— 文字能否出现在任务栏内部、有无黑块/闪烁/撕裂
//   2. 点击穿透 —— WS_EX_TRANSPARENT + WM_NCHITTEST=HTTRANSPARENT
//   3. 任务栏自动隐藏 —— 宿主移出屏幕时探针跟随隐藏(Tick 检测)
//   4. Explorer 重启 —— 宿主失效→脱离,新 Shell_TrayWnd 出现→自动重挂
//   5. 刷新闪烁 —— 每秒整窗重绘(比主程序实际刷新频率更激进)
//
// 坐标:按“任务栏屏幕矩形内、紧贴托盘区左侧”计算期望屏幕位置 S,SetWindowPos
// 后回读 GetWindowRect 与 S 比对,差值累积为校正项 —— 不假设宿主坐标语义
// (popup 风格窗口 SetParent 后的坐标基准以实测为准,自校正)。
//
// 构建:probe_build.bat(需 VS2022 BuildTools 的 MSVC)
// 用法:taskbar_probe.exe [-t <秒>]   不传 -t 则无限运行;日志 = exe 旁 probe.log

#include <windows.h>
#include <objidl.h> // gdiplus.h 依赖 IStream/PROPID(WIN32_LEAN_AND_MEAN 下不自动带入)
#include <gdiplus.h>
#include <cstdio>
#include <cwchar>
#include <string>
#include <cmath>
#include <algorithm>

#pragma comment(lib, "gdiplus.lib")

namespace {

constexpr wchar_t kClass[] = L"QB_TaskbarProbeWnd";
constexpr int kTickMs = 1000;

FILE* g_log = nullptr;
bool g_float = false;     // -float:不做 SetParent,浮在任务栏同位置(遮挡对照实验)
bool g_colorkey = false;  // -colorkey:SetLayeredWindowAttributes 色键渲染(ULW 对照)
bool g_dump_once = false; // -dump:首次绘制后把 DIB 存 PNG(验证内容层)
POINT g_at{-1, -1};       // -at X,Y:覆盖期望位置(屏幕坐标,对照实验)
HWND g_probe = nullptr;   // 探针窗口(分层、popup,SetParent 到宿主)
HWND g_host = nullptr;    // Shell_TrayWnd
HWND g_tray = nullptr;    // 锚点:TrayNotifyWnd(或降级估算)
bool g_attached = false;
bool g_host_visible = true;
bool g_probe_visible = false;
int g_tick = 0;
int g_dpi = 96;
POINT g_org{0, 0};        // 宿主 client 原点(屏幕坐标)
POINT g_last_placed{-1, -1};

void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
    fflush(stdout);
    if (g_log) {
        fprintf(g_log, "%s\n", buf);
        fflush(g_log);
    }
}

int Scaled(int dip) { return dip * g_dpi / 96; }

// 递归找类名子窗口(锚点可能在任务栏子树上较深层)
struct FindCtx { const wchar_t* cls; HWND found = nullptr; int depth = 0; };
BOOL CALLBACK FindEnum(HWND h, LPARAM lp) {
    FindCtx* ctx = (FindCtx*)lp;
    wchar_t cls[80];
    if (GetClassNameW(h, cls, 80) && wcscmp(cls, ctx->cls) == 0) {
        ctx->found = h;
        return FALSE;
    }
    if (ctx->depth < 6) {
        FindCtx sub{ctx->cls, nullptr, ctx->depth + 1};
        EnumChildWindows(h, FindEnum, (LPARAM)&sub);
        if (sub.found) { ctx->found = sub.found; return FALSE; }
    }
    return TRUE;
}
HWND FindChildByClass(HWND parent, const wchar_t* cls) {
    FindCtx ctx{cls, nullptr, 0};
    EnumChildWindows(parent, FindEnum, (LPARAM)&ctx);
    return ctx.found;
}

struct GdiPlusToken {
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    GdiPlusToken() { Gdiplus::GdiplusStartup(&token, &input, nullptr); }
    ~GdiPlusToken() { if (token) Gdiplus::GdiplusShutdown(token); }
};

LRESULT CALLBACK ProbeProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    (void)w; (void)l;
    switch (m) {
        case WM_NCHITTEST: return HTTRANSPARENT; // 与主程序 overlay 一致:穿透
        case WM_ERASEBKGND: return 1;
        default: return DefWindowProcW(h, m, w, l);
    }
}

struct Seg { const wchar_t* text; unsigned abgr; }; // 颜色仅用于区分视觉,格式 GDI+ ARGB

// 测量两行文字尺寸 + 准备字体/格式(两渲染模式共用)
struct Layout {
    int w = 0, h = 0, font_px = 12, pad_x = 0, pad_y = 0;
    Gdiplus::REAL line_step = 0;
    bool ok = false;
};
static Layout MeasureLayout(int tick) {
    wchar_t ds[64];
    swprintf(ds, 64, L"余额 $12.%02d", 34 + tick % 100);
    const Seg lines[2][2] = {
        {{L"Kimi 周期 ", 0xFF9AA0A6}, {L"12,345 / 60,000", 0xFFFFB454}},
        {{L"DeepSeek API ", 0xFF9AA0A6}, {ds, 0xFF4CD08A}},
    };
    const int n_lines = 2;
    Gdiplus::Bitmap tmp(1, 1, PixelFormat32bppPARGB);
    Gdiplus::Graphics gm(&tmp);
    Layout L;
    L.font_px = std::max(12, Scaled(12));
    Gdiplus::Font font(L"Segoe UI", (Gdiplus::REAL)L.font_px, Gdiplus::FontStyleRegular,
                       Gdiplus::UnitPixel);
    Gdiplus::StringFormat sf(Gdiplus::StringFormat::GenericTypographic());
    int max_w = 0;
    Gdiplus::REAL lh_max = 0;
    for (int i = 0; i < n_lines; i++) {
        Gdiplus::REAL lw = 0;
        for (const auto& seg : lines[i]) {
            Gdiplus::RectF box;
            gm.MeasureString(seg.text, (INT)wcslen(seg.text), &font,
                             Gdiplus::RectF(0, 0, 4096, 512), &sf, &box);
            lw += box.Width;
            lh_max = std::max(lh_max, box.Height);
        }
        max_w = std::max(max_w, (int)std::ceil(lw));
    }
    L.pad_x = Scaled(6);
    L.pad_y = Scaled(2);
    L.w = max_w + L.pad_x * 2;
    L.h = (int)std::ceil((double)lh_max * n_lines * 1.15) + L.pad_y * 2;
    L.line_step = (Gdiplus::REAL)((double)lh_max * 1.15);
    L.ok = L.w > 0 && L.h > 0;
    return L;
}

// 在给定 Graphics 上画两行文字(Layout 须先测量)
static void DrawLines(Gdiplus::Graphics& g, int tick, const Layout& L) {
    wchar_t ds[64];
    swprintf(ds, 64, L"余额 $12.%02d", 34 + tick % 100);
    const Seg lines[2][2] = {
        {{L"Kimi 周期 ", 0xFF9AA0A6}, {L"12,345 / 60,000", 0xFFFFB454}},
        {{L"DeepSeek API ", 0xFF9AA0A6}, {ds, 0xFF4CD08A}},
    };
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit); // 灰度抗锯齿
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Font font(L"Segoe UI", (Gdiplus::REAL)L.font_px, Gdiplus::FontStyleRegular,
                       Gdiplus::UnitPixel);
    Gdiplus::StringFormat sf(Gdiplus::StringFormat::GenericTypographic());
    Gdiplus::REAL y = (Gdiplus::REAL)L.pad_y;
    for (int i = 0; i < 2; i++) {
        Gdiplus::REAL x = (Gdiplus::REAL)L.pad_x;
        for (const auto& seg : lines[i]) {
            Gdiplus::RectF box;
            g.MeasureString(seg.text, (INT)wcslen(seg.text), &font,
                            Gdiplus::RectF(0, 0, 4096, 512), &sf, &box);
            Gdiplus::SolidBrush br(Gdiplus::Color(seg.abgr));
            Gdiplus::RectF layout(x, y, box.Width + 4, box.Height + 4);
            g.DrawString(seg.text, (INT)wcslen(seg.text), &font, layout, &sf, &br);
            x += box.Width;
        }
        y += L.line_step;
    }
}

// 绘制与 overlay::Paint 同管线(ULW 路径)。返回窗口像素尺寸。
bool PaintUlw(int tick, int& out_w, int& out_h) {
    Layout L = MeasureLayout(tick);
    if (!L.ok) { out_w = out_h = 0; return false; }
    out_w = L.w;
    out_h = L.h;
    const int w = L.w, h = L.h;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // 顶向下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC memdc = CreateCompatibleDC(screen);
    HGDIOBJ old = memdc ? SelectObject(memdc, dib) : nullptr;
    ReleaseDC(nullptr, screen);
    if (!dib || !bits) {
        if (memdc) { SelectObject(memdc, old); DeleteDC(memdc); }
        if (dib) DeleteObject(dib);
        return false;
    }
    memset(bits, 0, (size_t)w * h * 4); // 背景 Alpha=0(透明)

    {
        Gdiplus::Bitmap bmp(w, h, 4 * w, PixelFormat32bppPARGB, (BYTE*)bits);
        Gdiplus::Graphics g(&bmp);
        DrawLines(g, tick, L);
        if (g_dump_once) {
            g_dump_once = false;
            CLSID enc{};
            UINT num = 0, sz = 0;
            Gdiplus::GetImageEncodersSize(&num, &sz);
            if (sz) {
                std::string buf(sz, '\0');
                Gdiplus::GetImageEncoders(num, (UINT)buf.size(),
                                          (Gdiplus::ImageCodecInfo*)buf.data());
                auto* list = (Gdiplus::ImageCodecInfo*)buf.data();
                for (UINT i = 0; i < num; i++)
                    if (wcscmp(list[i].MimeType, L"image/png") == 0) { enc = list[i].Clsid; break; }
            }
            if (enc.Data1) bmp.Save(L"painted_dib.png", &enc, nullptr);
            Log("[dump] painted_dib.png 已保存");
        }
    }

    // ULW 只提交内容,不移动窗口(dst=NULL):
    // 父窗口存在时 ULW 的 dwptDst 按父客户区坐标解释,与 GetWindowRect 的
    // 屏幕坐标帧不一致,每 tick 传 dst 会积累 org 偏移(实测每 tick +1984px)。
    SIZE ulw_sz{(LONG)w, (LONG)h};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    HDC hdc = GetDC(g_probe);
    BOOL ok = UpdateLayeredWindow(g_probe, hdc, nullptr, &ulw_sz, memdc, nullptr, 0, &blend, ULW_ALPHA);
    DWORD err = ok ? 0 : GetLastError();
    ReleaseDC(g_probe, hdc);
    SelectObject(memdc, old);
    DeleteDC(memdc);
    DeleteObject(dib);
    if (!ok && (tick % 10 == 0))
        Log("[paint] ULW 失败 err=%lu", err);
    return ok != FALSE;
}

// 色键分层路径(对照):SetLayeredWindowAttributes 色键 + 直接画窗口 DC,
// 完全绕开 ULW。背景填品红 RGB(255,0,255),该色处变透明。
bool PaintColorKey(int tick, int& out_w, int& out_h) {
    Layout L = MeasureLayout(tick);
    if (!L.ok) { out_w = out_h = 0; return false; }
    out_w = L.w;
    out_h = L.h;
    HDC hdc = GetDC(g_probe);
    if (!hdc) return false;
    {
        Gdiplus::Graphics g(hdc);
        Gdiplus::SolidBrush key(Gdiplus::Color(255, 255, 0, 255)); // 品红
        g.FillRectangle(&key, 0, 0, L.w, L.h);
        DrawLines(g, tick, L);
    }
    ReleaseDC(g_probe, hdc);
    return true;
}

// 期望放置位置(屏幕坐标):宿主内紧贴托盘区,对边居中。
RECT ComputeDesiredScreenRect(int w, int h) {
    RECT hscr{};
    GetWindowRect(g_host, &hscr);
    if (g_at.x >= 0) { // -at 覆盖:直接用屏幕坐标,仅夹紧到任务栏所在显示器内
        HMONITOR mon = MonitorFromRect(&hscr, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(mon, &mi);
        RECT S{g_at.x, g_at.y, g_at.x + w, g_at.y + h};
        if (S.left < mi.rcMonitor.left) S.left = mi.rcMonitor.left;
        if (S.top < mi.rcMonitor.top) S.top = mi.rcMonitor.top;
        if (S.right > mi.rcMonitor.right) S.right = mi.rcMonitor.right;
        if (S.bottom > mi.rcMonitor.bottom) S.bottom = mi.rcMonitor.bottom;
        return S;
    }
    int hw = hscr.right - hscr.left, hh = hscr.bottom - hscr.top;
    bool horizontal = hw > hh; // 经典底/顶任务栏
    RECT tscr{};
    bool have_tray = g_tray && GetWindowRect(g_tray, &tscr);
    RECT S{};
    int gap = Scaled(6);
    if (horizontal) {
        int tx = have_tray ? tscr.left : hscr.right - Scaled(110); // 降级:右端约110px 估托盘
        S.left = tx - gap - w;
        if (S.left < hscr.left + 2) S.left = tx + gap; // 空间不足→改到托盘右侧
        S.top = hscr.top + (hh - h) / 2;
    } else { // 左/右边缘任务栏
        int ty = have_tray ? tscr.top : hscr.bottom - Scaled(110);
        S.top = ty - gap - h;
        if (S.top < hscr.top + 2) S.top = ty + gap;
        S.left = hscr.left + (hw - w) / 2;
    }
    // 夹紧进宿主屏幕矩形(防跑偏可见)
    if (S.left < hscr.left) S.left = hscr.left;
    if (S.top < hscr.top) S.top = hscr.top;
    if (S.left + w > hscr.right) S.left = hscr.right - w;
    if (S.top + h > hscr.bottom) S.top = hscr.bottom - h;
    return S;
}

void PlaceAndShow(int w, int h, bool first) {
    RECT S = ComputeDesiredScreenRect(w, h);
    // 父化窗口的 SetWindowPos 按父客户区坐标解释:屏幕坐标 S → 父客户坐标 S-org。
    // 每 tick 都重发一次(幂等),回读 GetWindowRect(屏幕帧)仅用于日志与偏差上报。
    int px = S.left - g_org.x, py = S.top - g_org.y;
    // float 对照需 TOPMOST 压过任务栏本身;父化模式用 TOP(兄弟序最高即可)
    HWND after = g_float ? HWND_TOPMOST : HWND_TOP;
    SetWindowPos(g_probe, after, px, py, w, h,
                 SWP_NOACTIVATE | (first || !g_probe_visible ? SWP_SHOWWINDOW : 0));
    g_probe_visible = true;
    RECT a{};
    GetWindowRect(g_probe, &a);
    bool moved = g_last_placed.x != S.left || g_last_placed.y != S.top;
    g_last_placed = {S.left, S.top};
    if (first || moved) {
        int dx = a.left - S.left, dy = a.top - S.top;
        Log("[place] S=(%d,%d) %dx%d 实际=(%d,%d) 偏差=(%d,%d) tray=%s", S.left,
            S.top, w, h, a.left, a.top, dx, dy, g_tray ? "真实" : "估算");
    }
}

// 重建探针窗口(父窗口 Shell_TrayWnd 销毁会递归销毁子窗口,宿主重启后必须重建)
HWND CreateProbeWindow() {
    HWND h = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                                 WS_EX_TRANSPARENT,
                             kClass, L"taskbar_probe", WS_POPUP, 0, 0, 10, 10,
                             nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (h && g_colorkey) {
        if (!SetLayeredWindowAttributes(h, RGB(255, 0, 255), 255, LWA_COLORKEY)) {
            Log("[rebuild] SetLayeredWindowAttributes 失败 err=%lu", GetLastError());
            DestroyWindow(h);
            return nullptr;
        }
    }
    return h;
}

// 确保 g_probe 有效;被宿主销毁则重建
bool EnsureProbe() {
    if (g_probe && IsWindow(g_probe)) return true;
    if (g_probe) Log("[rebuild] 探针窗口已被销毁(宿主连带),重建");
    g_probe = CreateProbeWindow();
    if (!g_probe) {
        Log("[rebuild] 重建失败 err=%lu", GetLastError());
        return false;
    }
    g_probe_visible = false;
    return true;
}

void Detach(const char* why) {
    if (!g_attached) return;
    Log("[detach] %s", why);
    if (g_probe && IsWindow(g_probe)) {
        ShowWindow(g_probe, SW_HIDE);
        g_probe_visible = false;
        if (!g_float) SetParent(g_probe, nullptr); // 脱离宿主(可能已死)
    }
    g_attached = false;
    g_host = nullptr;
    g_tray = nullptr;
    g_last_placed = {-1, -1};
}

bool TryAttach() {
    if (g_attached) return true;
    HWND host = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!host) return false;
    if (!EnsureProbe()) return false;
    if (!g_float) {
        if (!SetParent(g_probe, host)) {
            Log("[attach] SetParent 失败 err=%lu", GetLastError());
            return false;
        }
    }
    g_attached = true;
    g_host = host;
    POINT p{0, 0};
    ClientToScreen(host, &p);
    g_org = g_float ? POINT{0, 0} : p; // float 模式未父化:SetWindowPos 为屏幕帧
    g_dpi = (int)GetDpiForWindow(host);
    if (g_dpi <= 0) g_dpi = 96;
    RECT hc{};
    GetClientRect(host, &hc);
    g_tray = FindChildByClass(host, L"TrayNotifyWnd");
    Log("[attach] host=0x%p %s dpi=%d client=%dx%d org=(%d,%d) tray=%s",
        (void*)host, g_float ? "[float 对照 未父化]" : "", g_dpi,
        (int)(hc.right - hc.left), (int)(hc.bottom - hc.top), p.x, p.y,
        g_tray ? "找到" : "未找到(用估算锚点)");
    return true;
}

void Tick() {
    g_tick++;
    // 1) 宿主失效 → 脱离
    if (g_host && (!IsWindow(g_host))) {
        Detach("Shell_TrayWnd 失效(疑似 Explorer 重启)");
        return;
    }
    // 2) 未挂载 → 尝试挂载
    if (!g_attached && TryAttach()) { /* 首次挂载:随后布局 */ }
    if (!g_attached) {
        if (g_tick % 5 == 0) Log("[wait] 宿主未找到 t=%d", g_tick);
        return;
    }

    // 3) 宿主是否可见(自动隐藏检测):屏幕内可见面积 <50% 视为隐藏
    RECT hscr{};
    GetWindowRect(g_host, &hscr);
    int hw = hscr.right - hscr.left, hh = hscr.bottom - hscr.top;
    HMONITOR mon = MonitorFromWindow(g_host, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    RECT inter{};
    IntersectRect(&inter, &hscr, &mi.rcMonitor);
    int iv = (inter.right - inter.left) * (inter.bottom - inter.top);
    bool vis = (long long)iv * 2 >= (long long)hw * hh;
    if (vis != g_host_visible) {
        g_host_visible = vis;
        Log("[hide] 宿主可见=%s t=%d 屏内面积=%d/%d", vis ? "是" : "否", g_tick, iv,
            hw * hh);
    }

    // 4) 绘制 + 布局(每秒整窗重绘 → 观察闪烁)
    int w = 0, h = 0;
    bool painted = g_colorkey ? PaintColorKey(g_tick, w, h) : PaintUlw(g_tick, w, h);
    if (!painted || w <= 0 || h <= 0) return;
    if (!g_host_visible) {
        if (g_probe_visible) {
            ShowWindow(g_probe, SW_HIDE);
            g_probe_visible = false;
        }
        return;
    }
    bool was_hidden = !g_probe_visible;
    PlaceAndShow(w, h, was_hidden || g_tick <= 1); // 每 tick 幂等重发位置

    // 5) 心跳
    if (g_tick % 10 == 0) {
        RECT pr{};
        GetWindowRect(g_probe, &pr);
        Log("[hb] t=%d attached=%d host可见=%d probe=(%d,%d %dx%d)", g_tick,
            (int)g_attached, (int)g_host_visible, pr.left, pr.top,
            pr.right - pr.left, pr.bottom - pr.top);
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    int limit = 0; // 0 = 无限
    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"-t") == 0 && i + 1 < argc) limit = _wtoi(argv[i + 1]);
        if (wcscmp(argv[i], L"-float") == 0) g_float = true;
        if (wcscmp(argv[i], L"-dump") == 0) g_dump_once = true;
        if (wcscmp(argv[i], L"-at") == 0 && i + 2 < argc) {
            g_at.x = _wtoi(argv[i + 1]);
            g_at.y = _wtoi(argv[i + 2]);
        }
        if (wcscmp(argv[i], L"-colorkey") == 0) g_colorkey = true;
    }

    // 日志:exe 同目录 probe.log
    wchar_t dir[512];
    GetModuleFileNameW(nullptr, dir, 512);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    std::wstring lp = std::wstring(dir) + L"\\probe.log";
    g_log = _wfopen(lp.c_str(), L"w");

    Log("=== taskbar_probe v1 GDI+/ULW 管线 ===");
    BOOL aware = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Log("[init] DPI感知=%s", aware ? "PerMonitorV2" : "失败(按系统)");

    static GdiPlusToken gp; // 进程生命周期

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ProbeProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc)) {
        Log("[init] 注册窗口类失败 err=%lu", GetLastError());
        return 2;
    }
    g_probe = CreateProbeWindow();
    if (!g_probe) {
        Log("[init] 创建探针窗口失败 err=%lu", GetLastError());
        return 3;
    }
    if (g_colorkey) Log("[init] 色键模式:品红=透明");

    DWORD start = GetTickCount();
    Log("[init] 就绪,每 Tick 主动 Sleep 驱动;Ctrl+C / taskkill 结束");
    for (;;) {
        Tick();
        if (limit > 0 && (GetTickCount() - start) >= (DWORD)limit * 1000) {
            Log("[exit] 到达 -t %d 秒上限", limit);
            break;
        }
        // 处理 WM_QUIT/窗口消息(维持消息泵,虽无输入)
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) goto done;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(200);
    }
done:
    Log("[exit] 清理");
    if (g_attached) Detach("退出");
    if (g_probe) DestroyWindow(g_probe);
    if (g_log) fclose(g_log);
    return 0;
}
