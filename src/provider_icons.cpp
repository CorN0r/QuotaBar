// provider_icons.cpp — 原厂图标加载与两路绘制。
// PNG 从 RCDATA 经 IStream 交给 GDI+ 解码一次：Gdiplus::Bitmap 留作 GDI+ 直绘，
// 同源像素手工预乘成 BGRA 供 D2D（D2D 语义 = PREMULTIPLIED，GDI+ = 直值 alpha）。
#include "provider_icons.hpp"
#include "theme.hpp"
#include <objidl.h> // GDI+ 头的前置要求（IStream）
#include <gdiplus.h>
#include <memory>
#include <vector>

namespace qb {

namespace {

struct IconBuf {
    std::unique_ptr<Gdiplus::Bitmap> bmp; // GDI+ 直绘（直值 alpha）
    std::vector<BYTE> premul;             // D2D 用预乘 BGRA（含 stride，top-down）
    UINT side = 0;
    bool ready = false;
};

IconBuf* Slot(AccountType t) {
    static IconBuf buf[4]; // AccountType { Kimi, Codex, Glm, Deepseek }
    return &buf[(int)t];
}

const wchar_t* ResName(AccountType t) {
    switch (t) {
        case AccountType::Kimi: return L"PVID_KIMI";
        case AccountType::Codex: return L"PVID_CODEX";
        case AccountType::Glm: return L"PVID_GLM";
        case AccountType::Deepseek: return L"PVID_DEEPSEEK";
    }
    return nullptr;
}

} // namespace

void ProviderIconsInit() {
    static bool done = false;
    if (done) return;
    done = true;
    theme::EnsureGdiplus();
    HINSTANCE inst = GetModuleHandleW(nullptr);
    for (int i = 0; i < 4; ++i) {
        const auto t = (AccountType)i;
        const wchar_t* name = ResName(t);
        IconBuf* s = Slot(t);
        if (!name || !s) continue;
        HRSRC rc = FindResourceW(inst, name, (LPCWSTR)RT_RCDATA);
        HGLOBAL h = rc ? LoadResource(inst, rc) : nullptr;
        const void* bytes = h ? LockResource(h) : nullptr;
        const DWORD size = h ? SizeofResource(inst, rc) : 0;
        if (!bytes || !size) continue;
        // 资源块不可写，拷到可移动内存再包 IStream（fDeleteOnRelease 托管释放）
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
        if (!mem) continue;
        void* p = GlobalLock(mem);
        if (!p) {
            GlobalFree(mem);
            continue;
        }
        memcpy(p, bytes, size);
        GlobalUnlock(mem);
        IStream* stream = nullptr;
        if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &stream))) {
            GlobalFree(mem);
            continue;
        }
        std::unique_ptr<Gdiplus::Bitmap> bmp(Gdiplus::Bitmap::FromStream(stream));
        stream->Release(); // 释放即回收 mem
        if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) continue;
        Gdiplus::Rect full(0, 0, (INT)bmp->GetWidth(), (INT)bmp->GetHeight());
        Gdiplus::BitmapData bd{};
        if (bmp->LockBits(&full, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB,
                          &bd) != Gdiplus::Ok)
            continue;
        const int w = (int)bd.Width, hgt = (int)bd.Height;
        const int stride = (int)bd.Stride;
        s->premul.assign((size_t)stride * hgt, 0);
        for (int y = 0; y < hgt; ++y) {
            const BYTE* src = (const BYTE*)bd.Scan0 + (size_t)y * stride;
            BYTE* dst = s->premul.data() + (size_t)y * stride;
            for (int x = 0; x < w; ++x) {
                const BYTE b = src[x * 4 + 0], g2 = src[x * 4 + 1];
                const BYTE r = src[x * 4 + 2], a = src[x * 4 + 3];
                dst[x * 4 + 0] = (BYTE)((UINT)b * a / 255);
                dst[x * 4 + 1] = (BYTE)((UINT)g2 * a / 255);
                dst[x * 4 + 2] = (BYTE)((UINT)r * a / 255);
                dst[x * 4 + 3] = a;
            }
        }
        bmp->UnlockBits(&bd);
        s->side = (UINT)w;
        s->bmp = std::move(bmp);
        s->ready = true;
    }
}

bool ProviderIconReady(AccountType t) {
    IconBuf* s = Slot(t);
    return s && s->ready;
}

void DrawProviderIcon(HDC hdc, AccountType t, const RECT& box) {
    IconBuf* s = Slot(t);
    if (!s || !s->ready || !s->bmp || box.right <= box.left || box.bottom <= box.top)
        return;
    Gdiplus::Graphics g(hdc);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g.DrawImage(s->bmp.get(),
                Gdiplus::Rect(box.left, box.top, box.right - box.left,
                              box.bottom - box.top));
}

const BYTE* ProviderIconPixels(AccountType t, UINT& side) {
    IconBuf* s = Slot(t);
    if (!s || !s->ready) return nullptr;
    side = s->side;
    return s->premul.data();
}

Gdiplus::Bitmap* ProviderGpBitmap(AccountType t) {
    IconBuf* s = Slot(t);
    return s && s->ready ? s->bmp.get() : nullptr;
}

} // namespace qb
