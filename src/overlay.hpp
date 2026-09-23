// overlay.hpp — 透明文字小窗（§10）：WS_POPUP 分层窗 + 预乘 Alpha 位图 + 点击穿透。
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "config.hpp"
#include "state.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace qb {

// 浮条拖放落位判定（纯函数，selftest 可测）：根据松手时窗口矩形与主屏工作区/任务栏
// 矩形，决定落点为「吸附任务栏」还是「自由位置」，并产出应持久化的 DIP 字段。
// 吸附条件：任务栏在底部、窗口底缘距任务栏上缘 ≤ 32 DIP、且与任务栏水平有交集。
struct BarDrop {
    bool snap = false;
    int free_pos_x_dip = 0, free_pos_y_dip = 0; // snap=false 时有效（工作区 DIP 坐标）
    int snap_x_dip = -40, snap_y_dip = 0;       // snap=true 时有效（右缘偏移/上缘间隙）
};
BarDrop ResolveBarDrop(const RECT& win, const RECT& work, const RECT& monitor,
                       const RECT& taskbar, int dpi);

class Overlay {
public:
    // 拖放落位回调（松手即存 → 携带应持久化的 DisplayConfig）
    using AdjustEndFn = std::function<void(bool save, const std::optional<DisplayConfig>& next)>;
    void SetAdjustEndCallback(AdjustEndFn fn) { adjust_cb_ = std::move(fn); }

    Overlay();                // 构造/析构在 overlay.cpp 定义
    ~Overlay();               // （gdiplus_ 需完整 GdiPlusToken，避免头内联展开）
    bool Create(HINSTANCE hinst, HWND msg_wnd);
    void Destroy();

    // 数据/配置变化：仅变化时重绘
    void Update(const DisplayModel& model, const DisplayConfig& cfg);
    // 圆环模式（DisplayMode::Rings）：语义同 Update
    void UpdateRings(const std::vector<RingItem>& items, const DisplayConfig& cfg,
                     bool test_label);
    // 显式重新定位/重建位图（DPI、主题、分辨率、主屏、Explorer 重启）
    void Relayout();
    void OnDpiChanged(int dpi);
    void OnTaskbarCreated();       // Explorer 重启：重建 Z 序并重新定位
    void OnDisplayChange();
    void OnFullscreenCheck();      // 前台/位置变化事件驱动：全屏则隐藏

    // 圆环弧值动画步进（WM_TIMER 内部驱动；公开仅为窗口过程可调用）
    void TickRingAnimation();

    // Ctrl+拖动直接移动（2026-09-21 取代"进入调整/双击保存"仪式流）：
    // 轮询计时器检测 Ctrl+悬停 → 临时取消点击穿透；按下拖动，松手即存（近任务栏自动吸附）。
    // 以下方法由窗口过程调用（公开仅为 OverlayProc 可达）。
    void TickGrab();                 // 轮询：Ctrl+悬停判定 → 抓取态切换
    bool BeginDrag(LPARAM lparam);   // WM_LBUTTONDOWN：起拖
    void DragMove(LPARAM lparam);    // WM_MOUSEMOVE：跟手移动
    bool EndDrag();                  // WM_LBUTTONUP：松手落位并持久化（返回是否处理）
    void OnCaptureLost();            // WM_CAPTURECHANGED：拖动中断复位
    bool IsDragging() const { return dragging_; }
    bool IsGrabbable() const { return grabbable_; }

    void Show();
    void Hide();
    bool IsVisible() const { return visible_; }
    // 嵌入模式抑制(display.mode=Embed)：浮窗整体不上屏，事件驱动的重定位也不得唤起。
    // 关闭抑制不自动显示，由下一次 Update/UpdateRings 决定。
    void SetSuppressed(bool on);

    // 测试钩子：无窗口环境下测量文本（selftest 用）
    static std::pair<int, int> MeasureModel(const DisplayModel& model, int font_px, int dpi);

    HWND hwnd() const { return hwnd_; }

private:
    void Paint();                  // 生成位图并 UpdateLayeredWindow（圆环模式路由到 PaintRings）
    void PaintRings();             // 圆环模式绘制
    void SetupRingAnimation(const std::vector<RingItem>& items); // 弧值变化→启动从当前值到新值的过渡
    void ApplyPosition();
    void ApplyWindowStyle();       // 穿透/边框样式
    RECT TaskbarRect();            // 主任务栏几何（兼容模块：失败则用工作区）
    int Scaled(int dip) const;

    HWND hwnd_ = nullptr;
    HWND msg_wnd_ = nullptr;
    HINSTANCE inst_ = nullptr;
    DisplayModel model_;
    std::vector<RingItem> rings_;
    std::vector<float> ring_disp_; // 各环弧的当前显示值（动画从呈现值出发，不跳变）
    std::vector<float> ring_from_; // 当前过渡的起点值
    DWORD ring_anim_tick_ = 0;     // 过渡起始时刻（GetTickCount）
    bool ring_animating_ = false;
    bool test_label_ = false;      // 圆环模式 [测试] 角标
    DisplayConfig cfg_;
    bool visible_ = false;
    bool suppressed_ = false;      // 嵌入模式抑制：一切路径不得点亮浮窗
    bool grabbable_ = false;       // Ctrl+悬停：临时可抓取（穿透已取消、虚线提示）
    bool dragging_ = false;        // 拖动中（SetCapture 跟手）
    POINT grab_off_{};             // 按下点相对窗口左上角偏移（拖动保持抓取点）
    AdjustEndFn adjust_cb_;        // 松手落位通知（持久化由宿主完成）
    POINT saved_pos_{};            // 当前上屏位置（PresentSurface_ 复用）
    int dpi_ = 96;
    SIZE size_{};                  // 当前物理像素尺寸
    bool has_bitmap_ = false;

    struct GdiPlusToken;
    std::unique_ptr<GdiPlusToken> gdiplus_;

    // ULW 提交（Paint/PaintRings 共用）：成功才置 has_bitmap_（失败保持旧值，
    // 下一次 Update 因 !has_bitmap_ 强制重画自愈）；memdc 上的绘制须已完成
    bool PresentSurface_(HDC memdc);
};

} // namespace qb
