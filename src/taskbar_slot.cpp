// taskbar_slot.cpp — 任务栏内嵌槽实现（设计见 taskbar_slot.hpp）。
// 渲染路径:父化子窗口 + WS_EX_NOREDIRECTIONBITMAP(无重定向位图,不遮挡宿主内容)
// + DirectComposition surface(1:1 物理像素,每帧 D2D1+DWrite 绘制后 Commit)。
// 坐标系纪律:几何决策用屏幕坐标,SetWindowPos 用父客户坐标(宿主原点每次应用前重读);
// 仅在几何/内容/可见性变化时动作,静态期零 Commit(§12)。
#include "taskbar_slot.hpp"
#include "log.hpp"
#include "provider_icons.hpp"
#include "util.hpp"
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi.h>
#include <cmath>
#include <cwchar>

namespace qb {

// ---------- 兼容层常量(§10.2:系统窗口类识别隔离在本模块) ----------
namespace {
constexpr wchar_t kSlotClass[] = L"QuotaBarSlotWnd";
constexpr wchar_t kShellTrayClass[] = L"Shell_TrayWnd";
constexpr wchar_t kTrayNotifyClass[] = L"TrayNotifyWnd";
constexpr int kAnchorDepthMax = 6;          // 递归深度上限(探针同款)
constexpr int kTrayEstWidthDip = 110;       // 无 TrayNotifyWnd 时右端估算宽(探针同款)
constexpr int kGapDip = 6;                  // 贴托盘间隙(探针实测值)
constexpr int kStartPadDip = 30;            // 文字条与开始按钮的常规留白(用户实机拍板)
constexpr int kMinPadDip = 4;               // 内容较宽时放开到的贴靠留白下限(自适应长度)
constexpr int kWeatherWidthDip = 115;       // 天气/资讯小组件的估算宽(无 HWND)。本机实机量:
                                            // "25℃+说明"≈196物理≈98DIP(2026-09-06 像素实测),
                                            // 预留变长(阴雨/长说明)缓冲后取 115DIP
constexpr int kSegGapDip = 4;               // 段间距（值行内部段）
constexpr int kColGapDip = 12;              // 列间距（两行嵌入：名称列/值列的间隔）
constexpr float kLayoutMax = 16384.0f;      // DWrite 布局上限(无换行测量)
// CreateTargetForHwnd 的 topmost 标志。实测(2026-09-05):合成交换链内容
// topmost=TRUE 显示正常;FALSE 未与交换链复测。留待 M2 全屏场景验证:
// 若全屏应用下文字悬浮残留则改 FALSE,否则保持 TRUE 并回写 CLAUDE.md 固化。
constexpr BOOL kTargetTopmost = TRUE;

// 渲染路径结论(2026-09-05 实机逐项验证,回写 CLAUDE.md「已知状态」):
// ① 窗口必须「顶层 WS_POPUP 创建 → SetParent(Shell_TrayWnd)」——出生即子窗的窗口
//    在该 XAML 任务栏上不合成任何内容(ULW/色键/GDI/DComp 表面/交换链全灭,z 序无关);
// ② 内容载体 = 合成交换链 CreateSwapChainForComposition(首参传 D3D 设备;
//    传 v1 DComp 设备 DXGI_ERROR_INVALID_CALL;须 Device2/桌面设备直求创建);
// ③ 绘制 = D2D1 v1 CreateDxgiSurfaceRenderTarget 直绑交换链后缓冲
//    (位图包装路径在本机 E_INVALIDARG;DComp surface BeginDraw 直返上下文不受支持);
// ④ SetContent / Present 后均需 dcomp Commit。
} // namespace

// ---------- 纯函数实现 ----------

SlotLayout ComputeSlotLayout(const RECT& host_rc, const RECT& tray_rc, int content_w,
                             int content_h, int gap_px, int min_avail_px) {
    SlotLayout out;
    const int host_w = host_rc.right - host_rc.left;
    const int host_h = host_rc.bottom - host_rc.top;
    if (host_w <= 0 || host_h <= 0 || content_w <= 0 || content_h <= 0) return out;
    if (host_h > host_w) return out;               // 垂直任务栏:本期范围外
    if (content_h > host_h) return out;            // 内容高于宿主:放不下
    if (gap_px < 0) gap_px = 0;
    if (min_avail_px < 0) min_avail_px = 0;

    // 两侧可用宽(锚点须落在宿主内;越界侧视为不可用)
    int avail_l = (tray_rc.left > host_rc.left && tray_rc.left < host_rc.right)
                      ? tray_rc.left - host_rc.left - gap_px
                      : -1;
    int avail_r = (tray_rc.right > host_rc.left && tray_rc.right < host_rc.right)
                      ? host_rc.right - tray_rc.right - gap_px
                      : -1;

    auto place = [&](bool left_side, int w, int avail) {
        out.avail_w = avail;
        const int y = host_rc.top + (host_h - content_h) / 2;
        if (left_side) {
            out.rc.left = tray_rc.left - gap_px - w; // 右缘贴 tray.left-gap
        } else {
            out.rc.left = tray_rc.right + gap_px;
        }
        out.rc.right = out.rc.left + w;
        out.rc.top = y;
        out.rc.bottom = y + content_h;
    };

    // 首选:托盘左侧整宽放得下且不低于最小可用宽
    if (content_w <= avail_l && avail_l >= min_avail_px) {
        out.placement = SlotPlacement::LeftOfTray;
        place(true, content_w, avail_l);
        return out;
    }
    // 次选:托盘右侧
    if (content_w <= avail_r && avail_r >= min_avail_px) {
        out.placement = SlotPlacement::RightOfTray;
        place(false, content_w, avail_r);
        return out;
    }
    // 两侧都放不下:选可用宽较大的一侧落位(宽=可用宽),由调用方裁切内容后复算
    if (avail_l < min_avail_px && avail_r < min_avail_px) return out; // NoSpace
    if (avail_l >= avail_r) {
        out.placement = SlotPlacement::LeftOfTray;
        place(true, avail_l < content_w ? avail_l : content_w, avail_l);
    } else {
        out.placement = SlotPlacement::RightOfTray;
        place(false, avail_r < content_w ? avail_r : content_w, avail_r);
    }
    return out;
}

bool HostVisible(const RECT& host_rc, const RECT& monitor_rc) {
    RECT i{};
    if (!IntersectRect(&i, &host_rc, &monitor_rc)) return false;
    const LONG64 host_area =
        (LONG64)(host_rc.right - host_rc.left) * (host_rc.bottom - host_rc.top);
    if (host_area <= 0) return false;
    const LONG64 vis_area = (LONG64)(i.right - i.left) * (i.bottom - i.top);
    return vis_area * 2 >= host_area; // 屏内面积 ≥ 一半(探针同款判定)
}

bool ResolveSlotDarkText(int text_color_mode, bool apps_light_theme) {
    if (text_color_mode == 2) return true;  // 显式黑字
    if (text_color_mode == 1) return false; // 显式白字
    return apps_light_theme;                // 自动:浅色任务栏→黑字,深色→白字
}

int ClampSlotFontPx(int want_px, int host_h_px, int min_px, int v_margin_px) {
    const int cap = host_h_px - 2 * v_margin_px;
    if (cap < min_px) return 0;                 // 宿主装不下最小字号
    return want_px < cap ? want_px : cap;
}

std::vector<DisplaySegment> TruncateSlotSegs(const std::vector<DisplaySegment>& in,
                                             float max_w, float gap_w,
                                             const std::wstring& ellipsis,
                                             SlotMeasureFn measure, void* ctx) {
    std::vector<DisplaySegment> out;
    if (in.empty() || max_w <= 0 || !measure) return out;
    auto width_of = [&](const std::vector<DisplaySegment>& v) {
        float w = 0.0f;
        bool first = true;
        for (const auto& s : v) {
            if (!first) w += gap_w;
            w += measure(s.text, ctx);
            first = false;
        }
        return w;
    };
    if (width_of(in) <= max_w) return in; // 全部放得下

    // 从头保留尽量多的完整段
    size_t k = 0;
    float acc = 0.0f;
    bool first = true;
    while (k < in.size()) {
        const float add = (first ? 0.0f : gap_w) + measure(in[k].text, ctx);
        if (acc + add > max_w) break;
        acc += add;
        ++k;
        first = false;
    }
    out.assign(in.begin(), in.begin() + k);

    if (k < in.size()) {
        // 预留与下一段的间隙与省略号宽,再逐字符截短
        float budget = max_w - acc - (k > 0 ? gap_w : 0.0f);
        const float ell_w = measure(ellipsis, ctx);
        const float remain = budget - ell_w;
        const auto& seg = in[k];
        size_t take = 0;
        if (remain > 0) {
            float w = 0.0f;
            for (size_t i = 0; i < seg.text.size(); ++i) {
                const float w1 = measure(seg.text.substr(0, i + 1), ctx);
                if (w1 > remain) break;
                w = w1;
                take = i + 1;
            }
        }
        if (take > 0 || ell_w <= budget) {
            DisplaySegment part = seg;
            part.text = seg.text.substr(0, take) + ellipsis;
            out.push_back(std::move(part));
        }
    }
    return out; // 连省略号也放不下 → 仅保留完整前缀段(可能为空)
}

void ComputeEmbedCols(const std::vector<float>& name_w, const std::vector<float>& value_w,
                      float gap, float avail, std::vector<float>& col_w, float& total) {
    col_w.assign(name_w.size(), 0.0f);
    total = 0.0f;
    const size_t n = name_w.size();
    if (n == 0) return;
    float natural = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        col_w[i] = std::max(name_w[i], value_w[i]);
        natural += col_w[i];
    }
    natural += gap * (float)(n - 1); // 列间 gap
    if (natural <= avail) {
        total = natural;
        return;
    }
    // 超宽：内容(不含 gap)按可用比例收缩,占满 avail
    float content = natural - gap * (float)(n - 1);
    float scale = (avail - gap * (float)(n - 1)) / content;
    if (scale < 0.0f) scale = 0.0f;
    for (size_t i = 0; i < n; ++i) col_w[i] *= scale;
    total = avail;
}

