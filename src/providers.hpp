// providers.hpp — 五个静态适配器（Kimi Code / DeepSeek / Codex / GLM / Antigravity）。不做动态插件加载。
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
//  - Antigravity: POST http://127.0.0.1:<httpPort>/exa.language_server_pb.LanguageServerService/
//    RetrieveUserQuotaSummary（**本机 hub 的本地 RPC,不是 Google 端点**——认证/刷新由 hub 代劳,
//    QuotaBar 全程不接触 OAuth 令牌）。hub 描述文件 ~/.gemini/<product>/daemon/hub_<port>.json =
//    {pid,httpsPort,httpPort,lsVersion,csrfToken};请求头 x-codeium-csrf-token=csrfToken,体 {}。
//    契约=antigravity-quota-summary@2026-11-27 真机锁定（Antigravity 44.9.1 / LS 2.35.2,探针样本
//    experiments/ag_probe/）:响应 {response:{groups[{displayName,buckets[{bucketId,window,
//    remainingFraction,resetTime?}]}]}},桶按 bucketId 分派——gemini-5h/gemini-weekly（Gemini 组）、
//    3p-5h/3p-weekly（Claude&GPT 组）;**组内 weekly 在前**,不可按数组序;未动过的桶（剩余 1.0）
//    无 resetTime;额度耗尽时 remainingFraction 为 proto3 零值被整个省略。
//    社区另报道的直连路线（daily-cloudcode-pa.googleapis.com,需 OAuth 令牌 + UA 版本门控）未采用。
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
inline constexpr const char* kProviderAntigravity = "antigravity";
// 锁定的契约版本标识（写入快照；未来字段漂移时升级并重新验证）
inline constexpr const char* kKimiContract = "kimi-usages@86f1364";
inline constexpr const char* kDeepSeekContract = "deepseek-balance@docs-2026-09";
inline constexpr const char* kCodexContract = "codex-wham@2026-09-05";
inline constexpr const char* kGlmContract = "glm-quota-limit@2026-09-12";
inline constexpr const char* kAntigravityContract = "antigravity-quota-summary@2026-11-27";

struct ProviderRequestSpec {
    std::string provider_id;
    std::string url;              // https（本机 hub 类为 http://127.0.0.1）
    std::string user_agent;       // QuotaBar/<version>
    std::string account_id;       // Codex 专用:ChatGPT-Account-Id 头(由调度器填充)
    bool auth_bearer = true;      // true="Bearer <key>";GLM 端点要求裸 Key,置 false
    // 方法/体/认证头名（默认值 = 旧行为:GET、无体、Authorization 头）。Antigravity hub:
    // POST {} + 头 x-codeium-csrf-token（见 BuildAntigravityRequest）
    std::string method = "GET";
    std::string body;                       // 非空 → 随请求发送,Content-Type: application/json
    const char* auth_header = "Authorization"; // 密钥所在头名;空串 = 不发认证头
};

// 无 API Key 的本机凭证类是否只能建一张卡片（codex 读 auth.json、antigravity 读本机 hub:
// 凭证源全机唯一,多卡只会重复显示同一账户）。纯函数（selftest 覆盖）。
bool ProviderSingleCard(AccountType t);
// 是否参与低额预警阈值（比例窗口类:Kimi/GLM/Antigravity）。codex 历史上不参与,只做重置
// 检测——沿用现状,勿顺手"补全"。卡片阈值行、条上低额变色与阈值通知共用此判定。
bool ProviderAlertCapable(AccountType t);

// ---- 账户类型辅助（类型 → 平台 id/徽章/控制台；providers.cpp 实现） ----
const char* ProviderIdOf(AccountType t);        // "kimi"/"codex"/"glm"/"deepseek"
wchar_t ProviderBadgeOf(AccountType t);         // K/C/G/D（品牌徽章字母）
const char* ProviderConsoleUrl(AccountType t);  // 控制台 URL（ASCII）
const char* ProviderDefaultAlias(AccountType t); // 默认别名（UTF-8）
bool ProviderUsesApiKey(AccountType t);         // false=codex（免密直连 auth.json）

// local_base = 本机 hub 类 provider 的现场基址（"http://127.0.0.1:<port>",随 hub 重启变化,
// 由调度器经宿主解析后传入）;其余类型忽略该参数,返回固定地址。
ProviderRequestSpec BuildKimiRequest(const std::string& local_base);
ProviderRequestSpec BuildDeepSeekRequest(const std::string& local_base);
ProviderRequestSpec BuildCodexRequest(const std::string& local_base);
ProviderRequestSpec BuildGlmRequest(const std::string& local_base);
ProviderRequestSpec BuildAntigravityRequest(const std::string& local_base);

// 合成假数据（测试模式），显式标注 test_mode 由调用方处理
ProviderSnapshot MakeKimiTestSnapshot(int64_t now_utc);
ProviderSnapshot MakeDeepSeekTestSnapshot(int64_t now_utc);
ProviderSnapshot MakeCodexTestSnapshot(int64_t now_utc);
ProviderSnapshot MakeGlmTestSnapshot(int64_t now_utc);
ProviderSnapshot MakeAntigravityTestSnapshot(int64_t now_utc);

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
ParseResult ParseAntigravityResponse(const std::string& body, int64_t now_utc, int64_t now_mono);

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
    bool uses_api_key;         // false=本机凭证类（codex 读 auth.json、antigravity 读本机 hub）
    ProviderRequestSpec (*build_request)(const std::string& local_base);
    ParseResult (*parse)(const std::string& body, int64_t now_utc, int64_t now_mono);
    COLORREF brand_color;      // 品牌色（圆环 accent/详情卡 accent/卡片章共用）
    const wchar_t* subtitle;   // 详情卡副题（中文宽字面）
    const wchar_t* type_tag;   // 卡头类型章文案（中文宽字面）
    bool local_hub;            // true=本机 hub 数据源（antigravity）：端口/令牌随 hub 启停变化,
                               // 缺源与瞬态失败按短周期重探、401 不停机（scheduler 消费）
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
