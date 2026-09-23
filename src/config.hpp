// config.hpp — 设置项与默认值。config.json 不含任何密钥。
// 2026-09-14 起账户模型为动态列表：每个 coding plan 账户 = 一张卡片
//（类型 + 别名 + 启用/显隐 + 凭证版本），不再按平台固定 4 槽。
#pragma once
#include <string>
#include <vector>

namespace qb {

// JSON display.mode 键值（数值是落盘契约，勿重排）：
//   0=Docked(旧"贴靠任务栏外侧"，2026-09-21 并入 Free：读取时迁移为 Free+bar_snap)
//   1=Free(浮条) 2=TrayOnly 3=Embed(任务栏内嵌) 4=Rings(圆环)
// 枚举保留 Docked 名仅为迁移判定与 JSON 契约文档，UI/运行时不再产生该值。
enum class DisplayMode { Docked, Free, TrayOnly, Embed, Rings };

struct DisplayConfig {
    DisplayMode mode = DisplayMode::Embed; // 默认=推荐项「嵌入任务栏」
    int free_pos_x_dip = 140;       // 浮条自由位置（DIP，主屏工作区坐标）
    int free_pos_y_dip = 60;
    bool bar_snap = false;          // 浮条吸附任务栏上缘右端（旧 Docked 行为；拖近自动吸附）
    int bar_snap_x_dip = -40;       // 吸附水平偏移（DIP，相对主屏右缘；圆环模式同用）
    int bar_snap_y_dip = 0;         // 吸附垂直间隙（DIP，≥0，任务栏上缘向上）
    int font_size_pt = 9;
    int text_color_mode = 0;        // 0=自动 1=白 2=黑
    int slot_offset_x_dip = 0;      // 嵌入条水平偏移(DIP,向右为正;0=默认紧贴天气估算右缘)
    bool two_lines = true;
    bool low_quota_color = true;    // 低额度值段变色（琥珀=≤阈值，红=耗尽）；状态告警色不受影响
    bool brand_color_names = false; // 账户名/别名用平台品牌色（值段仍走语义色）
    bool embed_show_names = true;   // 嵌入条名称行（关=单行只显示值，省一半高度）
    bool embed_show_windows = true; // 嵌入条窗口标签（"5h余"/"周余"；关=只显示数值）
    bool embed_brand_icons = false; // 原厂图标：嵌入条名称行与卡片/圆环徽章改用厂商图标（回落文字）
};

struct QueryConfig {
    int interval_sec = 300;         // 60..3600
    bool power_save = false;        // true → 900s
    bool pause = false;
    bool autostart = false;
};

struct NotifyConfig {
    bool enabled = false;           // 默认关闭
    bool reset_reminder = true;     // 额度窗口重置恢复提醒（enabled 开启时生效）
    int kimi_threshold_pct = 20;    // 全局默认：百分比窗口（Kimi/GLM）剩余 ≤ 该值提醒
    double deepseek_threshold_cny = 5.0; // 金额账户（DeepSeek 等）余额 ≤ 该值提醒
};

// 账户类型（决定请求适配器与解析器；同类型可多账户，codex 除外——见 AccountConfig）
enum class AccountType { Kimi, Codex, Glm, Deepseek };

struct AccountConfig {
    std::string id;              // 稳定路由键（cache/secrets/调度）；新建 = "acc<N>"，codex 固定 "codex"
    AccountType type = AccountType::Kimi;
    std::string alias;           // 显示别名（"某某某的 codex"）；空 = 平台默认名
    bool enabled = true;         // 启用：参与查询 + 常驻条/圆环显示；停用后卡片置灰
    bool card_visible = true;    // 卡片是否显示在额度详情页（隐藏后从「已隐藏」区找回）
    int64_t cred_rev = 0;        // 凭证版本（换 Key/删除递增，隔离旧结果）；codex 恒 1
    int alert_pct = 0;           // 低额预警阈值（百分比账户）：0=继承全局 1=关闭 5..95=自定义
};

struct AppConfig {
    int config_version = 2;
    DisplayConfig display;
    QueryConfig query;
    NotifyConfig notify;
    bool first_run_done = false;
    int ui_theme = 0;               // 主窗口主题:0=跟随系统 1=深色 2=浅色(F1 双主题的手动覆盖)
    std::vector<AccountConfig> accounts;
    int64_t account_seq = 0;        // 新账户 id 自增计数（保证 id 不复用）

    const AccountConfig* FindAccount(const std::string& id) const {
        for (const auto& a : accounts)
            if (a.id == id) return &a;
        return nullptr;
    }
    AccountConfig* FindAccount(const std::string& id) {
        for (auto& a : accounts)
            if (a.id == id) return &a;
        return nullptr;
    }
    bool HasType(AccountType t) const {
        for (const auto& a : accounts)
            if (a.type == t) return true;
        return false;
    }
    // 默认账户集（首次运行/配置损坏；顺序=历史卡片序 K/C/D/G）
    static std::vector<AccountConfig> Defaults();

    int EffectiveIntervalSec() const {
        return query.power_save ? 900 : query.interval_sec;
    }
};

// 序列化与加载（storage.cpp）。Load 兼容 v1：无 accounts 键时按旧平铺字段迁移。
struct ConfigIo {
    static std::string Dump(const AppConfig& c);
    static bool Load(const std::string& json_text, AppConfig& c);
};

// 卡片拖拽排序（F3）：把第 vis_from 个"可见"元素移动到可见序列的第 vis_to 位，
// 不可见元素保持原位。is_visible 投影区分两类元素。纯函数（selftest 可测）。
template <class T, class VisFn>
void MoveVisibleInVector(std::vector<T>& v, size_t vis_from, int vis_to, VisFn&& is_visible) {
    std::vector<size_t> slots; // 可见槽位（旧序=位置升序）
    for (size_t i = 0; i < v.size(); ++i)
        if (is_visible(v[i])) slots.push_back(i);
    if (vis_from >= slots.size() || slots.size() < 2) return;
    int dst = vis_to;
    if (dst < 0) dst = 0;
    if (dst >= (int)slots.size()) dst = (int)slots.size() - 1;
    if ((int)vis_from == dst) return;
    // 新可见序：取走 from，再插入 dst（元素用旧下标表达）
    std::vector<size_t> order = slots;
    size_t moved = order[vis_from];
    order.erase(order.begin() + vis_from);
    order.insert(order.begin() + dst, moved);
    // 置换写法：每个旧下标恰好读一次、每个槽位恰好写一次（不可见元素原位）
    std::vector<T> out(v.size());
    for (size_t j = 0; j < slots.size(); ++j) out[slots[j]] = std::move(v[order[j]]);
    for (size_t i = 0; i < v.size(); ++i)
        if (!is_visible(v[i])) out[i] = std::move(v[i]);
    v = std::move(out);
}

} // namespace qb
