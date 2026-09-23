// providers.hpp — 四个静态适配器（Kimi Code / DeepSeek / Codex / GLM）。不做动态插件加载。
// 契约依据（设计文档 §2.1/§7/§8）：
//  - Kimi:  GET https://api.kimi.com/coding/v1/usages （kimi-cli 提交 86f136422a0aae6b217ea49e7ea1d2e8a1defcd2）
//  - DeepSeek: GET https://api.deepseek.com/user/balance （官方公开文档）
//  - Codex: GET https://chatgpt.com/backend-api/wham/usage（官方 Codex CLI 自有查询端点,
//    仅给已用百分比/重置时间,无绝对额度;认证=ChatGPT OAuth 令牌,取自 ~/.codex/auth.json,
//    QuotaBar 只读不写,见 app.cpp ReadCodexAuth;契约版本=codex-wham@2026-09-05 实机样本锁定）
//  - GLM:   GET https://open.bigmodel.cn/api/monitor/usage/quota/limit（智谱 GLM Coding Plan
//    额度端点;认证=API Key 且 Authorization 头【不带】Bearer 前缀。契约=glm-quota-limit
//    @2026-09-12 真机锁定（探针样本 experiments/glm_probe/）:响应 data.limits[] 按 type 分派,
//    CREDIT_LIMIT=带绝对点数+unit/number 窗口时长（(3,5)=5h、(6,1)=周）+毫秒 nextResetTime;
//    TOKENS_LIMIT/TIME_LIMIT 为旧形态,保留兼容）
#pragma once
#include "config.hpp" // AccountType（账户类型 → 适配器分派）
#include "state.hpp"
#include <string>
#include <vector>