const char* SlotDegradeText(SlotDegrade d) {
    switch (d) {
        case SlotDegrade::NoHost: return "任务栏不可用";
        case SlotDegrade::NotPrimary: return "任务栏不在主屏";
        case SlotDegrade::NoSpace: return "任务栏无空闲空间";
        case SlotDegrade::RenderFail: return "渲染初始化失败";
        default: return "未知";
    }
}

// ---------- 递归锚点查找(深度 ≤6,探针同款;§10.2 隔离) ----------
namespace {
HWND FindChildByClass(HWND parent, const wchar_t* cls, int depth) {
    if (!IsWindow(parent) || depth > kAnchorDepthMax) return nullptr;
    struct Probe {
        const wchar_t* cls;
        HWND out;
    } p{cls, nullptr};
    EnumChildWindows(parent, [](HWND h, LPARAM lp) -> BOOL {
        auto* q = (Probe*)lp;
        wchar_t buf[64];
        if (GetClassNameW(h, buf, 64) != 0 && wcscmp(buf, q->cls) == 0) {
            q->out = h;
            return FALSE;
        }
        return TRUE;
    }, (LPARAM)&p);
    if (p.out) return p.out;
    struct Deep {
        const wchar_t* cls;
        int depth;
        HWND out;
    } d{cls, depth, nullptr};
    EnumChildWindows(parent, [](HWND h, LPARAM lp) -> BOOL {
        auto* q = (Deep*)lp;
        if (q->out) return FALSE;
        q->out = FindChildByClass(h, q->cls, q->depth + 1);
        return q->out ? FALSE : TRUE;
    }, (LPARAM)&d);
    return d.out;
}
} // namespace

// ---------- 窗口对象 ----------

TaskbarSlot::TaskbarSlot() = default;
TaskbarSlot::~TaskbarSlot() { Destroy(); }

bool TaskbarSlot::Create(HINSTANCE hinst) {
    inst_ = hinst;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &TaskbarSlot::SlotProc;
    wc.hInstance = hinst;
    wc.lpszClassName = kSlotClass;
    if (!RegisterClassExW(&wc)) return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return true;
}

void TaskbarSlot::Destroy() {
    exiting_ = true;
    active_ = false;
    SetVisible(false);
    if (wnd_ && IsWindow(wnd_)) DestroyWindow(wnd_);
    wnd_ = nullptr;
    host_ = nullptr;
    ReleaseAll();
}

