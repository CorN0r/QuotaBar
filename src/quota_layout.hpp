// quota_layout.hpp — 额度页布局计算器（纯函数，selftest 直接覆盖）。
// 2026-09-18 从 tray_ui.cpp ComputeQuota 抽出：该函数是额度页所有矩形的单一来源
// （CLAUDE.md "Compute* 单一来源"契约），历史四次 RC() 炸版/行高错位事故都在此族。
// 抽为无 App/全局依赖的纯函数后可先行测试，为 tray_ui 拆分（quota_page 模块）打底。
// tray_ui 侧保留同名薄包装（从 App+g_q 快照输入），调用点零改动。
#pragma once
#include <windows.h>
#include <vector>

namespace qb::ui {

// 「添加卡片」面板的类型 chip 数（kAddTypes 同长,定义在 quota_page.cpp）
inline constexpr int kAddTypeCount = 5;

// 卡片内控件矩形（含滚动偏移后的最终客户区坐标）
struct QuotaCardR {
    RECT toggle{0, 0, 0, 0};        // 启用开关
    RECT eye{0, 0, 0, 0};           // 显隐开关
    RECT note{0, 0, 0, 0};          // ⓘ 命中区（paint 按文本宽回填）
    RECT stats{0, 0, 0, 0};         // 核心数值区（内嵌面板：圆环/余额）
    RECT settings_btn{0, 0, 0, 0};  // 卡片带内「设置」按钮
    RECT ex_alias{0, 0, 0, 0};      // 展开区：别名输入框
    RECT ex_key{0, 0, 0, 0};        // 展开区：Key 输入框
    RECT ex_alert{0, 0, 0, 0};      // 展开区：低额预警阈值按钮（Kimi/GLM 卡）
    RECT ex_key_eye{0, 0, 0, 0}, ex_key_copy{0, 0, 0, 0}, ex_key_clear{0, 0, 0, 0};
    RECT ex_codex_sync{0, 0, 0, 0}; // 无 Key 类「重新读取」（凭证行右端,卡内右对齐;其余卡为空矩形）
    RECT ex_save{0, 0, 0, 0}, ex_delete{0, 0, 0, 0};
    RECT trend{0, 0, 0, 0};         // 趋势带（标题行 + 图表面板）
    int expand_h = 0;               // 展开区高（px）
};

// 额度页整页矩形集（卡片网格 2 列；行高=两卡较大值，顶对齐）
struct QuotaLayout {
    RECT ready_chip{0, 0, 0, 0};
    RECT add_btn{0, 0, 0, 0};
    RECT refresh_btn{0, 0, 0, 0};   // 页头刷新（原标题栏「刷新」移入本页页头）
    RECT hidden_chip{0, 0, 0, 0};
    bool has_hidden_chip = false;
    RECT stats_block{0, 0, 0, 0};   // 右上统计文本块（右对齐两行；有隐藏 chip 时让出其
                                    // 左界——chip 后画，重叠会盖住「N 项服务正常计费中」）
    RECT add_panel{0, 0, 0, 0};     // 添加面板（展开时有效）
    RECT ap_type[kAddTypeCount]{};
    RECT ap_alias{0, 0, 0, 0}, ap_key{0, 0, 0, 0};
    RECT ap_key_eye{0, 0, 0, 0}, ap_key_copy{0, 0, 0, 0}, ap_key_clear{0, 0, 0, 0};
    RECT ap_error{0, 0, 0, 0};
    RECT ap_create{0, 0, 0, 0}, ap_cancel{0, 0, 0, 0};
    RECT ap_console{0, 0, 0, 0};
    RECT hidden_box{0, 0, 0, 0};
    std::vector<int> hidden_idx;    // 隐藏账户在全列表的序（序=hidden_rows 序）
    std::vector<RECT> hidden_rows;
    std::vector<int> vis_idx;       // 可见账户在全列表的序（序=卡片序）
    std::vector<RECT> cards;
    std::vector<QuotaCardR> cr;
    int content_bottom = 0;         // 内容流底部（滚动前坐标）
    int viewport_bottom = 0;
    bool empty_state = false;       // 无任何账户
};

// 输入快照：由 tray_ui 包装层从 App 账户状态 + g_q 会话态构造
struct QuotaCardIn {
    bool visible = true;        // card_visible
    bool has_trend = false;     // App::AccountHasTrendFor（趋势带占位与 paint 同判定）
    bool expanded = false;      // 额度页展开设置区
    bool alert_capable = false; // 比例窗口类（Kimi/GLM/Antigravity：阈值行仅这几类卡有）
    bool uses_api_key = true;   // false=无 Key 类:凭证行右端给「重新读取」按钮让位（状态框收窄）
};
struct QuotaPageIn {
    bool add_open = false;      // 「添加卡片」面板展开
    bool hidden_open = false;   // 已隐藏卡片区展开
    int scroll = 0;             // 当前滚动（函数内钳制后经 scroll_out 写回）
    std::vector<QuotaCardIn> cards;
};

// 额度页布局（矩形=客户区物理像素，DIP 常量 × dpi/96；含计算端点一律 brace-init 的
// RC 纪律在此延续）。滚动钳制：scroll_out = clamp(scroll, 0, content_bottom+8dip-视口底)。
QuotaLayout ComputeQuotaLayout(const QuotaPageIn& in, const RECT& content, int dpi,
                               int& scroll_out);

} // namespace qb::ui