namespace qb {

inline constexpr const char* kProviderKimi = "kimi";
inline constexpr const char* kProviderDeepSeek = "deepseek";
inline constexpr const char* kProviderCodex = "codex";
inline constexpr const char* kProviderGlm = "glm";
// 锁定的契约版本标识（写入快照；未来字段漂移时升级并重新验证）
inline constexpr const char* kKimiContract = "kimi-usages@86f1364";
inline constexpr const char* kDeepSeekContract = "deepseek-balance@docs-2026-09";
inline constexpr const char* kCodexContract = "codex-wham@2026-09-05";
inline constexpr const char* kGlmContract = "glm-quota-limit@2026-09-12";

struct ProviderRequestSpec {
    std::string provider_id;
    std::string url;              // https
    std::string user_agent;       // QuotaBar/<version>
    std::string account_id;       // Codex 专用:ChatGPT-Account-Id 头(由调度器填充)
    bool auth_bearer = true;      // true="Bearer <key>";GLM 端点要求裸 Key,置 false
};

// ---- 账户类型辅助（类型 → 平台 id/徽章/控制台；providers.cpp 实现） ----
const char* ProviderIdOf(AccountType t);        // "kimi"/"codex"/"glm"/"deepseek"
wchar_t ProviderBadgeOf(AccountType t);         // K/C/G/D（品牌徽章字母）
const char* ProviderConsoleUrl(AccountType t);  // 控制台 URL（ASCII）
const char* ProviderDefaultAlias(AccountType t); // 默认别名（UTF-8）
bool ProviderUsesApiKey(AccountType t);         // false=codex（免密直连 auth.json）

ProviderRequestSpec BuildKimiRequest();
ProviderRequestSpec BuildDeepSeekRequest();
ProviderRequestSpec BuildCodexRequest();
ProviderRequestSpec BuildGlmRequest();

// 合成假数据（测试模式），显式标注 test_mode 由调用方处理
ProviderSnapshot MakeKimiTestSnapshot(int64_t now_utc);
ProviderSnapshot MakeDeepSeekTestSnapshot(int64_t now_utc);
ProviderSnapshot MakeCodexTestSnapshot(int64_t now_utc);
ProviderSnapshot MakeGlmTestSnapshot(int64_t now_utc);

struct ParseResult {
    bool ok = false;               // 200 且 JSON 有效且顶层结构可识别
    bool protocol_error = false;   // 404/410/结构不兼容
    std::string error;             // 脱敏描述
    std::string completeness = "unknown"; // full | partial | unknown
    std::vector<QuotaWindow> windows;
    std::vector<MoneyBalance> balances;
};

ParseResult ParseKimiResponse(const std::string& body, int64_t now_utc, int64_t now_mono);
ParseResult ParseDeepSeekResponse(const std::string& body, int64_t now_utc, int64_t now_mono);
ParseResult ParseCodexResponse(const std::string& body, int64_t now_utc, int64_t now_mono);
ParseResult ParseGlmResponse(const std::string& body, int64_t now_utc, int64_t now_mono);

// ---- 适配器分派表（2026-09-18 收敛）----
// "类型 → 平台行为 + 视觉身份"的唯一真值：providers 的辅助 switch、scheduler 的
// 三处分派（组请求/选解析器/写契约版本）与原四文件散落的品牌映射（theme 品牌色/
// state 副题/tray_ui 类型章）全部查这张表。新增 provider = 加一行表项 + 实现
// build/parse + 各层一处 case，不再多点复制（口径漂移源）。
// 视觉身份（品牌色/副题/类型章）是固定品牌常量、不随深浅主题变——放本层而非
// theme（theme 只管随主题变的 token）；state 因此不再依赖 theme，恢复其
// "显示模型不携带界面配色"的自述契约（state.hpp DetailTone 注释）。
struct ProviderOps {
    const char* id;            // 平台 id（provider_id 路由键）
    const char* contract;      // 锁定契约版本（写入快照 contract_revision）
    wchar_t badge;             // 品牌徽章字母
    const char* console_url;   // 控制台 URL（ASCII）
    const char* default_alias; // 默认别名（UTF-8）
    bool uses_api_key;         // false=codex（免密直连 ~/.codex/auth.json）
    ProviderRequestSpec (*build_request)();
    ParseResult (*parse)(const std::string& body, int64_t now_utc, int64_t now_mono);
    COLORREF brand_color;      // 品牌色（圆环 accent/详情卡 accent/卡片章共用）
    const wchar_t* subtitle;   // 详情卡副题（中文宽字面）
    const wchar_t* type_tag;   // 卡头类型章文案（中文宽字面）
};
// 按账户类型取表项；未列类型回落 Kimi（与旧 switch 的 default 同语义）
const ProviderOps& ProviderOpsOf(AccountType t);
// 按平台 id 查表（调度器按 provider_id 路由响应）；未知 id 返回 nullptr
const ProviderOps* ProviderOpsById(const std::string& id);
// 按徽章字母查表（tray_ui 卡片绘制只持 badge）；未知字母回落 Kimi（与旧 switch 同）
const ProviderOps& ProviderOpsByBadge(wchar_t badge);

// ---------- 额度计算规则（§6，selftest 复用） ----------
struct CountTriple {
    bool has_limit = false, has_used = false, has_remaining = false;
    double limit = 0, used = 0, remaining = 0;
};
// 计算剩余比例；availability 输出 Known/Unknown/Invalid。
// 规则：limit<=0 → Unknown；used/remaining 负值 → Invalid；
//       二者同时存在且矛盾（误差 > max(1, limit*0.5%)）→ Invalid；
//       优先 remaining；仅 used 时 remaining=limit-used。
void ComputePercent(const CountTriple& c, QuotaWindow& out);

// Kimi 时长标签：18000s → "5h余"；300m 已在窗口内换算秒
std::string KimiRollingLabel(int64_t duration_seconds);
// TIME_UNIT_* → 秒（未知 → 0，period=Other）
int64_t KimiTimeUnitSeconds(const std::string& unit, bool& known);

} // namespace qb