LRESULT CALLBACK TaskbarSlot::SlotProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCDESTROY) {
        // 宿主销毁会连带销毁子窗:窗口句柄作废,合成对象交下次重建(退出除外)
        auto* self = (TaskbarSlot*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (self && !self->exiting_ && self->wnd_ == h) {
            self->wnd_ = nullptr;
            self->ResetComposition();
        }
        return DefWindowProcW(h, m, w, l);
    }
    switch (m) {
        case WM_NCHITTEST:
            return HTTRANSPARENT; // 纯展示:点击穿透(交任务栏自身处理)
        case WM_ERASEBKGND:
            return 1; // 内容全部经合成交换链呈现,窗口自身无绘制
        default:
            return DefWindowProcW(h, m, w, l);
    }
}

// ---------- 事件入口 ----------

void TaskbarSlot::Update(const std::vector<EmbedRowItem>& items, const DisplayConfig& cfg) {
    items_ = items;
    cfg_ = cfg;
    if (!active_) {
        active_ = true;
        ClearDegrade(); // 重新切入=新 episode,允许再次尝试
    }
    content_dirty_ = true;
    PresentIfActive();
}

void TaskbarSlot::OnTaskbarCreated() {
    if (!active_ || exiting_) return;
    // Explorer 重启:旧宿主/旧子窗已(或即将)被销毁 → 整窗重建(detach→rebuild→attach)
    wnd_ = nullptr;
    host_ = nullptr;
    tray_hwnd_ = nullptr;
    start_hwnd_ = nullptr; // 锚缓存随旧宿主作废,防钉死到已亡布局
    start_cached_ = RECT{};
    est_tray_logged_ = false;
    ResetComposition();
    ClearDegrade();
    content_dirty_ = true;
    PresentIfActive();
}

void TaskbarSlot::OnDisplayChange() {
    if (!active_ || exiting_) return;
    ClearDegrade(); // NotPrimary/RenderFail 也可能随显示变化恢复
    content_dirty_ = true;
    PresentIfActive();
}

void TaskbarSlot::Deactivate() {
    active_ = false;
    SetVisible(false);
}

void TaskbarSlot::Poll() {
    if (!active_ || exiting_) return;
    switch (degrade_) {
        // §15:不再每秒重新发现/重试;恢复由 TaskbarCreated / DisplayChange / 模式重入驱动
        case SlotDegrade::NoHost:
        case SlotDegrade::NotPrimary:
        case SlotDegrade::RenderFail:
            return;
        default:
            break;
    }
    PresentIfActive();
}

// ---------- 呈现状态机(Update/Poll 共用;UI 线程) ----------

bool TaskbarSlot::PresentIfActive() {
    if (!active_ || exiting_) return false;
    if (!TryAttachHost()) return false;
    if (!IsWindow(host_)) { // 宿主已亡(子窗应已被连带销毁)
        host_ = nullptr;
        wnd_ = nullptr;
        ResetComposition();
        EnterDegrade(SlotDegrade::NoHost);
        return false;
    }
    if (wnd_ && !IsWindow(wnd_)) {
        wnd_ = nullptr;
        ResetComposition();
    }
    if (!EnsureWindow()) return false;
    if (!EnsureRender()) return false;
    // GetDpiForWindow 个别路径返回 0（全仓实测结论）：0 会让字号归零被误判"装不下"
    if (int d = (int)GetDpiForWindow(host_)) dpi_ = d;
    const RECT hr = HostRect();
    if (hr.right <= hr.left || hr.bottom <= hr.top) return false;
    if (!HostVisible(hr, MonitorRect())) {
        // 自动隐藏收起/移出屏幕:不占位、不重绘(脏标记保留,恢复后整帧重绘)
        SetVisible(false);
        return false;
    }
    return RefreshGeometry();
}

bool TaskbarSlot::TryAttachHost() {
    if (host_) return true;
    // 降级门控(§15):NoHost/NotPrimary/RenderFail 不在此重试
    if (degrade_ == SlotDegrade::NoHost || degrade_ == SlotDegrade::NotPrimary ||
        degrade_ == SlotDegrade::RenderFail)
        return false;
    HWND h = FindWindowW(kShellTrayClass, nullptr);
    if (!h) {
        EnterDegrade(SlotDegrade::NoHost);
        return false;
    }
    RECT r{};
    if (!GetWindowRect(h, &r)) {
        EnterDegrade(SlotDegrade::NoHost);
        return false;
    }
    // 本期只支持主屏任务栏(副屏无统一锚点,范围外;见 CLAUDE.md 已知状态)
    const POINT zero{0, 0};
    if (MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY) !=
        MonitorFromPoint(zero, MONITOR_DEFAULTTOPRIMARY)) {
        EnterDegrade(SlotDegrade::NotPrimary);
        return false;
    }
    host_ = h;
    tray_hwnd_ = nullptr;
    start_hwnd_ = nullptr; // 换宿主:锚缓存作废
    start_cached_ = RECT{};
    est_tray_logged_ = false;
    if (int d = (int)GetDpiForWindow(h)) dpi_ = d; // 0 时沿用上次值（见 Poll 处注释）
    ClearDegrade(); // 成功发现宿主:脱离降级态
    return true;
}

bool TaskbarSlot::EnsureWindow() {
    if (wnd_ && IsWindow(wnd_)) return true;
    // 窗口必须以顶层 WS_POPUP 创建、再 SetParent 挂进任务栏(本机验证的铁律,
    // 见文件头渲染路径结论;出生即子窗不合成)。WS_CHILD 由 SetParent 自动补上。
    const DWORD ex = WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_NOPARENTNOTIFY;
    wnd_ = CreateWindowExW(ex, kSlotClass, L"", WS_POPUP,
                           0, 0, 0, 0, nullptr, nullptr, inst_, nullptr);
    if (wnd_ && host_) SetParent(wnd_, host_);
    if (!wnd_) {
        Log::Write(LogLevel::Error, "slot", "嵌入子窗口创建失败");
        EnterDegrade(SlotDegrade::RenderFail);
        return false;
    }
    SetWindowLongPtrW(wnd_, GWLP_USERDATA, (LONG_PTR)this);
    placed_once_ = false; // 新窗口回到 0,0:首帧必须重新落位
    return true;
}

