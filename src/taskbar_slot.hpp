// taskbar_slot.hpp — 任务栏内嵌槽（display.mode=Embed）：把单行额度文字父化嵌入
// Shell_TrayWnd 的空闲槽位（托盘图标区左/右侧），纯展示、全程点击穿透，交互走托盘。
//
// 职责与边界（设计文档 §10.2）：Shell_TrayWnd / TrayNotifyWnd 等系统窗口类识别
// 全部隔离在本模块（兼容封装），识别失败立即降级，不持续枚举整个 UI 树。
// 渲染路径（CLAUDE.md「已知状态」裁定）：不依赖父化分层窗口（ULW/色键本机实测不合成），
// 生产唯一路径为 D2D1 + DirectWrite + DirectComposition；落地前必须实机肉眼验收。
// 坐标纪律（探针实测）：父化窗口 SetWindowPos 按父客户区坐标、GetWindowRect 按屏幕坐标，
// 混用会每 tick 漂移一个宿主原点（+1984px/tick）——本模块几何统一用屏幕坐标决策，
// 仅落地时换算父客户坐标，且只在变化时 SetWindowPos。
//
// 全部方法仅在 UI 线程调用；测试只覆盖文件头部的纯函数（不实例化窗口对象）。
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// ComPtr 成员直接持有 COM 接口：必须在此引入完整 SDK 声明（含 __declspec(uuid)）。
// 注意：手动前置声明这些接口会破坏 __uuidof（IID 变空，DComp/D2D 调用返回
// E_INVALIDARG/E_NOINTERFACE）——实测踩过，勿改回。
#include <d2d1.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include "config.hpp"
#include "state.hpp"
#include <functional>
#include <string>
#include <vector>

namespace qb {

// ---------- 纯逻辑（无窗口、无 DComp，可直接入 selftest） ----------

enum class SlotPlacement { NoSpace, LeftOfTray, RightOfTray };

// 文本段测量回调（实现方注入；测试用假宽度）
using SlotMeasureFn = float (*)(const std::wstring& text, void* ctx);

struct SlotLayout {
    SlotPlacement placement = SlotPlacement::NoSpace;
    RECT rc{};        // 期望窗口屏幕矩形（仅 placement != NoSpace 时有意义）
    int avail_w = 0;  // 放置侧可用宽度（内容裁切上限）
};

// 水平任务栏的槽位决策（物理 px；宿主/托盘均为屏幕矩形）：
//  垂直任务栏（host 高>宽）、内容高于宿主、宿主尺寸非法 → NoSpace。
//  优先托盘左侧（右缘贴 tray.left - gap）；左空间不足再试托盘右侧；
//  两侧均放不下时选可用宽较大的一侧落位（rc 宽=可用宽），由调用方按 avail_w
//  裁切内容后再次调用落定精确位置。
SlotLayout ComputeSlotLayout(const RECT& host_rc, const RECT& tray_rc,
                             int content_w, int content_h, int gap_px, int min_avail_px);

// 宿主可见性（任务栏自动隐藏/移出屏幕时）：与所在监视器相交面积 ≥ 一半视为可见
bool HostVisible(const RECT& host_rc, const RECT& monitor_rc);

// 内嵌字色：0=自动（浅色主题任务栏→黑字，深色→白字）、1=白、2=黑。
// 注意与浮窗 overlay 的自动档相反：浮窗底色是壁纸，任务栏底色是主题表面。
bool ResolveSlotDarkText(int text_color_mode, bool apps_light_theme);

// 字号约束：返回 min(want_px, host_h - 2×上下边距)；宿主装不下最小字号返回 0
int ClampSlotFontPx(int want_px, int host_h_px, int min_px, int v_margin_px);

// 单行多色段超宽裁切（物理 px 宽度；gap_w 为段间间隙，省略号计入宽度）：
//  全部放得下 → 原样返回。放不下 → 从头保留尽量多的完整段，对下一段逐字符
//  截短并追加省略号；连省略号也放不下则只保留完整前缀段。全放不下 → 空。
std::vector<DisplaySegment> TruncateSlotSegs(const std::vector<DisplaySegment>& in,
                                             float max_w, float gap_w,
                                             const std::wstring& ellipsis,
                                             SlotMeasureFn measure, void* ctx);

// 降级原因（进入降级只记一次/每 episode；气泡文案由宿主拼装）
enum class SlotDegrade { None, NoHost, NotPrimary, NoSpace, RenderFail };
const char* SlotDegradeText(SlotDegrade d); // UTF-8 短语（日志/提示）

// 两行嵌入列布局（纯函数，物理 px）：每列宽 = max(名称宽, 值宽)，列间 gap；
// 自然总宽超 avail 时按比例收缩（不下划小于 min）；输出 col_w 与总宽 total。
void ComputeEmbedCols(const std::vector<float>& name_w, const std::vector<float>& value_w,
                      float gap, float avail, std::vector<float>& col_w, float& total);

// ---------- 窗口对象（仅 UI 线程） ----------

class TaskbarSlot {
public:
    // 降级进入（degraded=true，每 episode 一次）或恢复（false）回调
    using StatusFn = std::function<void(bool degraded, const char* why)>;

