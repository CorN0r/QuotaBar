// disp_layout.hpp — 显示设置页布局计算器（纯函数，selftest 直接覆盖）。
// 2026-09-18 从 tray_ui.cpp ComputeDisp 抽出（继 ui_layout/quota_layout 后第三个
// 纯计算模块）：显示页所有矩形的单一来源（"Compute* 单一来源"契约）。
// tray_ui 侧保留同名薄包装（从 g_draft.cfg + g_dscroll 快照输入）。
// 2026-09-21 模式合并：5 模式 → 4（贴靠并入浮条吸附）；外观增加低额变色/品牌色/
// 嵌入内容行；位置区按模式三形态（嵌入滑杆 / 浮条吸附+XY / 圆环吸附偏移）。
#pragma once
#include <windows.h>
#include "config.hpp" // DisplayMode

namespace qb::ui {

// 显示设置页矩形集（macOS 系统设置式分组卡片；行高 DIP：模式行 54/普通行 48）
struct DispLayout {
    RECT sec[5];   // 小节标题：显示模式/外观/位置微调/刷新与提醒/启动与数据
    RECT card[5];  // 分区卡片
    RECT mode[4];                 // 模式单选行（嵌入/浮条/仅托盘/圆环）
    RECT theme_label, theme_seg;  // 主题三段（跟随系统/深色/浅色）
    RECT embed_note;              // 嵌入模式说明行（排版/字号不适用）
    RECT layout_label, seg_d, seg_s;      // 排版布局（非嵌入）
    RECT font_label, font_dec, font_inc, font_val;  // 字体字号步进（非嵌入）
    RECT color_label, color_seg;          // 字色自适应三段（全模式）
    RECT lowc_label, lowc_toggle;         // 低额度变色开关（全模式）
    RECT brand_label, brand_toggle;       // 账户名品牌色开关（全模式）
    RECT embicon_label, embicon_toggle;   // 原厂图标替代名称/徽章（全模式）
    RECT embname_label, embname_toggle;   // 嵌入条·显示名称行（仅嵌入）
    RECT embwin_label, embwin_toggle;     // 嵌入条·窗口标签（仅嵌入）
    bool embed = false;
    // 位置微调区（pos_visible=嵌入/浮条/圆环；仅托盘整区隐藏）
    bool pos_visible = false;
    bool free = false;            // 浮条模式（吸附开关 + X/Y 滑杆）
    bool rings = false;           // 圆环模式（吸附水平偏移滑杆）
    bool snap = false;            // 浮条吸附开（隐藏 Y 行，X 行变吸附偏移）
    RECT big_val, b_zero;         // 嵌入：偏移行（标签+数值+置零）
    RECT slider, kbd_line;        // 嵌入：滑杆行 + 快捷键提示行
    RECT snap_row, snap_toggle;   // 浮条：吸附任务栏开关行
    RECT x_label, x_val, x_slider;   // 水平行（自由位置 / 吸附偏移）+ 滑杆行
    RECT y_label, y_val, y_slider;   // 垂直行（仅浮条且未吸附）
    RECT pos_hint;                   // 浮条拖动提示行
    RECT int_label, int_edit, int_slider;
    RECT row_eco, eco_toggle;
    RECT row_notify, notify_toggle;
    bool thr_visible = false;
    RECT thr_k, thr_k_edit, thr_d, thr_d_edit;
    RECT row_auto, auto_toggle;
    RECT row_clear, clear_btn;
    int content_bottom = 0;
    int viewport_bottom = 0;
};

// 输入快照（由 tray_ui 从 g_draft.cfg 构造）
struct DispPageIn {
    DisplayMode mode = DisplayMode::Embed;
    bool notify_enabled = false;  // 预警开→阈值两行可见
    bool snap = false;            // 浮条吸附开（隐藏 Y 行）
    int scroll = 0;               // 当前滚动（钳制后经 scroll_out 写回）
};

// 显示设置页布局（矩形=客户区物理像素，DIP 常量 × dpi/96）。
// 设置列限宽 640 DIP 居中；滚动钳制 scroll_out = clamp(scroll, 0, content_bottom+12dip-视口底)。
DispLayout ComputeDispLayout(const DispPageIn& in, const RECT& content, int dpi,
                             int& scroll_out);

} // namespace qb::ui