bool TaskbarSlot::EnsureRender() {
    const bool ready = d3d_ && dxgi_dev_ && dwf_ && d2d_factory_ && dxgi_factory2_ &&
                       desktop_ && dcomp_ && target_ && visual_;
    if (ready) return true;
    ReleaseAll(); // 干净重建(首次或部分失败后)
    HRESULT hr = S_OK;
    const char* step = "";
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                           &d3d_, nullptr, nullptr);
    step = "D3D11CreateDevice";
    if (FAILED(hr)) goto fail;
    hr = d3d_.As(&dxgi_dev_);
    step = "IDXGIDevice QI";
    if (FAILED(hr)) goto fail;
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(dwf_.GetAddressOf()));
    step = "DWriteCreateFactory";
    if (FAILED(hr)) goto fail;
    // D2D1 v1 渲染目标(交换链后缓冲直绑绘制)
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&d2d_factory_));
    step = "D2D1CreateFactory";
    if (FAILED(hr)) goto fail;
    // DXGI 1.2 工厂(合成交换链创建用;IDXGIDevice → Adapter → Factory2)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        hr = dxgi_dev_->GetAdapter(&adapter);
        step = "GetAdapter";
        if (FAILED(hr)) goto fail;
        hr = adapter->GetParent(IID_PPV_ARGS(&dxgi_factory2_));
        step = "IDXGIFactory2 QI";
        if (FAILED(hr)) goto fail;
    }
    // 组合依据(本机实机逐层验证,见 taskbar_slot.hpp 渲染成员注释):
    // Device2 系:直求 IDCompositionDevice2 E_NOINTERFACE → 直求桌面设备成功;
    // target/visual/交换链互通均要求 Device2 系(DXGI 合成交换链对 v1 设备 INVALID_CALL)
    hr = DCompositionCreateDevice2(d3d_.Get(), IID_PPV_ARGS(&desktop_));
    step = "DCompositionCreateDevice2";
    if (FAILED(hr)) goto fail;
    hr = desktop_.As(&dcomp_);
    step = "IDCompositionDevice2 QI";
    if (FAILED(hr)) goto fail;
    hr = desktop_->CreateTargetForHwnd(wnd_, kTargetTopmost, &target_);
    step = "CreateTargetForHwnd";
    if (FAILED(hr)) goto fail;
    hr = dcomp_->CreateVisual(&visual_);
    step = "CreateVisual";
    if (FAILED(hr)) goto fail;
    hr = target_->SetRoot(visual_.Get());
    step = "SetRoot";
    if (FAILED(hr)) goto fail;
    if (font_px_ > 0) SetFont(); // 设备重建后格式对象作废
    return true;
fail:
    Log::WriteF(LogLevel::Error, "slot", "渲染初始化失败[%s] hr=0x%08lX", step,
                (unsigned long)hr);
    ReleaseAll();
    EnterDegrade(SlotDegrade::RenderFail);
    return false;
}

