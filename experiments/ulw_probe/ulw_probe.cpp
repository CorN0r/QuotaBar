// ulw_probe.cpp - Minimal per-path reproduction of the invisible-ULW issue.
//
// Question: on this machine (Win11 25H2 26200, Intel Iris Xe, 3120x2080 @200%),
// plain GDI windows and color-key layered windows (SLWA+LWA_COLORKEY) are
// visible, but per-pixel-alpha UpdateLayeredWindow content never reaches any
// screenshot, although the window exists, is visible, ULW returns TRUE and the
// DIB content is correct (verified via -dump in taskbar_probe).
//
// This probe runs sequential phases, one window each, at a fixed physical-pixel
// spot, so a capture script can screenshot the region between phases and
// classify whether the expected color appeared:
//
//   1 plain_gdi     yellow   WS_POPUP, direct GDI fill               (sanity)
//   2 colorkey      red      SLWA + LWA_COLORKEY magenta bg          (known good)
//   3 slwa_alpha    red      SLWA + LWA_ALPHA 128, GDI fill          (alpha path of SLWA)
//   4 ulw_pix       red      ULW per-pixel alpha, opaque             (the failing path)
//   5 ulw_flat      green    ULW, AlphaFormat=0, SourceConstantAlpha=255
//   6 ulw_flat_a    blue     ULW, AlphaFormat=0, SourceConstantAlpha=128
//   7 ulw_pix_show  orange   ULW after ShowWindow (reversed order)
//   8 ulw_pix_nt    cyan     ULW per-pixel, WITHOUT WS_EX_TRANSPARENT
//
// Order of phases mirrors the real overlay for the default case: create hidden,
// paint, then SetWindowPos(SWP_SHOWWINDOW). Phase 7 reverses paint/show.
//
// Extra evidence per ULW phase (DWM-side):
//   * DWMWA_CLOAKED state after show
//   * PrintWindow(PW_RENDERFULLCONTENT) saved to pw_N.png - what DWM believes
//     the window content is, independent of what the screen shows.
//
// Flags: -sysdpi  run System DPI aware instead of PerMonitorV2
//        -only N  run a single phase
//        -ms N    phase duration in ms (default 3000)
//        -demo    show three side-by-side windows (plain GDI / colorkey / ULW)
//                 for -ms ms each (log "DEMO ..." lines), for eyeball checks
// Build:  ulw_build.bat

#include <windows.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <cmath>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")

namespace {

constexpr int kX = 600, kY = 300, kW = 300, kH = 150;

FILE* g_log = nullptr;
bool g_sysdpi = false;
bool g_demo = false;
bool g_dib_text = false; // -text: ULW demo window content drawn via GDI+ (overlay pipeline)
int g_demo_y = 1150;
DWORD g_ulw_rgb = 0x000000FF; // COLORREF fill for the demo ULW window (default red)
int g_only = 0;
int g_ms = 3000;

void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
    fflush(stdout);
    if (g_log) { fprintf(g_log, "%s\n", buf); fflush(g_log); }
}

struct GdiPlusToken {
    Gdiplus::GdiplusStartupInput in;
    ULONG_PTR token = 0;
    GdiPlusToken() { Gdiplus::GdiplusStartup(&token, &in, nullptr); }
    ~GdiPlusToken() { if (token) Gdiplus::GdiplusShutdown(token); }
};

struct CloakCheck { bool has = false; DWORD v = 0; };
CloakCheck GetCloak(HWND h) {
    CloakCheck c;
    c.has = SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &c.v, sizeof(c.v)));
    return c;
}