    TaskbarSlot();
    ~TaskbarSlot();
    bool Create(HINSTANCE hinst);   // 注册窗口类；设备懒建于首次 attach
    void Destroy();                 // 退出清理：隐藏/销毁子窗、释放全部合成对象

    // 数据/配置变化（app 仅在 Embed 模式调用；内部置 active 并尝试呈现）。
    // 两行布局条目：第一行 agent 名称、第二行额度（列对齐，见 BuildEmbedItems）。
    void Update(const std::vector<EmbedRowItem>& items, const DisplayConfig& cfg);
    void OnTaskbarCreated();        // Explorer 重启：整窗重建（宿主销毁连带销毁子窗）
    void OnDisplayChange();         // WM_DISPLAYCHANGE：分辨率/DPI/任务栏跨屏恢复
    void Poll();                    // 1s 轮询（Embed 期由 app 定时器驱动）
    void Deactivate();              // 切出 Embed：隐藏停窗，保留对象供再入
    void SetStatusCallback(StatusFn fn) { status_cb_ = std::move(fn); }

    bool active() const { return active_; }
    HWND hwnd() const { return wnd_; }

private:
    bool PresentIfActive();    // 状态机主入口：attach/几何/绘制/降级（Update/Poll 共用）
    bool TryAttachHost();      // 宿主发现与主屏校验（受降级门控）
    bool EnsureWindow();       // 子窗创建（失效重建）
    bool EnsureRender();       // D3D/D2D/DWrite/DComp 设备与合成树懒建
    bool RefreshGeometry();    // 内容段/裁切/槽位计算与落位；失败返回 false
    bool EnsureSwapChain(int w, int h); // 合成交换链尺寸不匹配时重建（尺寸固定语义）
    bool DrawSwapchainFrame(); // 段文本 → 合成交换链 → Present；失败入 RenderFail
    void SetVisible(bool v);   // ShowWindow(SW_SHOW/SW_HIDE)，变化才动
    void SetFont();            // fmt_/fmt_value_/line_h_ 按当前 dpi/字号重建
    float WidthOf(const std::vector<DisplaySegment>& v, int gap); // 段总宽（含间隙）
    float Measure(const std::wstring& text); // 段文本宽度（物理 px,名称字体）
    float MeasureValue(const std::wstring& text); // 值行宽度（值字体,小一档）
    static float MeasureCb(const std::wstring& text, void* ctx);
    static float MeasureValueCb(const std::wstring& text, void* ctx);
    RECT HostRect() const;
    RECT MonitorRect() const;
    RECT EstimateTrayRect() const; // 无锚点时的右端估算
    bool TrayRect(RECT& out, bool& estimated); // 锚点矩形；估算时置位
    // 锚定链(首选项用户实机拍板 2026-09-05):
    //   首选 ClusterLeft = 开始按钮左缘(贴天气与开始之间);失败/无空间 → ClusterRight
    //   簇右缘(任务栏按钮之后);仍无 → 托盘锚。
    bool ClusterRight(RECT& out); // 左簇(开始/天气/任务栏按钮)右缘;未找到返回 false
    bool ClusterLeft(RECT& out);  // 开始按钮(Start)左缘;现场枚举失败时用粘性锚兜底
    void ResetComposition();   // 释放绑定子窗的 target/visual/surface
    void ReleaseAll();         // 释放全部（含设备）
    void EnterDegrade(SlotDegrade d); // 转移记录 + 日志 + 回调（每 episode 一次）
    void ClearDegrade();       // 恢复（日志 + 回调）
    void HideWnd() { SetVisible(false); }