bool TaskbarSlot::RefreshGeometry() {
    // 单行模式（embed_show_names=false）：只渲染值行，总高减半（任务栏装不下两行的备选）
    const bool two_rows = cfg_.embed_show_names;
    // ---- 字号(受宿主高度约束;总高 = rows 每行 line_h_,装不下降字号 → 无空间降级) ----
    const RECT hr = HostRect();
    const int host_h = hr.bottom - hr.top;
    const int margin_px = 5 * dpi_ / 96;
    const int min_px = 6 * dpi_ / 96;
    const int want_px = cfg_.font_size_pt * dpi_ / 72; // pt → 物理 px
    int fp = ClampSlotFontPx(want_px, host_h, min_px, margin_px);
    if (fp <= 0) {
        EnterDegrade(SlotDegrade::NoSpace);
        SetVisible(false);
        return false;
    }
    if (fp != font_px_) {
        font_px_ = fp;
        SetFont();
        content_dirty_ = true;
    }
    const int rows_wanted = two_rows ? 2 : 1;
    if (rows_wanted * line_h_ > host_h - 2 * margin_px) {
        // 挤不下:按可用高度比例估算字号降一次
        int fp2 = (int)std::max(min_px,
                                fp * (host_h - 2 * margin_px) /
                                    (rows_wanted * 2 * std::max(line_h_, 1)));
        if (fp2 < fp) {
            font_px_ = fp2;
            SetFont();
            content_dirty_ = true;
        }
    }
    if (rows_wanted * line_h_ > host_h - 2 * margin_px) {
        EnterDegrade(SlotDegrade::NoSpace);
        SetVisible(false);
        return false;
    }
    int rows_h = rows_wanted * line_h_;

    // ---- 内容列(名称行[可关] + 值行,按列对齐) ----
    if (items_.empty()) {
        SetVisible(false); // 无可显示内容(平台均关闭等):不占槽
        return false;
    }
    const int seg_gap = kSegGapDip * dpi_ / 96;
    const int col_gap = kColGapDip * dpi_ / 96; // 列间距(名称列之间)
    // 原厂图标模式：名称行宽=图标边长（方，随行高），不再量文字宽
    const bool name_icons = cfg_.embed_brand_icons;
    const float icon_side = (float)line_h_;
    auto name_width = [&](const EmbedRowItem& it) -> float {
        if (!two_rows) return 0.0f;
        return name_icons && ProviderIconReady(it.type) ? icon_side : Measure(it.name);
    };
    std::vector<float> name_w(items_.size()), value_w(items_.size());
    for (size_t i = 0; i < items_.size(); ++i) {
        name_w[i] = name_width(items_[i]);
        value_w[i] = WidthOf(items_[i].values, seg_gap);
    }
    float total_full = 0.0f;
    auto remeasure = [&]() {
        for (size_t i = 0; i < items_.size(); ++i) {
            name_w[i] = name_width(items_[i]);
            value_w[i] = WidthOf(items_[i].values, seg_gap);
        }
        std::vector<float> tmp;
        ComputeEmbedCols(name_w, value_w, (float)col_gap, 1e9f, tmp, total_full);
    };
    remeasure();

    // ---- 锚点几何与可用宽(物理 px;内容宽度随后由列布局核算) ----
    const int gap = kGapDip * dpi_ / 96;
    RECT tray{};
    bool estimated = false;
    const bool have_tray = TrayRect(tray, estimated);
    if (!have_tray) {
        tray = EstimateTrayRect();
        if (!est_tray_logged_) {
            est_tray_logged_ = true;
            Log::Write(LogLevel::Info, "slot", "TrayNotifyWnd 未找到,使用右端估算锚点");
        }
    }
    int y_center = hr.top + (host_h - rows_h) / 2;
    const int min_avail = std::max(2 * font_px_, gap * 2); // 可用宽下限(实机可调)

    int x0 = -1, avail = 0, right_lim = -1;
    char anchor_tag = '-'; // 诊断:L=簇左(天气→开始) R=簇右 F=托盘兜底 - =无落位
    // 锚定链(首选→回退,用户实机拍板):① 开始按钮左侧(贴天气右缘,右缘不超开始按钮留白);
    // ② 簇右缘(任务栏按钮之后);③ 托盘左/右(含 NoSpace 判定)。
    // 可用宽不足时最终由列布局按比例收缩并截断数值段。
    // 锚点与内容宽耦合(软界自适应按"当前内容宽"判定),故封装成可重跑:
    // 字号收缩后内容变窄,第二遍常能把落位放宽到贴靠软界,免去列挤压/尾段截断。
    auto compute_anchor = [&](bool final_pass) {
        x0 = -1;
        avail = 0;
        right_lim = -1;
        anchor_tag = '-';
        RECT start_rc{};
        bool offset_applied = false; // 簇左分支内联处理偏移(需在软界判定前生效)
        if (prefer_cluster_left_ && ClusterLeft(start_rc)) {
            anchor_tag = 'L';
            const int cand_x0 = hr.left + kWeatherWidthDip * dpi_ / 96 + gap; // 贴天气右缘
            const int bound_r = start_rc.left - gap - kStartPadDip * dpi_ / 96; // 常规留白界
            const int bound_soft = start_rc.left - gap - kMinPadDip * dpi_ / 96; // 贴靠软界
            // 用户水平偏移先行应用:可用宽必须按"偏移后的实际 x0"到两道界的距离判定,
            // 否则偏移右移会白白吃掉可用宽(实测 slot_offset_x_dip=32 时 580→516)
            int x0_off = cand_x0 + cfg_.slot_offset_x_dip * dpi_ / 96;
            const int x_lo = hr.left + gap;
            const int x_hi = bound_soft - min_avail; // 软界=硬限:x0 不得越过
            if (x0_off > x_hi) x0_off = x_hi;
            if (x0_off < x_lo) x0_off = x_lo;
            offset_applied = true;
            const int avail_reg = bound_r - x0_off;
            int cand_avail = avail_reg;
            // 自适应长度：常规界放不下，但贴靠软界（4DIP 贴开始按钮）容纳时放宽
            if (avail_reg < (int)std::ceil(total_full) &&
                bound_soft - x0_off >= (int)std::ceil(total_full)) {
                cand_avail = bound_soft - x0_off;
            }
            if (cand_avail >= min_avail) {
                x0 = x0_off;
                avail = cand_avail;
                right_lim = bound_r;
                offset_applied = true;
            }
        }
        if (x0 < 0) {
            RECT cluster{};
            if (prefer_cluster_left_ && ClusterRight(cluster)) {
                anchor_tag = 'R';
                const int cand_x0 = cluster.left + gap; // ClusterRight:out.left = 簇右缘
                const int cand_avail = (tray.left - gap) - cand_x0;
                if (cand_x0 >= hr.left && cand_avail >= min_avail) {
                    x0 = cand_x0;
                    avail = cand_avail;
                    right_lim = tray.left - gap;
                }
            }
        }
        if (x0 < 0) {
            anchor_tag = 'F';
            SlotLayout lay =
                ComputeSlotLayout(hr, tray, (int)std::ceil(total_full), rows_h, gap, min_avail);
            if (lay.placement == SlotPlacement::NoSpace) {
                if (final_pass) EnterDegrade(SlotDegrade::NoSpace);
                return;
            }
            x0 = lay.rc.left;
            avail = lay.avail_w;
            right_lim = lay.rc.right;
        }

        // 用户水平偏移(其余锚点分支):钳制在 [宿主左缘, 右界-min_avail]。
        // 簇左分支已在上面内联处理(需先于软界判定),此处跳过避免二次叠加。
        if (!offset_applied && right_lim > 0 && cfg_.slot_offset_x_dip != 0) {
            x0 += cfg_.slot_offset_x_dip * dpi_ / 96;
            const int x_hi = right_lim - min_avail;
            const int x_lo = hr.left + gap;
            if (x0 > x_hi) x0 = x_hi;
            if (x0 < x_lo) x0 = x_lo;
            avail = right_lim - x0;
        }
    };
    compute_anchor(false); // 第一遍:按收缩前内容宽落位(收缩比例据此估算)

    // 自适应长度：内容自然宽仍超可用 → 整体降字号一次（保住全部数字，而不是
    // 裁切 7天等尾列；仍超再由列核算收缩兜底）。
    if (x0 >= 0 && avail > min_avail && total_full > avail && fp > min_px) {
        int fp2 = (int)std::max(min_px, (int)(fp * avail / total_full));
        if (fp2 < fp) {
            fp = fp2;
            font_px_ = fp2;
            SetFont();
            content_dirty_ = true;
            rows_h = rows_wanted * line_h_;
            y_center = hr.top + (host_h - rows_h) / 2;
            remeasure();
        }
    }
    // 第二遍:字号收缩后内容变窄,重评锚定链——软界自适应按当前内容宽判定,
    // 常能放宽到贴靠软界,免去下面的列挤压与尾段截断(四平台并排的常态落点)。
    compute_anchor(true);
    if (x0 < 0) { // 两遍均无落位(NoSpace):降级
        SetVisible(false);
        return false;
    }

    // ---- 列核算(比例收缩 + 值段按列宽截断) ----
    float total = 0.0f;
    ComputeEmbedCols(name_w, value_w, (float)col_gap, (float)avail, col_w_, total);
    if (content_dirty_) {
        Log::WriteF(LogLevel::Info, "slot", "布局 fp=%d total_full=%.0f avail=%d total=%.0f "
                                             "x0=%d anchor=%c",
                    fp, (double)total_full, avail, (double)total, x0, anchor_tag);
    }
    if (col_w_.empty() || total < min_avail) {
        EnterDegrade(SlotDegrade::NoSpace);
        SetVisible(false);
        return false;
    }
    col_x_.clear();
    col_vals_.clear();
    {
        float cx = 0.0f;
        for (size_t i = 0; i < items_.size(); ++i) {
            col_x_.push_back(cx);
            std::vector<DisplaySegment> vals = items_[i].values;
            const float tight = col_w_[i]; // 预算=列宽本身(与 max 计算同口径,免微差误截)
            if (WidthOf(vals, seg_gap) > tight) {
                vals = TruncateSlotSegs(vals, tight, (float)seg_gap, L"…",
                                        &TaskbarSlot::MeasureValueCb, this);
            }
            col_vals_.push_back(std::move(vals));
            cx += col_w_[i] + (float)col_gap;
        }
    }

    // ---- 落位:窗口尺寸=内容尺寸(两行高),屏幕→父客户坐标 ----
    POINT org{};
    if (!ClientToScreen(host_, &org)) {
        EnterDegrade(SlotDegrade::NoHost);
        SetVisible(false);
        return false;
    }
    host_org_ = org;
    const int w = (int)std::ceil(total);
    const int x = x0 - org.x;
    const int y = y_center - org.y;
    if (!EnsureSwapChain(w, rows_h)) {
        EnterDegrade(SlotDegrade::RenderFail);
        SetVisible(false);
        return false;
    }
    if (!placed_once_ || x != last_x_ || y != last_y_ || w != last_w_ ||
        rows_h != last_h_) {
        SetWindowPos(wnd_, nullptr, x, y, w, rows_h,
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        last_x_ = x;
        last_y_ = y;
        last_w_ = w;
        last_h_ = rows_h;
        placed_once_ = true;
    }

    // ---- 绘制与显示(内容/尺寸/隐藏恢复才整帧重绘;静态期零提交) ----
    if (content_dirty_ || !visible_) {
        if (!DrawSwapchainFrame()) { // 失败已入 RenderFail 降级:不显示、不再清降级
            SetVisible(false);
            return false;
        }
    }
    content_dirty_ = false;
    ClearDegrade();
    SetVisible(true);
    return true;
}

bool TaskbarSlot::EnsureSwapChain(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (swapchain_) {
        DXGI_SWAP_CHAIN_DESC1 d{};
        if (SUCCEEDED(swapchain_->GetDesc1(&d)) && d.Width == (UINT)w && d.Height == (UINT)h)
            return true;
    }
    if (!dxgi_factory2_ || !dcomp_ || !visual_) return false;
    swapchain_.Reset();
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = (UINT)w;
    desc.Height = (UINT)h;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    // 首参镜像 TrafficMonitor 精确用法:传 D3D 设备本身(其源码即如此;查证二者皆可)
    HRESULT hr = dxgi_factory2_->CreateSwapChainForComposition(d3d_.Get(), &desc, nullptr,
                                                               &swapchain_);
    if (FAILED(hr)) {
        Log::WriteF(LogLevel::Error, "slot", "合成交换链创建失败 hr=0x%08lX",
                    (unsigned long)hr);
        return false;
    }
    visual_->SetContent(swapchain_.Get());
    dcomp_->Commit(); // SetContent 后必须提交一次,合成器才应用树变更
    content_dirty_ = true; // 新交换链需整帧绘制
    return true;
}

bool TaskbarSlot::DrawSwapchainFrame() {
    if (!swapchain_) return false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
    HRESULT hr = swapchain_->GetBuffer(0, IID_PPV_ARGS(&back));
    if (FAILED(hr) || !back) {
        Log::WriteF(LogLevel::Error, "slot", "交换链取缓冲失败 hr=0x%08lX", (unsigned long)hr);
        EnterDegrade(SlotDegrade::RenderFail);
        return false;
    }
    Microsoft::WRL::ComPtr<IDXGISurface> sf;
    hr = back.As(&sf);
    if (FAILED(hr)) {
        Log::WriteF(LogLevel::Error, "slot", "缓冲转 DXGI surface 失败 hr=0x%08lX",
                    (unsigned long)hr);
        EnterDegrade(SlotDegrade::RenderFail);
        return false;
    }
    // 位图包装(1.1 上下文)在本机链路 E_INVALIDARG(与 DComp surface 同),
    // 改用 v1 渲染目标直接绑定 DXGI surface 绘制(该路径 EndDraw 已验证可过)。
    D2D1_RENDER_TARGET_PROPERTIES rtp{};
    rtp.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
    rtp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rtp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    rtp.dpiX = 96.0f;
    rtp.dpiY = 96.0f;
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> rt;
    hr = d2d_factory_->CreateDxgiSurfaceRenderTarget(sf.Get(), &rtp, &rt);
    if (FAILED(hr) || !rt) {
        Log::WriteF(LogLevel::Error, "slot", "交换链渲染目标失败 hr=0x%08lX",
                    (unsigned long)hr);
        EnterDegrade(SlotDegrade::RenderFail);
        return false;
    }
    rt->BeginDraw();
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE); // 亚克力上禁 ClearType
    rt->Clear(D2D1_COLOR_F{0.0f, 0.0f, 0.0f, 0.0f});               // 全透明底

    // 两行布局:第一行 agent 名称(名称色),第二行额度段(各段保色),列内左对齐
    const int seg_gap = kSegGapDip * dpi_ / 96;
    const float line_h = (float)line_h_;
    // 原厂图标模式（与 RefreshGeometry 的测宽口径一致：边长=行高）
    const bool name_icons = cfg_.embed_brand_icons;
    const float icon_side = (float)line_h_;
    // 名称/值两把字号：值行小一档(0.8×),视觉层级也更清晰
    auto draw_seg_fmt = [&](const std::wstring& text, float px, float py, COLORREF color,
                            IDWriteTextFormat* fmt, float meas_w) {
        D2D1_COLOR_F col{GetRValue(color) / 255.0f, GetGValue(color) / 255.0f,
                         GetBValue(color) / 255.0f, 1.0f};
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        if (SUCCEEDED(rt->CreateSolidColorBrush(col, &brush))) {
            rt->DrawText(text.c_str(), (UINT32)text.size(), fmt,
                         D2D1_RECT_F{px, py, px + meas_w, py + line_h}, brush.Get(),
                         D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
    };
    for (size_t i = 0; i < items_.size() && i < col_x_.size(); ++i) {
        const float cx = col_x_[i];
        const float cw = col_w_[i];
        // 名称行(主字号)：列内水平居中（embed_show_names=false 时整行省略）
        if (cfg_.embed_show_names) {
            // 原厂图标模式：方芯片贴行高居中；图标未就绪回落文字
            UINT side_px = 0;
            const BYTE* pixels =
                name_icons ? ProviderIconPixels(items_[i].type, side_px) : nullptr;
            if (pixels && side_px > 0) {
                Microsoft::WRL::ComPtr<ID2D1Bitmap> bm;
                const D2D1_BITMAP_PROPERTIES props{
                    D2D1_PIXEL_FORMAT{DXGI_FORMAT_B8G8R8A8_UNORM,
                                      D2D1_ALPHA_MODE_PREMULTIPLIED},
                    96.0f, 96.0f};
                // 渲染目标每帧随交换链表面重建，位图随之逐帧创建（小图，开销可忽略）
                if (SUCCEEDED(rt->CreateBitmap(D2D1_SIZE_U{side_px, side_px}, pixels,
                                               side_px * 4, props, &bm))) {
                    const float x0 = cx + (cw - icon_side) * 0.5f;
                    rt->DrawBitmap(bm.Get(),
                                   D2D1_RECT_F{x0, 0.0f, x0 + icon_side, icon_side});
                } else {
                    const float nw = Measure(items_[i].name);
                    draw_seg_fmt(items_[i].name, cx + (cw - nw) * 0.5f, 0.0f,
                                 items_[i].name_color, fmt_.Get(), nw);
                }
            } else {
                const float nw = Measure(items_[i].name);
                draw_seg_fmt(items_[i].name, cx + (cw - nw) * 0.5f, 0.0f,
                             items_[i].name_color, fmt_.Get(), nw);
            }
        }
        // 值行(小字号，列内水平居中)；单行模式占满整窗（py 不再加行偏移）
        float vals_w = 0.0f;
        bool first_seg = true;
        for (const auto& s : col_vals_[i]) {
            if (!first_seg) vals_w += (float)seg_gap;
            vals_w += MeasureValue(s.text);
            first_seg = false;
        }
        // 单行模式（名称行关闭）：值行占满整窗，不再加第二行偏移
        const float vrow_y = cfg_.embed_show_names ? line_h * 1.08f : line_h * 0.08f;
        float vx = cx + (cw - vals_w) * 0.5f;
        for (const auto& s : col_vals_[i]) {
            const float w = MeasureValue(s.text);
            draw_seg_fmt(s.text, vx, vrow_y, s.color, fmt_value_.Get(), w);
            vx += w + (float)seg_gap;
        }
    }
    const HRESULT end_hr = rt->EndDraw();
    if (FAILED(end_hr)) {
        Log::WriteF(LogLevel::Error, "slot", "交换链 EndDraw 失败 hr=0x%08lX",
                    (unsigned long)end_hr);
        rt.Reset();
        EnterDegrade(SlotDegrade::RenderFail);
        return false;
    }
    rt.Reset();
    hr = swapchain_->Present(1, 0);
    if (FAILED(hr)) {
        Log::WriteF(LogLevel::Warn, "slot", "交换链 Present 失败 hr=0x%08lX",
                    (unsigned long)hr);
        EnterDegrade(SlotDegrade::RenderFail);
        return false;
    }
    dcomp_->Commit(); // 合成交换链帧也随 Commit 应用(幂等,仅当有未决变更)
    return true;
}

void TaskbarSlot::SetFont() {
    fmt_.Reset();
    fmt_value_.Reset();
    if (!dwf_ || font_px_ <= 0) return;
    if (FAILED(dwf_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      (float)font_px_, L"", &fmt_)))
        return;
    // 值行小一档(0.8×)：名称/值两行层级更清晰，也为内容自适应宽度腾出空间
    const int vpx = (int)std::max(6 * dpi_ / 96, font_px_ * 4 / 5);
    if (FAILED(dwf_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      (float)vpx, L"", &fmt_value_)))
        fmt_value_ = fmt_; // 失败退化为同字号(内容仍可显示)
    // 行高:按主字号样例布局的高度(DWrite 行盒)
    Microsoft::WRL::ComPtr<IDWriteTextLayout> probe;
    line_h_ = 0;
    if (SUCCEEDED(dwf_->CreateTextLayout(L"Ay", 2, fmt_.Get(), kLayoutMax, kLayoutMax,
                                         &probe))) {
        DWRITE_TEXT_METRICS m{};
        probe->GetMetrics(&m);
        line_h_ = (int)std::ceil(m.height);
    }
    if (line_h_ <= 0) line_h_ = (int)std::ceil(font_px_ * 1.3f); // 兜底估算
}