// Save the DWM-side content of a window via PrintWindow(PW_RENDERFULLCONTENT).
void DumpPrintWindow(HWND hwnd, const wchar_t* path, int w, int ph) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -ph;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC scr = GetDC(nullptr);
    HBITMAP dib = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC mem = CreateCompatibleDC(scr);
    ReleaseDC(nullptr, scr);
    HGDIOBJ old = mem ? SelectObject(mem, dib) : nullptr;
    if (!dib || !bits || !mem) { Log("  [pw] alloc fail"); return; }
    BOOL ok = PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT);
    Log("  [pw] PrintWindow(PW_RENDERFULLCONTENT)=%s err=%lu", ok ? "OK" : "FAIL",
        GetLastError());
    {
        Gdiplus::Bitmap bmp(w, ph, 4 * w, PixelFormat32bppARGB, (BYTE*)bits);
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
        if (enc.Data1) {
            Gdiplus::Status st = bmp.Save(path, &enc, nullptr);
            Log("  [pw] saved %ls st=%d", path, (int)st);
        }
    }
    SelectObject(mem, old);
    DeleteDC(mem);
    DeleteObject(dib);
}

struct Phase {
    int id;
    const char* name;
    DWORD ex_layered;      // 0 = plain window, 1 = layered path flags
    bool color_key;
    bool slwa_alpha;       // LWA_ALPHA value used with SLWA
    bool ulw;              // render via ULW
    bool ulw_pixalpha;     // ULW AlphaFormat=AC_SRC_ALPHA
    BYTE ulw_const;        // ULW SourceConstantAlpha
    bool show_first;       // ShowWindow before paint (ULW phases only)
    DWORD rgb;             // fill color
    bool no_transparent;   // omit WS_EX_TRANSPARENT
};

// DIB paint helpers (top-down 32bpp, alpha premultiplied where used).
// rgb is a COLORREF (0x00BBGGRR). Returns bits via bits_out when requested.
HBITMAP MakeDib(int w, int h, DWORD rgb, bool opaque_alpha, void** bits_out = nullptr) {
    void* bits = nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC scr = GetDC(nullptr);
    HBITMAP dib = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, scr);
    if (!dib || !bits) { if (dib) DeleteObject(dib); return nullptr; }
    if (bits_out) *bits_out = bits;
    BYTE* p = (BYTE*)bits;
    for (int i = 0; i < w * h; i++) {
        BYTE a = opaque_alpha ? 255 : 0;
        if (!opaque_alpha) { a = 255; } // alpha channel value irrelevant when not used
        p[i * 4 + 0] = GetBValue(rgb);
        p[i * 4 + 1] = GetGValue(rgb);
        p[i * 4 + 2] = GetRValue(rgb);
        p[i * 4 + 3] = a;
    }
    // 4px white border to make partial placement visible
    for (int x = 0; x < w; x++) {
        for (int b = 0; b < 4; b++) {
            int row0 = b, row1 = h - 1 - b;
            BYTE* q0 = p + ((size_t)row0 * w + x) * 4;
            BYTE* q1 = p + ((size_t)row1 * w + x) * 4;
            for (int c = 0; c < 3; c++) { q0[c] = q1[c] = 255; }
            q0[3] = q1[3] = 255;
        }
    }
    for (int y = 0; y < h; y++) {
        for (int b = 0; b < 4; b++) {
            BYTE* q0 = p + ((size_t)y * w + b) * 4;
            BYTE* q1 = p + ((size_t)y * w + (w - 1 - b)) * 4;
            for (int c = 0; c < 3; c++) { q0[c] = q1[c] = 255; }
            q0[3] = q1[3] = 255;
        }
    }
    return dib;
}

LRESULT CALLBACK ProbeProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            EndPaint(h, &ps);
            return 0;
        }
        default: return DefWindowProcW(h, m, w, l);
    }
}

