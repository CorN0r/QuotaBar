// state.hpp — 各平台独立状态快照、新鲜度与显示模型。
// 本文件结构为内部数据模型，不是服务商返回字段（见设计文档 §6）。
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "config.hpp" // AccountType（嵌入条/圆环条目的原厂图标取图键）

namespace qb {

struct Decimal;
int64_t CompareDecimal(const Decimal& a, const Decimal& b);

enum class PeriodKind { Monthly, Weekly, Rolling, Other, Unknown };
enum class Availability { Known, Unknown, Invalid };

enum class FetchStatus {
    Unconfigured,      // 尚未配置 Key
    Fetching,          // 查询中
    Ok,                // 最近一次成功
    Offline,           // 网络/超时等失败（可能有旧值）
    AuthError,         // 401/403：凭证或权限问题，停止自动重试
    QueryRateLimited,  // 429：查询受限
    ProtocolError,     // 404/410/结构不兼容：暂停自动查询
    RequestError,      // 其他 4xx
    Paused,            // 用户暂停查询
};

struct QuotaWindow {
    std::string stable_key;                 // 业务标识（周期+服务端字段），不依赖数组次序
    std::string label;                      // 展示标签："5h余"/"7d余"/"月余"/"X"
    PeriodKind period_kind = PeriodKind::Unknown;
    int64_t duration_seconds = 0;           // rolling 窗口时长；0=未提供
    bool has_limit = false, has_used = false, has_remaining = false;
    double limit = 0, used = 0, remaining = 0;
    int32_t remaining_percent = -1;         // -1=不可计算；0..100=向下取整
    double used_percent_raw = -1;           // 服务端仅给比例(Codex wham)：>=0 时无绝对数，剩余%=100-它
    bool tiny_positive = false;             // 0 < 剩余 < 1%
    bool has_reset = false;
    int64_t reset_at_utc = 0;               // 0=未知/不可解析
    int64_t observed_at_utc = 0;            // 该项自己的获取时间（UTC）
    int64_t observed_mono = 0;              // 进程内单调毫秒
    Availability availability = Availability::Unknown;
};

struct MoneyBalance {
    std::string currency;                   // "CNY"/"USD"/... 未知币种保留原代码
    bool has_total = false;
    double total = 0;                       // 展示用两位小数；比较用 Decimal
    std::string total_raw;                  // 服务端原始金额字符串（十进制）
    bool has_granted = false, has_topped = false;
    std::string granted_raw, topped_raw;
    std::optional<bool> is_available;       // 账户可调用状态（服务端）
    bool availability_invalid = false;      // 金额字段非法
    int64_t observed_at_utc = 0;
    int64_t observed_mono = 0;
};

struct ProviderSnapshot {
    std::string provider_id;                // 平台类型："kimi"|"deepseek"|"codex"|"glm"
    std::string account_id;                 // 账户路由键（多账户隔离；与 config.accounts[].id 一致）
    int64_t credential_revision = 0;
    FetchStatus status = FetchStatus::Unconfigured;
    int64_t last_attempt_at_utc = 0;        // 0=从未尝试
    int64_t last_success_at_utc = 0;        // 平台级概览；不覆盖单项旧数据标记
    int64_t last_attempt_mono = 0;
    int64_t last_success_mono = 0;
    int64_t next_allowed_mono = 0;          // 0=无限制（受 Retry-After/手动限制）
    std::string error_text;                 // 已脱敏
    int http_status = 0;
    bool from_cache = false;                // 来自磁盘缓存，本会话尚未联网验证
    std::string contract_revision;          // 解析所依据的契约版本标识
    std::string completeness;               // "full" | "partial" | "unknown"
    std::vector<QuotaWindow> windows;
    std::vector<MoneyBalance> balances;