float TaskbarSlot::WidthOf(const std::vector<DisplaySegment>& v, int gap) {
    float w = 0.0f;
    bool first = true;
    for (const auto& s : v) {
        if (!first) w += (float)gap;
        w += MeasureValue(s.text); // 值行按小字号宽度核算
        first = false;
    }
    return w;
}

float TaskbarSlot::Measure(const std::wstring& text) {
    if (!fmt_ || text.empty() || !dwf_) return 0.0f;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> lay;
    if (FAILED(dwf_->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt_.Get(), kLayoutMax,
                                      kLayoutMax, &lay)))
        return 0.0f;
    DWRITE_TEXT_METRICS m{};
    lay->GetMetrics(&m);
    return m.width > 0 ? m.width : 0.0f;
}

float TaskbarSlot::MeasureValue(const std::wstring& text) {
    if (!fmt_value_ || text.empty() || !dwf_) return 0.0f;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> lay;
    if (FAILED(dwf_->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt_value_.Get(),
                                      kLayoutMax, kLayoutMax, &lay)))
        return 0.0f;
    DWRITE_TEXT_METRICS m{};
    lay->GetMetrics(&m);
    return m.width > 0 ? m.width : 0.0f;
}

float TaskbarSlot::MeasureCb(const std::wstring& text, void* ctx) {
    return ctx ? static_cast<TaskbarSlot*>(ctx)->Measure(text) : 0.0f;
}