void RunPhase(const Phase& ph) {
    Log("PHASE %d %s  (%.6X%s%s)", ph.id, ph.name, ph.rgb,
        ph.show_first ? " show_first" : "",
        ph.no_transparent ? " no_transparent" : "");

    DWORD ex = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (ph.ex_layered) ex |= WS_EX_LAYERED;
    if (!ph.no_transparent) ex |= WS_EX_TRANSPARENT;

    HWND hwnd = CreateWindowExW(ex, L"UlwProbeWnd", L"ulw_probe", WS_POPUP,
                                0, 0, kW, kH, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) { Log("  [create] fail err=%lu", GetLastError()); return; }

    if (ph.color_key) {
        if (!SetLayeredWindowAttributes(hwnd, RGB(255, 0, 255), 255, LWA_COLORKEY))
            Log("  [slwa] colorkey fail err=%lu", GetLastError());
    } else if (ph.slwa_alpha) {
        if (!SetLayeredWindowAttributes(hwnd, 0, 128, LWA_ALPHA))
            Log("  [slwa] alpha fail err=%lu", GetLastError());
    }

    // Common paint: fill color band + magenta base when colorkey.
    if (!ph.ulw) {
        HDC hdc = GetDC(hwnd);
        if (ph.color_key) {
            RECT whole{0, 0, kW, kH};
            HBRUSH key = CreateSolidBrush(RGB(255, 0, 255));
            FillRect(hdc, &whole, key);   // window rect whole area
            DeleteObject(key);
            RECT inner{0, 0, kW, 60};
            HBRUSH br = CreateSolidBrush(RGB(GetRValue(ph.rgb), GetGValue(ph.rgb),
                                             GetBValue(ph.rgb)));
            FillRect(hdc, &inner, br);
            DeleteObject(br);
        } else {
            RECT whole{0, 0, kW, kH};
            HBRUSH br = CreateSolidBrush(RGB(GetRValue(ph.rgb), GetGValue(ph.rgb),
                                             GetBValue(ph.rgb)));
            FillRect(hdc, &whole, br);
            DeleteObject(br);
        }
        ReleaseDC(hwnd, hdc);
    } else {
        HBITMAP dib = MakeDib(kW, kH, ph.rgb, ph.ulw_pixalpha);
        if (!dib) { Log("  [dib] fail"); DestroyWindow(hwnd); return; }
        HDC scr = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(scr);
        ReleaseDC(nullptr, scr);
        HGDIOBJ old = SelectObject(mem, dib);
        SIZE sz{kW, kH};
        POINT zero{0, 0};
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = ph.ulw_const;
        blend.AlphaFormat = ph.ulw_pixalpha ? AC_SRC_ALPHA : 0;
        HDC wdc = GetDC(hwnd);
        BOOL ok = UpdateLayeredWindow(hwnd, wdc, nullptr, &sz, mem, &zero, 0, &blend,
                                      ULW_ALPHA);
        Log("  [ulw] call=%s err=%lu pixalpha=%d const=%u", ok ? "TRUE" : "FAIL",
            GetLastError(), (int)ph.ulw_pixalpha, ph.ulw_const);
        ReleaseDC(hwnd, wdc);
        SelectObject(mem, old);
        DeleteDC(mem);
        DeleteObject(dib);
    }

    // Placement at the fixed physical-pixel spot.
    int sx = kX, sy = kY;
    if (g_sysdpi) { // system aware: coords are virtualized logical px
        UINT dpi = GetDpiForSystem();
        sx = kX * 96 / (int)dpi;
        sy = kY * 96 / (int)dpi;
    }
    if (ph.show_first) {
        SetWindowPos(hwnd, HWND_TOPMOST, sx, sy, kW, kH,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        Sleep(300); // let show settle before ULW
        // paint again via ULW on the already-visible window
        HBITMAP dib = MakeDib(kW, kH, ph.rgb, true);
        if (!dib) { DestroyWindow(hwnd); return; }
        HDC scr = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(scr);
        ReleaseDC(nullptr, scr);
        HGDIOBJ old = SelectObject(mem, dib);
        SIZE sz{kW, kH};
        POINT zero{0, 0};
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;
        HDC wdc = GetDC(hwnd);
        BOOL ok = UpdateLayeredWindow(hwnd, wdc, nullptr, &sz, mem, &zero, 0, &blend,
                                      ULW_ALPHA);
        Log("  [ulw] show_first call=%s err=%lu", ok ? "TRUE" : "FAIL", GetLastError());
        ReleaseDC(hwnd, wdc);
        SelectObject(mem, old);
        DeleteDC(mem);
        DeleteObject(dib);
    } else {
        SetWindowPos(hwnd, HWND_TOPMOST, sx, sy, kW, kH,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    RECT rc{};
    GetWindowRect(hwnd, &rc);
    CloakCheck ck = GetCloak(hwnd);
    Log("  [win] visible=%d rect=%ld,%ld %ldx%ld cloak=%s%s", (int)IsWindowVisible(hwnd),
        rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
        ck.has ? (ck.v ? "CLOAKED" : "not-cloaked") : "n/a",
        ph.ulw || ph.slwa_alpha || ph.color_key ? "" : "");

    if (ph.ulw) {
        wchar_t path[64];
        swprintf(path, 64, L"pw_%d.png", ph.id);
        Sleep(400);
        DumpPrintWindow(hwnd, path, kW, kH);
    }

    // hold the phase visible
    DWORD start = GetTickCount();
    while (GetTickCount() - start < (DWORD)g_ms) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(50);
    }
    Log("  [end] phase %d done", ph.id);
    DestroyWindow(hwnd);
    Sleep(300);
}

void PumpMs(int ms) {
    DWORD start = GetTickCount();
    while (GetTickCount() - start < (DWORD)ms) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(50);
    }
}

// Three side-by-side windows at fixed spots for eyeball ground truth.
//   1 plain yellow   at (100, 400)
//   2 colorkey red   at (500, 400)  (magenta=transparent)
//   3 ULW red        at (900, 400)  (per-pixel alpha, opaque)
void RunDemo(int ms) {
    Log("=== DEMO ===");
    struct WinSpec {
        const char* name;
        int x;
        bool ulw;
        bool ck;
    };
    const WinSpec specs[] = {
        {"plain", 200, false, false},
        {"colorkey", 600, false, true},
        {"ulw", 1000, true, false},
    };
    HWND wins[3] = {};
    for (int i = 0; i < 3; i++) {
        const WinSpec& s = specs[i];
        DWORD ex = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
        if (s.ulw || s.ck) ex |= WS_EX_LAYERED;
        if (!s.ulw) ex |= WS_EX_TRANSPARENT;
        HWND h = CreateWindowExW(ex, L"UlwProbeWnd", L"demo", WS_POPUP, 0, 0, kW, kH,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!h) { Log("  [demo %s] create fail err=%lu", s.name, GetLastError()); continue; }
        wins[i] = h;
        if (s.ck) SetLayeredWindowAttributes(h, RGB(255, 0, 255), 255, LWA_COLORKEY);
        if (s.ulw) {
            // g_ulw_rgb is a COLORREF (0x00BBGGRR); caller passes RGB(r,g,b)
            void* raw = nullptr;
            HBITMAP dib = MakeDib(kW, kH, g_ulw_rgb, true, &raw); // fill, white border
            if (dib && raw && g_dib_text) {
                // Overlay production pipeline: GDI+ PARGB wrap + DrawString
                Gdiplus::Bitmap bmp(kW, kH, 4 * kW, PixelFormat32bppPARGB, (BYTE*)raw);
                Gdiplus::Graphics g(&bmp);
                g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
                Gdiplus::Font font(L"Segoe UI", 14.0f, Gdiplus::FontStyleBold,
                                   Gdiplus::UnitPoint);
                Gdiplus::StringFormat sf(Gdiplus::StringFormat::GenericTypographic());
                Gdiplus::SolidBrush br(Gdiplus::Color(255, 0, 0, 0)); // black text
                g.DrawString(L"ULW TEXT OK", 11, &font, Gdiplus::RectF(10, 10, 260, 60),
                             &sf, &br);
                // dump for content self-check
                CLSID enc{};
                UINT num = 0, sz = 0;
                Gdiplus::GetImageEncodersSize(&num, &sz);
                if (sz) {
                    std::string buf(sz, '\0');
                    Gdiplus::GetImageEncoders(num, (UINT)buf.size(),
                                              (Gdiplus::ImageCodecInfo*)buf.data());
                    auto* list = (Gdiplus::ImageCodecInfo*)buf.data();
                    for (UINT ei = 0; ei < num; ei++)
                        if (wcscmp(list[ei].MimeType, L"image/png") == 0) { enc = list[ei].Clsid; break; }
                }
                if (enc.Data1) bmp.Save(L"ulw_text_dib.png", &enc, nullptr);
                Log("  [demo ulw] GDI+ text drawn, dump saved");
            }
            if (!dib) { Log("  [demo ulw] dib fail"); DestroyWindow(h); wins[i] = nullptr; continue; }
            HDC scr = GetDC(nullptr);
            HDC mem = CreateCompatibleDC(scr);
            ReleaseDC(nullptr, scr);
            HGDIOBJ old = SelectObject(mem, dib);
            SIZE sz{kW, kH};
            POINT zero{0, 0};
            BLENDFUNCTION blend{};
            blend.BlendOp = AC_SRC_OVER;
            blend.SourceConstantAlpha = 255;
            blend.AlphaFormat = AC_SRC_ALPHA;
            HDC wdc = GetDC(h);
            BOOL ok = UpdateLayeredWindow(h, wdc, nullptr, &sz, mem, &zero, 0, &blend,
                                          ULW_ALPHA);
            Log("  [demo ulw] ULW=%s err=%lu", ok ? "TRUE" : "FAIL", GetLastError());
            ReleaseDC(h, wdc);
            SelectObject(mem, old);
            DeleteDC(mem);
            DeleteObject(dib);
        } else {
            HDC wdc = GetDC(h);
            RECT whole{0, 0, kW, kH};
            HBRUSH br = CreateSolidBrush(s.ck ? RGB(255, 0, 255) : RGB(255, 255, 0));
            FillRect(wdc, &whole, br);
            DeleteObject(br);
            if (s.ck) {
                RECT band{0, 0, kW, 60};
                HBRUSH red = CreateSolidBrush(RGB(255, 0, 0));
                FillRect(wdc, &band, red);
                DeleteObject(red);
            }
            ReleaseDC(h, wdc);
        }
        SetWindowPos(h, HWND_TOPMOST, s.x, g_demo_y, kW, kH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        RECT rc{};
        GetWindowRect(h, &rc);
        CloakCheck ck = GetCloak(h);
        Log("  [demo %s] at %ld,%ld visible=%d cloak=%s", s.name, rc.left, rc.top,
            (int)IsWindowVisible(h), ck.has ? (ck.v ? "CLOAKED" : "no") : "n/a");
    }
    // Window station / desktop names for provenance
    {
        wchar_t ws[64] = {}, dk[64] = {};
        DWORD n = 0;
        HWINSTA hws = GetProcessWindowStation();
        HDESK hdk = GetThreadDesktop(GetCurrentThreadId());
        if (hws && GetUserObjectInformationW(hws, UOI_NAME, ws, sizeof(ws), &n)) {
            Log("  [demo] windowstation=%ls", ws);
        }
        n = 0;
        if (hdk && GetUserObjectInformationW(hdk, UOI_NAME, dk, sizeof(dk), &n)) {
            Log("  [demo] desktop=%ls", dk);
        }
    }
    // Repaint non-ULW windows every 500ms while visible (post-show paint test).
    DWORD start = GetTickCount();
    while (GetTickCount() - start < (DWORD)ms) {
        for (int i = 0; i < 3; i++) {
            HWND h = wins[i];
            if (!h || specs[i].ulw || !IsWindowVisible(h)) continue;
            HDC wdc = GetDC(h);
            RECT whole{0, 0, kW, kH};
            HBRUSH br = CreateSolidBrush(specs[i].ck ? RGB(255, 0, 255) : RGB(255, 255, 0));
            FillRect(wdc, &whole, br);
            DeleteObject(br);
            if (specs[i].ck) {
                RECT band{0, 0, kW, 60};
                HBRUSH red = CreateSolidBrush(RGB(255, 0, 0));
                FillRect(wdc, &band, red);
                DeleteObject(red);
            }
            ReleaseDC(h, wdc);
        }
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(100);
    }
    Log("=== DEMO done ===");
    for (int i = 0; i < 3; i++) {
        if (wins[i]) DestroyWindow(wins[i]);
    }
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-sysdpi") == 0) g_sysdpi = true;
        else if (strcmp(argv[i], "-demo") == 0) g_demo = true;
        else if (strcmp(argv[i], "-text") == 0) g_dib_text = true;
        else if (strcmp(argv[i], "-y") == 0 && i + 1 < argc) g_demo_y = atoi(argv[++i]);
        else if (strcmp(argv[i], "-ulwcolor") == 0 && i + 1 < argc)
            g_ulw_rgb = (DWORD)strtoul(argv[++i], nullptr, 16);
        else if (strcmp(argv[i], "-only") == 0 && i + 1 < argc) g_only = atoi(argv[++i]);
        else if (strcmp(argv[i], "-ms") == 0 && i + 1 < argc) g_ms = atoi(argv[++i]);
    }
    wchar_t dir[512];
    GetModuleFileNameW(nullptr, dir, 512);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    std::wstring lp = std::wstring(dir) + L"\\ulw_probe.log";
    g_log = _wfopen(lp.c_str(), L"w");

    Log("=== ulw_probe v1 ===");
    if (g_sysdpi) {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
        Log("[init] DPI感知=SystemAware");
    } else {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        Log("[init] DPI感知=PerMonitorV2");
    }
    Log("[init] 系统DPI=%u", GetDpiForSystem());

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ProbeProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"UlwProbeWnd";
    RegisterClassExW(&wc);

    static GdiPlusToken gp;

    if (g_demo) {
        RunDemo(g_ms);
        if (g_log) fclose(g_log);
        return 0;
    }

    const Phase phases[] = {
        {1,  "plain_gdi",   0,          false, false, false, false, 0,   false, 0x00FFFF00, false}, // yellow
        {2,  "colorkey",    WS_EX_LAYERED, true,  false, false, false, 0,   false, 0x00FF0000, false}, // red band
        {3,  "slwa_alpha",  WS_EX_LAYERED, false, true,  false, false, 0,   false, 0x00FF0000, false}, // red
        {4,  "ulw_pix",     WS_EX_LAYERED, false, false, true,  true,  255, false, 0x00FF0000, false}, // red
        {5,  "ulw_flat",    WS_EX_LAYERED, false, false, true,  false, 255, false, 0x0000FF00, false}, // green
        {6,  "ulw_flat_a",  WS_EX_LAYERED, false, false, true,  false, 128, false, 0x000000FF, false}, // blue
        {7,  "ulw_pix_show",WS_EX_LAYERED, false, false, true,  true,  255, true,  0x00FFA500, false}, // orange
        {8,  "ulw_pix_nt",  WS_EX_LAYERED, false, false, true,  true,  255, false, 0x0000FFFF, true},  // cyan
    };

    for (const auto& ph : phases) {
        if (g_only && ph.id != g_only) continue;
        Log("[run] 开始阶段 %d %s", ph.id, ph.name);
        RunPhase(ph);
    }
    Log("=== all phases done ===");
    if (g_log) fclose(g_log);
    return 0;
}