    static LRESULT CALLBACK SlotProc(HWND h, UINT m, WPARAM w, LPARAM l);

    // 状态
    HINSTANCE inst_ = nullptr;
    HWND host_ = nullptr;
    HWND wnd_ = nullptr;
    bool active_ = false;       // Embed 模式有效（app 分发）
    bool exiting_ = false;
    bool visible_ = false;      // 子窗当前显示
    bool content_dirty_ = true; // 内容/字号变化待重绘
    SlotDegrade degrade_ = SlotDegrade::None;
    std::vector<EmbedRowItem> items_;   // 两行条目（名称行/值行）
    DisplayConfig cfg_;
    int dpi_ = 96;
    int font_px_ = 0;           // 实际使用的字号（clamp 后，物理 px）
    int line_h_ = 0;            // 单行文本行高（物理 px；窗口高 = 2×line_h_）
    std::vector<float> col_x_, col_w_;                    // 每列起点/宽（surface 坐标）
    std::vector<std::vector<DisplaySegment>> col_vals_;  // 每列已裁切值段
    int last_x_ = 0, last_y_ = 0, last_w_ = 0, last_h_ = 0; // 最近应用的落位（去重）
    bool placed_once_ = false;  // 已落位过（窗口重建后强制再落位）
    POINT host_org_{};          // 宿主原点（屏幕→父客户换算，应用前重读）
    HWND tray_hwnd_ = nullptr;  // TrayNotifyWnd 锚点句柄（缓存 + 类名校验）
    bool est_tray_logged_ = false; // 估算锚点只记一次日志
    // 左簇锚点（开始/小组件/任务栏按钮之后放置，用户首选;次选贴托盘）
    bool prefer_cluster_left_ = true;
    // 粘性开始按钮锚(实测 2026-09-06,experiments/slot_probe/):开始菜单等 flyout
    // 打开期间 Win11 把左簇(含 Start HWND)从宿主子窗口树摘走换挂,EnumChildWindows
    // 枚举不到 → 现场查找失败顺链回退簇右缘,嵌入条跳到任务栏右侧。缓存句柄+矩形,
    // 现场失败时兜底保持不动;宿主重建/更换时清空。
    HWND start_hwnd_ = nullptr; // 最近一次现场成功找到的开始按钮句柄
    RECT start_cached_{};       // 对应矩形(句柄已亡时的兜底;零值=无缓存)
    StatusFn status_cb_;

    // 渲染（ComPtr 成员）。实机验证组合(2026-09-05,详见 taskbar_slot.cpp 顶部结论):
    //   窗口 = 顶层 WS_POPUP 创建后 SetParent(出生即子窗不合成);
    //   内容 = 合成交换链(CreateSwapChainForComposition,首参 D3D 设备);
    //   绘制 = D2D1 v1 CreateDxgiSurfaceRenderTarget 直绑交换链后缓冲;
    //   DComp = Device2/桌面设备直求(Device2 直求 E_NOINTERFACE)
    Microsoft::WRL::ComPtr<ID3D11Device> d3d_;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_dev_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwf_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_;       // 名称行（主字号）
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_value_; // 值行（0.8×,两行层级/自适应宽度）
    Microsoft::WRL::ComPtr<ID2D1Factory> d2d_factory_;
    Microsoft::WRL::ComPtr<IDXGIFactory2> dxgi_factory2_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapchain_;
    Microsoft::WRL::ComPtr<IDCompositionDesktopDevice> desktop_;
    Microsoft::WRL::ComPtr<IDCompositionDevice2> dcomp_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> target_;
    Microsoft::WRL::ComPtr<IDCompositionVisual2> visual_;
};

} // namespace qb