float TaskbarSlot::MeasureValueCb(const std::wstring& text, void* ctx) {
    return ctx ? static_cast<TaskbarSlot*>(ctx)->MeasureValue(text) : 0.0f;
}

RECT TaskbarSlot::HostRect() const {
    RECT r{};
    if (host_ && IsWindow(host_)) GetWindowRect(host_, &r);
    return r;
}

RECT TaskbarSlot::MonitorRect() const {
    RECT r{0, 0, 0, 0};
    if (!host_ || !IsWindow(host_)) return r;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(MonitorFromWindow(host_, MONITOR_DEFAULTTONEAREST), &mi)) r = mi.rcMonitor;
    return r;
}

bool TaskbarSlot::ClusterRight(RECT& out) {
    // 左簇右缘 = 宿主内「开始/搜索/任务视图/小组件/任务栏按钮」承载窗口的最大右缘。
    // 硬约束:候选窗不得进入托盘区(递归枚举会命中嵌套的同类名窗口,须以托盘左缘为界,
    // 否则会取到右缘 3120 的全宽对象——实测踩过)。
    if (!host_ || !IsWindow(host_)) return false;
    RECT hr{};
    if (!GetWindowRect(host_, &hr)) return false;
    RECT tray{};
    bool est = false;
    const bool have_tray = TrayRect(tray, est); // 复用缓存/估算;仅取其左缘作界
    const int tray_l = have_tray ? tray.left
                                 : hr.left + (hr.right - hr.left) * 3 / 4; // 无锚点时宽松右界
    static const wchar_t* kClusterClasses[] = {L"MSTaskListWClass", L"MSTaskSwWClass",
                                               L"ReBarWindow32", L"Start"};
    struct Ctx {
        const wchar_t* const* classes;
        int class_count;
        const RECT* hr;
        int tray_l;
        int right;
    } ctx{kClusterClasses, 4, &hr, tray_l, -1};
    EnumChildWindows(host_, [](HWND h, LPARAM lp) -> BOOL {
        auto* c = (Ctx*)lp;
        wchar_t cls[64];
        if (GetClassNameW(h, cls, 64) == 0) return TRUE;
        bool match = false;
        for (int i = 0; i < c->class_count; ++i) {
            if (wcscmp(cls, c->classes[i]) == 0) { match = true; break; }
        }
        if (!match) return TRUE;
        RECT r{};
        if (!GetWindowRect(h, &r)) return TRUE;
        if (r.left < c->hr->left) return TRUE; // 越界左
        if (r.right > c->hr->right || r.right > c->tray_l) return TRUE; // 越界/入托盘
        if (r.right - r.left <= 0) return TRUE;
        if (r.right > c->right) c->right = r.right;
        return TRUE;
    }, (LPARAM)&ctx);
    if (ctx.right < hr.left) return false; // 静默回退到托盘锚(降级链覆盖)
    out = hr;
    out.left = ctx.right;
    return true;
}