    const QuotaWindow* FindWindow(const std::string& stable_key) const;
    // 配置了 Key（可以参与查询）
    bool IsConfigured() const { return credential_revision > 0; }
};

// ---------- 序列化（storage 用） ----------
struct SnapshotJson {
    static std::string Dump(const ProviderSnapshot& s);
    static bool Parse(const std::string& json_text, ProviderSnapshot& out);
};

// ---------- 新鲜度 ----------
// 单项数据是否过期：距 observed 超过 2×当前有效间隔，或来自缓存未验证，或本次已标记失效
bool IsWindowStale(const QuotaWindow& w, const ProviderSnapshot& snap, int interval_sec,
                   int64_t now_mono);
bool IsBalanceStale(const MoneyBalance& b, const ProviderSnapshot& snap, int interval_sec,
                    int64_t now_mono);

// 主币种余额选择器：CNY 优先，否则第一项；空列表返回 nullptr。
// 通知阈值/常驻行/圆环/详情/历史采样共用同一口径，不得各自内联复制（口径漂移源）。
const MoneyBalance* PrimaryBalance(const std::vector<MoneyBalance>& balances);

// ---------- 显示模型 ----------
// 颜色常量（文字模式为黑时使用深色变体）
struct DisplayColors {
    COLORREF normal = RGB(255, 255, 255);
    COLORREF warn = RGB(255, 176, 32);
    COLORREF bad = RGB(255, 92, 92);
};
struct DisplaySegment {
    std::wstring text;
    COLORREF color;
    bool operator==(const DisplaySegment&) const = default;
};
struct DisplayModel {
    std::vector<std::vector<DisplaySegment>> lines;
    bool operator==(const DisplayModel& o) const { return lines == o.lines; }
    bool operator!=(const DisplayModel& o) const { return !(*this == o); }
};

// 嵌入任务栏两行布局：第一行 agent 名称（Kimi/DeepSeek/Codex/GLM），第二行对应额度（列对齐）。
// 值段沿用显示模型的语义（label+pct/金额+颜色）；名称行颜色为 base（不随额度状态变色）。
struct EmbedRowItem {
    std::wstring name;                  // "Kimi" / "DeepSeek" / "Codex" / "GLM"
    COLORREF name_color;
    std::vector<DisplaySegment> values; // 额度段（保色）；空=暂无值
    AccountType type = AccountType::Kimi; // 原厂图标替代名称时按类型取图（embed_brand_icons）
    bool operator==(const EmbedRowItem& o) const {
        return name == o.name && name_color == o.name_color && values == o.values &&
               type == o.type;
    }
};
struct DisplayOptions {
    bool two_lines = true;
    bool dark_text = false;          // true=深色文字（浅色背景场景）
    int low_percent_threshold = 20;  // 剩余 ≤ 此百分比显示琥珀色
    double low_money_cny = 5.0;      // 金额账户剩余 ≤ 此金额（CNY 口径）显示琥珀色
    bool low_quota_color = true;     // false=额度值段恒基色（「需更新密钥」等状态告警色不受影响）
    bool brand_color_names = false;  // true=账户名前缀段/嵌入名称行用平台品牌色（值段仍走语义色）
    bool embed_show_windows = true;  // false=嵌入值行剥掉窗口标签（"5h余 43%"→"43%"）
};

// 值段颜色：低额琥珀 / 耗尽红；low_quota_color 关闭时额度值恒基色。纯函数（selftest 可测）。
COLORREF ValueColor(const DisplayOptions& opt, COLORREF base, bool is_low, bool is_empty);
// 状态告警色（需更新密钥/接口变化等）：始终生效，不受 low_quota_color 影响。
COLORREF WarnColor(const DisplayOptions& opt, COLORREF base);
// 逐账户低额阈值（百分比窗口）：alert_pct 0=继承全局 1=关闭(返回 -1) 5..95=自定义。
// 金额账户只在 ==1 时视为关闭（余额阈值无逐账户自定义项）。
int EffectiveLowPct(const struct AccountConfig& a, int global_pct);

// 圆环模式条目：一个厂商一环。环弧=剩余比例，环心=数值文本，环顶压产品首字母徽章。
// 颜色由 state 层按主题/低额状态算好（COLORREF 无 alpha，透明度由 overlay 按状态叠加）。
struct RingItem {
    wchar_t badge = 0;          // 产品首字母 K/D/C/G
    AccountType type = AccountType::Kimi; // 原厂图标徽章按类型取图（embed_brand_icons）
    COLORREF accent = 0;        // 徽章底色（品牌近似色）
    COLORREF value_color = 0;   // 环弧与环心文字颜色（随低额/耗尽/主题）
    COLORREF track_color = 0;   // 环轨道颜色
    std::wstring center;        // 环心文本："62%" / "<1%" / "¥23.48" / "…" / "—" / "!"
    int arc_percent = -1;       // 0..100 环弧扫过比例；-1=无比例语义
    bool full_ring = false;     // 满环（金额类：无比例，仅状态色）
    bool low = false;           // 低额（琥珀）
    bool empty = false;         // 耗尽（红）
    bool stale = false;         // 旧数据（overlay 半透明呈现）
    bool paused = false;        // 查询暂停（overlay 半透明呈现）
    bool unconfigured = false;  // 未配置（灰环灰徽章）
    bool operator==(const RingItem&) const = default;
};

// ---------- 账户视图（构建器输入） ----------
// 调用方决定进入显示的账户集合（常驻条/圆环=enabled 的账户；详情页=card_visible 的账户）。
struct AccountConfig;
struct AccountView {
    const AccountConfig* cfg = nullptr;     // id/type/alias（别名作为显示名）
    const ProviderSnapshot* snap = nullptr;
};

// 输入：账户视图列表。now 为 UTC 秒 / 单调毫秒。
// 每个账户产出一行；首段恒为「别名 」前缀段，其后为额度值段（嵌入条按此拆名称行/值行）。
DisplayModel BuildDisplayModel(const std::vector<AccountView>& accounts,
                               const DisplayOptions& opt,
                               int64_t now_unix, int64_t now_mono, int interval_sec);

// 构造嵌入条目（纯函数，selftest 可测）：名称=别名，值=首段之后的额度段
std::vector<EmbedRowItem> BuildEmbedItems(const std::vector<AccountView>& accounts,
                                          const DisplayOptions& opt,
                                          int64_t now_unix, int64_t now_mono, int interval_sec);

// 构造圆环条目（纯函数，selftest 可测）：徽章=别名首字符（空则平台字母），品牌色随类型
std::vector<RingItem> BuildRingItems(const std::vector<AccountView>& accounts,
                                     const DisplayOptions& opt,
                                     int64_t now_unix, int64_t now_mono, int interval_sec);

// ---------- 详情仪表盘卡片模型 ----------
// 详情窗结构化数据（纯函数，可测）；文本呈现细节由 UI 层决定。
// 语义色：由 UI 层按 tone 映射到主题 token，state 层不携带界面配色。
enum class DetailTone { Neutral, Ok, Info, Warn, Bad, Muted };
struct DetailWindowRow {
    std::wstring label;          // 窗口短标签："5h" / "7天" / "月"
    std::wstring value_text;     // "62%" / "<1%" / "¥23.48" / "123 次"
    std::wstring reset_text;     // "还有 3小时25分" / "已到期" / "到期未知"
    DetailTone tone = DetailTone::Neutral; // 值文本/环色（低额琥珀、耗尽红）
    float ring_percent = -1.0f;  // >=0 画状态环；-1 无比例语义（金额/次数只画轨道）
};
struct DetailCard {
    wchar_t badge = 0;           // 平台字母 K/D/C/G
    COLORREF accent = 0;         // 品牌徽章色
    std::wstring title;          // 账户别名
    std::wstring subtitle;       // 类型说明："Kimi Code 周期额度" / "ChatGPT 订阅"…
    std::string account_id;      // 所属账户（UI 展开设置/删除卡片路由用）
    bool uses_api_key = true;    // false=codex（设置区显示 auth.json 状态而非 Key 输入）
    std::wstring status_text;    // "正常" / "查询中" / "凭证失效/权限不足"…
    DetailTone status_tone = DetailTone::Muted;
    bool unconfigured = false;   // 未配置：主体显示配置引导
    bool dimmed = false;         // 查询暂停 → 整卡半透明
    std::vector<DetailWindowRow> rows; // 最多取 3 行（构建器内截断）
    std::wstring note;           // 一行补充：错误信息/口径说明/月池状态
    std::wstring last_update;    // "最后成功 14:32" / "最后尝试 …（未成功）"；空=从未查询
    const char* console_url = nullptr; // 控制台 URL（ASCII 字面量）
    // ---- 趋势带（2026-09-15 A 迭代；有 ≥2 个历史样本才有值） ----
    std::vector<double> spark;   // 原始值序列（百分比或余额；UI 归一化绘制）
    std::wstring eta_text;       // "按当前速率 45分钟后用尽"；空=无有效估计
    std::wstring spend_text;     // "今日消耗 ¥12.40"（金额账户）；空=无
    std::wstring trend_span;     // "近 3小时25分"（样本时间跨度；坐标轴说明）
    bool trend_is_percent = false; // true=百分比序列（纵轴固定 0..100）；false=金额
    bool has_trend = false;      // 与 AccountHasTrend 同判定（布局/paint 一致）
};
std::vector<DetailCard> BuildDetailCards(const std::vector<AccountView>& accounts,
                                         int64_t now_unix, int interval_sec,
                                         const struct UsageHistory* hist = nullptr);

const char* FetchStatusText(FetchStatus s); // 中文短语（详情用）

// ---------- 用量历史（2026-09-15 A 迭代）----------
// 每账户滚动采样：百分比窗口存剩余比例，金额账户存余额原始十进制串。
// 采样与速率计算为纯函数（selftest 可测）；磁盘 I/O 在 storage 层（history.json）。
struct UsageSample {
    int64_t t_utc = 0;    // 采样时刻（UTC 秒）
    int32_t pct = -1;     // 剩余百分比（-1 = 不可计算，不参与速率）
};
struct BalanceSample {
    int64_t t_utc = 0;
    std::string total_raw; // 服务端原始金额字符串（Decimal 口径，不落二进制浮点）
};
struct WindowHistory {
    std::string stable_key;                 // 对齐 QuotaWindow::stable_key（前缀匹配同显示层）
    std::vector<UsageSample> samples;
};
struct BalanceHistory {
    std::string currency;
    std::vector<BalanceSample> samples;
};
struct AccountHistory {
    std::string account_id;
    std::string identity_key;               // 身份锚：codex=ChatGPT Account Id 哈希（Fnv1a64Hex）；
                                            // API-Key 账户留空（换 Key 隔离由 cred_rev 负责）
    std::vector<WindowHistory> windows;
    BalanceHistory balance;                 // DeepSeek 型；空 currency = 无
};
struct UsageHistory {
    std::vector<AccountHistory> accounts;
    AccountHistory* Find(const std::string& id);
    const AccountHistory* Find(const std::string& id) const;
    void Purge(int64_t now_utc);            // 截断超保留期的样本（逐序列）
};

// 成功快照 → 追加样本。去重：与尾样本同值且间隔 <10 分钟不重复；序列样本数封顶。
// codex 快照由调用方先锚定身份键（ApplyIdentityKey）再传入。
void RecordSnapshotSamples(AccountHistory& h, const AccountConfig& acc,
                           const ProviderSnapshot& snap, int64_t now_utc);

// 身份键一致性（codex 换 ChatGPT 账户检测；cred_rev 隔离的同口径延伸；纯函数 selftest 可测）。
// 空 key 无操作；段内无键 → 建立新键（旧版磁盘段无键，首次采样补锚，数据保留）；
// 键一致 → 不动；键不同 → 清空该段全部序列并写入新键（换账户后旧曲线不得混入新账户）。
// 返回是否发生了清段（调用方可记日志）。
bool ApplyIdentityKey(AccountHistory& h, const std::string& key);

// 消耗速率与预计用尽时间（线性拟合近期样本；纯函数）。
struct DepletionEstimate {
    bool valid = false;
    double pct_per_hour = 0;   // 负值 = 净消耗（-X%/小时）
    int64_t eta_seconds = 0;   // valid 时 >0：按当前速率到 0% 的秒数
};
DepletionEstimate EstimateDepletion(const std::vector<UsageSample>& samples,
                                    int64_t now_utc);

// 今日消耗（余额差分：今日 0 点（本地时区）前最后样本 与 最新样本 之差）。
// 净增加（充值污染）→ false 不显示。spend_raw 输出两位小数十进制串。
bool TodaySpend(const BalanceHistory& b, int64_t now_utc, std::string& spend_raw);

// 该账户是否有可绘趋势（≥2 个样本）；卡片布局与 BuildDetailCards 共用同一判定。
bool AccountHasTrend(const UsageHistory& hist, const AccountConfig& acc,
                     const ProviderSnapshot& snap);

} // namespace qb