bool TaskbarSlot::ClusterLeft(RECT& out) {
    // 开始按钮(Start)左缘:文字条右缘贴其左(天气组件之后的空闲带)。
    // 粘性锚(实测 2026-09-06,experiments/slot_probe/):开始菜单等 flyout 打开期间,
    // Win11 把左簇(含 Start HWND)从宿主子窗口树摘走换挂,EnumChildWindows 枚举不到,
    // 现场查找失败若直接返回 false,锚定链会回退簇右缘 → 嵌入条跳到任务栏右侧。
    // 因此:现场成功时刷新缓存(句柄+矩形);现场失败时句柄仍活用其实时矩形(位置仍
    // 被系统维护,最准),句柄已亡退缓存矩形兜底。所有矩形按宿主几何校验(比现场多
    // 一条垂直重叠),防宿主移动/换屏后钉死陈旧位置;全不过才回退锚定链。
    if (!host_ || !IsWindow(host_)) return false;
    RECT hr{};
    if (!GetWindowRect(host_, &hr)) return false;
    const int host_w = hr.right - hr.left;
    auto sane = [&](const RECT& r) {
        return r.left >= hr.left && r.left < hr.left + host_w / 2 &&
               r.right - r.left > 0 && r.right - r.left <= host_w / 2 &&
               r.bottom > hr.top && r.top < hr.bottom;
    };
    HWND start = FindChildByClass(host_, L"Start", kAnchorDepthMax);
    RECT r{};
    if (start && IsWindow(start) && GetWindowRect(start, &r) && sane(r)) {
        start_hwnd_ = start;
        start_cached_ = r;
        out = r;
        return true;
    }
    if (start_hwnd_ && IsWindow(start_hwnd_) && GetWindowRect(start_hwnd_, &r) &&
        sane(r)) {
        start_cached_ = r; // 句柄被摘挂但仍活:矩形继续保鲜
        out = r;
        return true;
    }
    if (sane(start_cached_)) { // 零值矩形过不了 sane(w=0),无需额外判空
        out = start_cached_;
        return true;
    }
    return false;
}

bool TaskbarSlot::TrayRect(RECT& out, bool& estimated) {
    estimated = false;
    if (host_ && IsWindow(host_)) {
        if (tray_hwnd_ && IsWindow(tray_hwnd_)) {
            wchar_t cls[64];
            if (GetClassNameW(tray_hwnd_, cls, 64) != 0 &&
                wcscmp(cls, kTrayNotifyClass) == 0 && GetWindowRect(tray_hwnd_, &out))
                return true;
        }
        tray_hwnd_ = FindChildByClass(host_, kTrayNotifyClass, 0);
        if (tray_hwnd_ && GetWindowRect(tray_hwnd_, &out)) return true;
        tray_hwnd_ = nullptr;
    }
    estimated = true;
    return false;
}

RECT TaskbarSlot::EstimateTrayRect() const {
    const RECT hr = HostRect();
    const int est_w = kTrayEstWidthDip * dpi_ / 96;
    const int left = hr.right - est_w;
    return {left > hr.left ? left : hr.left, hr.top, hr.right, hr.bottom};
}

void TaskbarSlot::SetVisible(bool v) {
    if (!wnd_ || visible_ == v) return;
    visible_ = v;
    ShowWindow(wnd_, v ? SW_SHOW : SW_HIDE);
}

void TaskbarSlot::ResetComposition() {
    swapchain_.Reset(); // 视觉内容先于视觉/目标释放
    visual_.Reset();
    target_.Reset();
    content_dirty_ = true; // 重建后需重绘
}

void TaskbarSlot::ReleaseAll() {
    ResetComposition(); // 绑定子窗的合成对象先于设备释放
    fmt_.Reset();
    dwf_.Reset();
    d2d_factory_.Reset();
    dxgi_factory2_.Reset();
    target_.Reset();
    visual_.Reset();
    desktop_.Reset();
    dcomp_.Reset();
    dxgi_dev_.Reset();
    d3d_.Reset();
}

void TaskbarSlot::EnterDegrade(SlotDegrade d) {
    if (degrade_ == d) return;
    degrade_ = d;
    Log::WriteF(LogLevel::Warn, "slot", "内嵌降级: %s", SlotDegradeText(d));
    if (status_cb_) status_cb_(true, SlotDegradeText(d));
}

void TaskbarSlot::ClearDegrade() {
    if (degrade_ == SlotDegrade::None) return;
    Log::WriteF(LogLevel::Info, "slot", "内嵌恢复(退出降级态)");
    degrade_ = SlotDegrade::None;
    if (status_cb_) status_cb_(false, nullptr);
}

} // namespace qb
