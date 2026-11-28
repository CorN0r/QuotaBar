#include "providers.hpp"
#include "util.hpp"
#include "json.hpp"
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace qb {

ProviderRequestSpec BuildKimiRequest(const std::string&) {
    ProviderRequestSpec r;
    r.provider_id = kProviderKimi;
    r.url = "https://api.kimi.com/coding/v1/usages";
    return r;
}
ProviderRequestSpec BuildDeepSeekRequest(const std::string&) {
    ProviderRequestSpec r;
    r.provider_id = kProviderDeepSeek;
    r.url = "https://api.deepseek.com/user/balance";
    return r;
}
ProviderRequestSpec BuildCodexRequest(const std::string&) {
    ProviderRequestSpec r;
    r.provider_id = kProviderCodex;
    r.url = "https://chatgpt.com/backend-api/wham/usage";
    // ChatGPT-Account-Id 头由调度器经 ProviderRequest::extra_headers 附加（宿主 GetAccountId）
    return r;
}
ProviderRequestSpec BuildGlmRequest(const std::string&) {
    ProviderRequestSpec r;
    r.provider_id = kProviderGlm;
    r.url = "https://open.bigmodel.cn/api/monitor/usage/quota/limit";
    r.auth_bearer = false; // 监控端点要求 Authorization: <key>（无 Bearer 前缀）
    return r;
}
ProviderRequestSpec BuildAntigravityRequest(const std::string& local_base) {
    ProviderRequestSpec r;
    r.provider_id = kProviderAntigravity;
    // 基址 = 现场解析的本机 hub（httpPort 每次启动都变）+ 固定 RPC 路径。走明文回环
    // httpPort：hub 另有自签证书的 httpsPort,回环明文已足够,不为它开证书校验口子。
    r.url = local_base + "/exa.language_server_pb.LanguageServerService/RetrieveUserQuotaSummary";
    r.auth_bearer = false;                    // 密钥即 CSRF 令牌本体
    r.method = "POST";
    // 请求体照官方插件的约定发空 JSON 对象（实测请求体不是合法 JSON 时 hub 回 400;
    // 空 body 未测过,不冒险）
    r.body = "{}";
    r.auth_header = "x-codeium-csrf-token";   // 令牌不走 Authorization
    return r;
}

// ---------- 账户类型辅助（全部委托分派表，行为与旧 switch 逐分支等价） ----------
const char* ProviderIdOf(AccountType t) { return ProviderOpsOf(t).id; }
wchar_t ProviderBadgeOf(AccountType t) { return ProviderOpsOf(t).badge; }
const char* ProviderConsoleUrl(AccountType t) { return ProviderOpsOf(t).console_url; }
const char* ProviderDefaultAlias(AccountType t) { return ProviderOpsOf(t).default_alias; }
bool ProviderUsesApiKey(AccountType t) { return ProviderOpsOf(t).uses_api_key; }
bool ProviderSingleCard(AccountType t) { return !ProviderUsesApiKey(t); }
bool ProviderAlertCapable(AccountType t) {
    return t == AccountType::Kimi || t == AccountType::Glm || t == AccountType::Antigravity;
}

// ---------- 适配器分派表（新增 provider 在此加一行） ----------
// 品牌色原定义于 theme.hpp kBrand*（2026-09-18 迁入：视觉身份属平台常量，
// theme 只保留随主题变的 token）；副题原在 state.cpp type_subtitle、类型章
// 原在 tray_ui.cpp TypeTag，收敛至此单点。
// antigravity 品牌色取自官方应用图标拱形主体的主色（experiments/ag_probe 实测 #3888F8）;
// 副题不重复别名里的产品名（卡头标题行有状态 chip 时副题右侧只剩 ~110 DIP,长了会被 ⓘ 截断）。
// clang-format off
static const ProviderOps kOpsTable[] = {
    // id             contract                    badge  console_url                                  default_alias  uses_key  build                 parse                  brand_color          subtitle                        type_tag            hub
    { kProviderKimi,       kKimiContract,       L'K', "https://www.kimi.com/code",                 "Kimi Code",   true,    BuildKimiRequest,       ParseKimiResponse,       RGB(0x56,0x60,0x74), L"Kimi Code 周期额度",          L"周期配额",        false },
    { kProviderCodex,      kCodexContract,      L'C', "https://chatgpt.com/codex/settings/usage", "Codex",       false,   BuildCodexRequest,      ParseCodexResponse,      RGB(0x10,0xA3,0x7F), L"Codex · ChatGPT 订阅",         L"免密直连",        false },
    { kProviderDeepSeek,   kDeepSeekContract,   L'D', "https://platform.deepseek.com",             "DeepSeek",    true,    BuildDeepSeekRequest,   ParseDeepSeekResponse,   RGB(0x4D,0x6B,0xFE), L"Pay-as-you-go 结算账户",       L"按量付费",        false },
    { kProviderGlm,        kGlmContract,        L'G', "https://bigmodel.cn",                       "GLM",         true,    BuildGlmRequest,        ParseGlmResponse,        RGB(0x7C,0x5C,0xFF), L"智谱 BigModel 编程订阅",       L"编码订阅",        false },
    { kProviderAntigravity,kAntigravityContract,L'A', "https://antigravity.google",                "Antigravity", false,   BuildAntigravityRequest,ParseAntigravityResponse, RGB(0x38,0x88,0xF8), L"Google AI 订阅",               L"本机直连",        true  },
};
// clang-format on

const ProviderOps& ProviderOpsOf(AccountType t) {
    switch (t) {
        case AccountType::Codex: return kOpsTable[1];
        case AccountType::Glm: return kOpsTable[3];
        case AccountType::Deepseek: return kOpsTable[2];
        case AccountType::Antigravity: return kOpsTable[4];
        default: return kOpsTable[0]; // 未列类型回落 Kimi（与旧 switch 同）
    }
}
const ProviderOps* ProviderOpsById(const std::string& id) {
    for (const auto& ops : kOpsTable)
        if (id == ops.id) return &ops;
    return nullptr;
}
const ProviderOps& ProviderOpsByBadge(wchar_t badge) {
    for (const auto& ops : kOpsTable)
        if (ops.badge == badge) return ops;
    return kOpsTable[0]; // 未知字母（如别名首字符非平台徽章）回落 Kimi（与旧 switch 同）
}

// ---------- 额度计算（§6） ----------
void ComputePercent(const CountTriple& c, QuotaWindow& out) {
    out.tiny_positive = false;
    out.remaining_percent = -1;

    if (c.has_limit && (!std::isfinite(c.limit) || c.limit < 0)) {
        out.availability = Availability::Invalid;
        return;
    }
    if (c.has_used && (!std::isfinite(c.used) || c.used < 0)) {
        out.availability = Availability::Invalid;
        return;
    }
    if (c.has_remaining && (!std::isfinite(c.remaining) || c.remaining < 0)) {
        out.availability = Availability::Invalid;
        return;
    }
    if (c.has_limit && c.limit > 0 && c.has_remaining && c.remaining > c.limit) {
        // 剩余大于上限：不得显示正常百分比
        out.availability = Availability::Invalid;
        return;
    }
    if (!c.has_limit || c.limit <= 0) {
        // limit=0 无明确业务定义：未知，不当无限或耗尽
        out.availability = Availability::Unknown;
        return;
    }
    double rem;
    if (c.has_remaining) {
        rem = c.remaining;
    } else if (c.has_used) {
        rem = c.limit - c.used;
    } else {
        out.availability = Availability::Unknown;
        return;
    }
    // used 与 remaining 同时存在时检查矛盾
    if (c.has_used && c.has_remaining) {
        double expect = c.limit - c.used;
        double tol = std::max(1.0, c.limit * 0.005);
        if (std::fabs(expect - c.remaining) > tol) {
            out.availability = Availability::Invalid;
            return;
        }
    }
    if (rem < 0) rem = 0; // 已用略超限：显示剩余 0（异常信息保留在 details 由 used>limit 判断）
    out.availability = Availability::Known;
    out.has_remaining = true;
    out.remaining = rem;
    double frac = rem / c.limit;
    if (rem > 0) {
        int pct = (int)std::floor(frac * 100.0 + 1e-9);
        if (pct == 0) {
            out.tiny_positive = true;
            out.remaining_percent = 0;
        } else {
            out.remaining_percent = std::min(pct, 100);
        }
    } else {
        out.remaining_percent = 0; // 明确耗尽
    }
}

// ---------- Kimi ----------
std::string KimiRollingLabel(int64_t duration_seconds) {
    if (duration_seconds <= 0) return "未知周期";
    char buf[32];
    if (duration_seconds % 86400 == 0) {
        sprintf_s(buf, "%lldd余", (long long)(duration_seconds / 86400));
    } else if (duration_seconds % 3600 == 0) {
        sprintf_s(buf, "%lldh余", (long long)(duration_seconds / 3600));
    } else if (duration_seconds % 60 == 0) {
        sprintf_s(buf, "%lldm余", (long long)(duration_seconds / 60));
    } else {
        sprintf_s(buf, "%llds余", (long long)duration_seconds);
    }
    return buf;
}

int64_t KimiTimeUnitSeconds(const std::string& unit, bool& known) {
    known = true;
    if (unit == "TIME_UNIT_MINUTE") return 60;
    if (unit == "TIME_UNIT_HOUR") return 3600;
    if (unit == "TIME_UNIT_DAY") return 86400;
    if (unit == "TIME_UNIT_WEEK") return 7 * 86400;
    if (unit == "TIME_UNIT_MONTH") return 30 * 86400; // 仅供滚动时长换算，不等于月池承诺
    known = false;
    return 0;
}

namespace {

// 重置字段存在多种命名；逐个尝试
int64_t ExtractResetTime(const JsonValue& obj, bool& has) {
    static const char* names[] = {"resetTime", "reset_time", "resetAt", "reset_at",
                                  "resetsAt", "resets_at", "nextResetTime", "next_reset_time"};
    has = false;
    for (const char* n : names) {
        const JsonValue& v = obj.Find(n);
        if (v.IsString()) {
            auto t = ParseIso8601Utc(v.AsString());
            if (t) {
                has = true;
                return *t;
            }
            return 0; // 找到字段但不可解析：保留"重置未知"
        }
        if (v.IsNumber()) {
            int64_t sec = 0;
            if (v.AsInt(sec) && sec > 0) {
                // 毫秒时间戳（GLM nextResetTime 实测毫秒）换算为秒；1e12 以上的秒级
                // 时间戳要到 3 万年后，判据安全
                if (sec > 1000000000000LL) sec /= 1000;
                has = true;
                return sec;
            }
            return 0;
        }
    }
    return 0;
}

// Kimi API 实测(2026-09-05 真实响应):limit/used/remaining 等计数以 JSON 字符串返回
// ("100" 而非 100),IsNumber() 会识别失败导致整窗静默跳过。先按数字取,失败再解析字符串。
double CountField(const JsonValue& obj, const char* name, bool& has) {
    const JsonValue& v = obj.Find(name);
    has = false;
    if (v.IsNumber()) {
        has = true;
        return v.AsDouble();
    }
    if (v.IsString()) {
        const std::string& s = v.AsString();
        char* end = nullptr;
        double d = std::strtod(s.c_str(), &end);
        if (end != s.c_str() && *end == '\0') {
            has = true;
            return d;
        }
    }
    return 0; // 无计数：跳过（未提供）
}

void ApplyCounts(const JsonValue& obj, const std::string& stable_key, const std::string& label,
                 PeriodKind kind, int64_t duration_seconds, int64_t now_utc, int64_t now_mono,
                 std::vector<QuotaWindow>& out) {
    CountTriple c;
    c.has_limit = false;
    c.limit = CountField(obj, "limit", c.has_limit);
    c.has_used = false;
    c.used = CountField(obj, "used", c.has_used);
    c.has_remaining = false;
    c.remaining = CountField(obj, "remaining", c.has_remaining);
    if (!c.has_limit && !c.has_used && !c.has_remaining) return; // 无计数：跳过（未提供）

    QuotaWindow w;
    w.stable_key = stable_key;
    w.label = label;
    w.period_kind = kind;
    w.duration_seconds = duration_seconds;
    w.has_limit = c.has_limit;
    w.limit = c.limit;
    w.has_used = c.has_used;
    w.used = c.used;
    ComputePercent(c, w);
    w.reset_at_utc = ExtractResetTime(obj, w.has_reset);
    w.observed_at_utc = now_utc;
    w.observed_mono = now_mono;
    out.push_back(std::move(w));
}

} // namespace

ParseResult ParseKimiResponse(const std::string& body, int64_t now_utc, int64_t now_mono) {
    ParseResult r;
    auto root = JsonParse(body, &r.error);
    if (!root) {
        r.error = "JSON 解析失败（响应结构不兼容）";
        r.protocol_error = true;
        return r;
    }
    if (!root->IsObject()) {
        r.error = "顶层不是对象（结构不兼容）";
        r.protocol_error = true;
        return r;
    }
    const JsonValue& usage = root->Find("usage");
    const JsonValue& limits = root->Find("limits");
    bool usage_is_obj = usage.IsObject();
    bool limits_is_arr = limits.IsArray();
    // 契约要求至少其一（kimi-cli 86f1364 的解析路径）
    if (!usage_is_obj && !limits_is_arr) {
        r.error = "缺少 usage/limits（结构不兼容）";
        r.protocol_error = true;
        return r;
    }

    if (usage_is_obj) {
        // 官方 CLI 对顶层 usage 使用周额度默认标签（锁定契约内使用；未来有显式周期字段时优先字段）
        ApplyCounts(usage, "kimi_weekly", "7d余", PeriodKind::Weekly, 0, now_utc, now_mono, r.windows);
    }
    if (limits_is_arr) {
        for (const auto& item : limits.AsArray()) {
            if (!item.IsObject()) continue;
            const JsonValue* counts = &item;
            const JsonValue& detail = item.Find("detail");
            if (detail.IsObject()) counts = &detail;

            const JsonValue& win = item.Find("window");
            int64_t duration = 0;
            PeriodKind kind = PeriodKind::Other;
            if (win.IsObject()) {
                int64_t dur = 0;
                win.Find("duration").AsInt(dur);
                const JsonValue& tu = win.Find("timeUnit");
                std::string unit = tu.IsString() ? tu.AsString() : "";
                bool known_unit = false;
                int64_t unit_sec = unit.empty() ? 1 : KimiTimeUnitSeconds(unit, known_unit);
                if (known_unit && dur > 0) {
                    duration = dur * unit_sec;
                    kind = PeriodKind::Rolling;
                } else if (!unit.empty()) {
                    kind = PeriodKind::Other; // 未知周期：不自动套成周/月
                }
            }
            char key[48];
            if (duration > 0) sprintf_s(key, "kimi_rolling_%llds", (long long)duration);
            else sprintf_s(key, "kimi_limit_%zu", r.windows.size());
            ApplyCounts(*counts, key, KimiRollingLabel(duration), kind, duration, now_utc, now_mono,
                        r.windows);
        }
    }

    r.ok = true;
    // 部分完整：usage 缺失或 limits 为空数组
    if (!usage_is_obj || !limits_is_arr) r.completeness = "partial";
    else r.completeness = "full";
    r.error.clear();
    return r;
}

// ---------- DeepSeek ----------
ParseResult ParseDeepSeekResponse(const std::string& body, int64_t now_utc, int64_t now_mono) {
    ParseResult r;
    auto root = JsonParse(body, &r.error);
    if (!root) {
        r.error = "JSON 解析失败（响应结构不兼容）";
        r.protocol_error = true;
        return r;
    }
    if (!root->IsObject()) {
        r.error = "顶层不是对象（结构不兼容）";
        r.protocol_error = true;
        return r;
    }
    const JsonValue& infos = root->Find("balance_infos");
    if (!infos.IsArray()) {
        r.error = "缺少 balance_infos（结构不兼容）";
        r.protocol_error = true;
        return r;
    }
    const JsonValue& is_av = root->Find("is_available");
    std::optional<bool> available;
    if (is_av.IsBool()) available = is_av.AsBool();

    for (const auto& info : infos.AsArray()) {
        if (!info.IsObject()) continue;
        MoneyBalance b;
        b.currency = info.Find("currency").AsString();
        if (b.currency.empty()) continue; // 未知行跳过
        b.is_available = available;
        b.observed_at_utc = now_utc;
        b.observed_mono = now_mono;
        const JsonValue& total = info.Find("total_balance");
        if (total.IsString()) {
            b.total_raw = total.AsString();
            b.has_total = ParseDecimal(b.total_raw).valid;
            if (!b.has_total) b.availability_invalid = true;
        } else {
            b.availability_invalid = true;
        }
        const JsonValue& granted = info.Find("granted_balance");
        if (granted.IsString()) {
            b.granted_raw = granted.AsString();
            b.has_granted = ParseDecimal(b.granted_raw).valid;
        }
        const JsonValue& topped = info.Find("topped_up_balance");
        if (topped.IsString()) {
            b.topped_raw = topped.AsString();
            b.has_topped = ParseDecimal(b.topped_raw).valid;
        }
        r.balances.push_back(std::move(b));
    }
    r.ok = true;
    r.completeness = infos.AsArray().empty() ? "partial" : "full";
    return r;
}

// ---------- Codex（wham/usage，仅百分比口径） ----------
// 实机样本锁定（2026-09-05, plus 账户）：响应即 wham/usage 原样字段；
// 窗口只有 used_percent/limit_window_seconds/reset_after_seconds/reset_at,无绝对额度,
// 显示剩余 = 100 - used_percent（QuotaWindow.used_percent_raw 承载,has_limit=false）。
ParseResult ParseCodexResponse(const std::string& body, int64_t now_utc, int64_t now_mono) {
    ParseResult r;
    auto root = JsonParse(body, &r.error);
    if (!root) {
        r.error = r.error.empty() ? "JSON 解析失败（响应结构不兼容）" : r.error; // 透出具体原因
        r.protocol_error = true;
        return r;
    }
    if (!root->IsObject()) {
        r.error = "顶层不是对象（结构不兼容）";
        r.protocol_error = true;
        return r;
    }
    const JsonValue& rl = root->Find("rate_limit");
    if (!rl.IsObject()) {
        r.error = "缺少 rate_limit（结构不兼容）";
        r.protocol_error = true;
        return r;
    }

    auto add_window = [&](const JsonValue& win, const char* key, const char* label) {
        QuotaWindow w;
        w.stable_key = key;
        w.label = label;
        w.period_kind = PeriodKind::Rolling;
        win.Find("limit_window_seconds").AsInt(w.duration_seconds);
        double used = 0;
        if (win.Find("used_percent").IsNumber() || win.Find("used_percent").IsString()) {
            bool has = false;
            used = CountField(win, "used_percent", has);
            if (has && used >= 0 && used <= 100) {
                w.used_percent_raw = used;
                w.remaining_percent = (int32_t)(100.0 - used + 1e-9); // 向下取整
                if (w.remaining_percent < 0) w.remaining_percent = 0;
                w.availability = Availability::Known;
            } else {
                w.availability = Availability::Unknown;
            }
        } else {
            w.availability = Availability::Unknown;
        }
        int64_t reset = 0;
        if (win.Find("reset_at").AsInt(reset) && reset > 0) {
            w.has_reset = true;
            w.reset_at_utc = reset;
        }
        w.observed_at_utc = now_utc;
        w.observed_mono = now_mono;
        if (w.availability != Availability::Unknown) r.windows.push_back(std::move(w));
    };

    // 窗口顺序：primary(5h 起) → secondary(7 天起),缺失的跳过。
    // 7 天滚动窗口与 Kimi 的每周(weekly)同义,统一标签"7d余"(用户实机拍板)。
    add_window(rl.Find("primary_window"), "codex_5h", "5h余");
    add_window(rl.Find("secondary_window"), "codex_7d", "7d余");
    if (r.windows.empty()) {
        r.error = "窗口数据不可解析（结构不兼容）";
        r.protocol_error = true;
        return r;
    }
    r.ok = true;
    r.completeness = r.windows.size() >= 2 ? "full" : "partial";
    r.error.clear();
    return r;
}

// ---------- GLM（智谱 Coding Plan,monitor/quota/limit） ----------
// 契约真机锁定（2026-09-12,探针导出样本 experiments/glm_probe/glm_response.json）：
// 信封 {code,msg,data:{limits[],level},success}。limits[] 条目按 type 分派：
//  - CREDIT_LIMIT（现网实测形态）：带绝对点数 usage=总量/currentValue=已用/remaining=剩余,
//    窗口时长由 unit+number 描述（实测 (3,5)=5h、(6,1)=1 周,nextResetTime 重置时刻互证;
//    未知组合不猜周期,仅进详情）;percentage=已用%（与计数自洽,不单独采用）;
//    nextResetTime=毫秒时间戳（ExtractResetTime 内换算）。
//  - TOKENS_LIMIT / TIME_LIMIT（2026-09 前社区样本形态,保留兼容）：TOKENS_LIMIT 只有
//    percentage（数组序 1=5h、2=周,与 cc-switch 一致）,TIME_LIMIT 为月度 MCP 调用次数。
// 百分比窗口剩余=100-已用,同 Codex 口径。
ParseResult ParseGlmResponse(const std::string& body, int64_t now_utc, int64_t now_mono) {
    ParseResult r;
    auto root = JsonParse(body, &r.error);
    if (!root) {
        r.error = "JSON 解析失败（响应结构不兼容）";
        r.protocol_error = true;
        return r;
    }
    if (!root->IsObject()) {
        r.error = "顶层不是对象（结构不兼容）";
        r.protocol_error = true;
        return r;
    }
    // 信封检查：success=false 或 code!=200 → 结构/服务状态不兼容
    const JsonValue& success = root->Find("success");
    if (success.IsBool() && !success.AsBool()) {
        r.error = "服务端返回失败（success=false）";
        r.protocol_error = true;
        return r;
    }
    const JsonValue& code = root->Find("code");
    int64_t code_val = 200;
    if (code.IsNumber() && code.AsInt(code_val) && code_val != 200) {
        r.error = "服务端返回失败（code≠200）";
        r.protocol_error = true;
        return r;
    }
    const JsonValue& data = root->Find("data");
    const JsonValue& payload = data.IsObject() ? data : *root;
    const JsonValue& limits = payload.Find("limits");
    if (!limits.IsArray()) {
        r.error = "缺少 limits（结构不兼容）";
        r.protocol_error = true;
        return r;
    }

    int legacy_token_windows = 0; // TOKENS_LIMIT（旧形态）按数组序映射 5h/周
    for (const auto& item : limits.AsArray()) {
        if (!item.IsObject()) continue;
        const std::string type = item.Find("type").AsString();

        if (type == "CREDIT_LIMIT") {
            int64_t unit = 0, number = 0;
            item.Find("unit").AsInt(unit);
            item.Find("number").AsInt(number);
            CountTriple c;
            c.limit = CountField(item, "usage", c.has_limit);
            c.used = CountField(item, "currentValue", c.has_used);
            c.remaining = CountField(item, "remaining", c.has_remaining);
            if (!c.has_limit && !c.has_used && !c.has_remaining) continue;
            QuotaWindow w;
            if (unit == 3 && number == 5) {
                w.stable_key = "glm_5h";
                w.label = "5h余";
                w.duration_seconds = 5 * 3600;
                w.period_kind = PeriodKind::Rolling;
            } else if (unit == 6 && number == 1) {
                w.stable_key = "glm_week";
                w.label = "7d余";
                w.duration_seconds = 7 * 86400;
                w.period_kind = PeriodKind::Rolling;
            } else {
                // 未映射的窗口时长：仅进详情,不猜周期
                char kbuf[32];
                sprintf_s(kbuf, "glm_credit_%zu", r.windows.size());
                w.stable_key = kbuf;
                w.label = "未知周期";
                w.period_kind = PeriodKind::Other;
            }
            w.has_limit = c.has_limit;
            w.limit = c.limit;
            w.has_used = c.has_used;
            w.used = c.used;
            ComputePercent(c, w);
            w.has_reset = false;
            w.reset_at_utc = ExtractResetTime(item, w.has_reset);
            w.observed_at_utc = now_utc;
            w.observed_mono = now_mono;
            r.windows.push_back(std::move(w));
        } else if (type == "TOKENS_LIMIT") {
            bool has = false;
            double used = CountField(item, "percentage", has);
            if (!has || used < 0 || used > 100) continue; // 百分比缺失/越界：跳过该窗口
            QuotaWindow w;
            if (legacy_token_windows == 0) {
                w.stable_key = "glm_5h";
                w.label = "5h余";
                w.duration_seconds = 5 * 3600;
            } else if (legacy_token_windows == 1) {
                w.stable_key = "glm_week";
                w.label = "7d余";
                w.duration_seconds = 7 * 86400;
            } else {
                ++legacy_token_windows;
                continue; // 第 3+ 个 token 窗口：契约未定义标签,不猜周期
            }
            ++legacy_token_windows;
            w.period_kind = PeriodKind::Rolling;
            w.used_percent_raw = used;
            w.remaining_percent = (int32_t)(100.0 - used + 1e-9); // 向下取整,同 Codex
            if (w.remaining_percent < 0) w.remaining_percent = 0;
            w.availability = Availability::Known;
            w.has_reset = false;
            w.reset_at_utc = ExtractResetTime(item, w.has_reset);
            w.observed_at_utc = now_utc;
            w.observed_mono = now_mono;
            r.windows.push_back(std::move(w));
        } else if (type == "TIME_LIMIT") {
            // 旧形态：月度 MCP 调用次数
            CountTriple c;
            c.limit = CountField(item, "usage", c.has_limit);
            c.used = CountField(item, "currentValue", c.has_used);
            c.remaining = CountField(item, "remaining", c.has_remaining);
            if (!c.has_limit && !c.has_used && !c.has_remaining) continue;
            QuotaWindow w;
            w.stable_key = "glm_month";
            w.label = "月余";
            w.period_kind = PeriodKind::Monthly;
            w.has_limit = c.has_limit;
            w.limit = c.limit;
            w.has_used = c.has_used;
            w.used = c.used;
            ComputePercent(c, w);
            w.has_reset = false;
            w.reset_at_utc = ExtractResetTime(item, w.has_reset);
            w.observed_at_utc = now_utc;
            w.observed_mono = now_mono;
            r.windows.push_back(std::move(w));
        }
        // 未知 type：跳过（前向兼容,不整体失败）
    }

    if (r.windows.empty()) {
        r.error = "窗口数据不可解析（结构不兼容）";
        r.protocol_error = true;
        return r;
    }
    bool have_5h = false, have_week = false;
    for (const auto& w : r.windows) {
        if (w.stable_key == "glm_5h") have_5h = true;
        if (w.stable_key == "glm_week") have_week = true;
    }
    r.ok = true;
    // 完整 = 5h+周两个窗口齐备
    r.completeness = have_5h && have_week ? "full" : "partial";
    r.error.clear();
    return r;
}

// ---------- Antigravity（本机 hub RetrieveUserQuotaSummary,比例口径） ----------
// 真机样本 experiments/ag_probe/ag_response.json（2026-11-27）。要点:
//  - 信封 {response:{groups[]}};兼容顶层 groups / 顶层 buckets（未分组形态）。
//  - 桶按 bucketId 分派,不看数组序（真机组内 weekly 排在 5h 之前）:
//      gemini-5h→ag_5h   gemini-weekly→ag_week     Gemini 模型池（常驻条/圆环/卡片主窗口）
//      3p-5h→ag_3p_5h    3p-weekly→ag_3p_week      Claude&GPT 模型池（仅详情注解）
//    其余桶（社区报道的 argon-*/carbon-* 独立池等）不猜语义:按 window 归周期,仅进详情。
//  - remainingFraction ∈ [0,1] = 剩余比例。proto3 零值省略:额度耗尽时字段整个消失,
//    以 disabled=true 或未过期的 resetTime 佐证为 0%;二者皆无 = 无数据,跳过该桶。
//  - resetTime 只在桶有消耗时出现（剩余 1.0 的桶没有）→ 缺失即「到期未知」。
ParseResult ParseAntigravityResponse(const std::string& body, int64_t now_utc, int64_t now_mono) {
    ParseResult r;
    auto root = JsonParse(body, &r.error);
    if (!root) {
        r.error = "JSON 解析失败（响应结构不兼容）";
        r.protocol_error = true;
        return r;
    }
    if (!root->IsObject()) {
        r.error = "顶层不是对象（结构不兼容）";
        r.protocol_error = true;
        return r;
    }
    const JsonValue& env = root->Find("response");
    const JsonValue& payload = env.IsObject() ? env : *root;
    const JsonValue& groups = payload.Find("groups");
    const JsonValue& loose = payload.Find("buckets");
    if (!groups.IsArray() && !loose.IsArray()) {
        r.error = "缺少 groups/buckets（结构不兼容）";
        r.protocol_error = true;
        return r;
    }

    int others = 0; // 未映射池序号（stable_key 只需在本快照内稳定）
    auto add_bucket = [&](const JsonValue& b) {
        if (!b.IsObject()) return;
        double frac = -1;
        const JsonValue& rf = b.Find("remainingFraction");
        if (rf.IsNumber()) {
            frac = rf.AsDouble();
        } else if (b.Find("disabled").AsBool()) {
            frac = 0; // 被停用的桶 = 无额度
        } else {
            bool has_reset = false;
            int64_t reset = ExtractResetTime(b, has_reset);
            if (has_reset && reset > now_utc) frac = 0; // 零值省略 + 重置时刻未到 = 已耗尽
        }
        if (!(frac >= 0) || !std::isfinite(frac)) return; // 无数据/非法：跳过该桶
        if (frac > 1) frac = 1;

        const std::string id = b.Find("bucketId").AsString();
        const std::string win = b.Find("window").AsString(); // "5h" | "weekly"
        QuotaWindow w;
        w.period_kind = PeriodKind::Rolling;
        if (id == "gemini-5h") {
            w.stable_key = "ag_5h";
            w.label = "5h余";
            w.duration_seconds = 5 * 3600;
        } else if (id == "gemini-weekly") {
            w.stable_key = "ag_week";
            w.label = "7d余";
            w.duration_seconds = 7 * 86400;
        } else if (id == "3p-5h") {
            w.stable_key = "ag_3p_5h";
            w.label = "5h余";
            w.duration_seconds = 5 * 3600;
        } else if (id == "3p-weekly") {
            w.stable_key = "ag_3p_week";
            w.label = "7d余";
            w.duration_seconds = 7 * 86400;
        } else {
            char kbuf[32];
            sprintf_s(kbuf, "ag_other_%d", others++);
            w.stable_key = kbuf;
            if (win == "5h") {
                w.label = "5h余";
                w.duration_seconds = 5 * 3600;
            } else if (win == "weekly" || win == "7d") {
                w.label = "7d余";
                w.duration_seconds = 7 * 86400;
            } else {
                w.label = "未知周期";
                w.period_kind = PeriodKind::Other;
            }
        }
        // 比例口径（同 Codex wham）:服务端只给剩余比例,无绝对额度
        w.used_percent_raw = (1.0 - frac) * 100.0;
        const int pct = (int)std::floor(frac * 100.0 + 1e-6); // 向下取整
        w.remaining_percent = std::min(pct, 100);
        w.tiny_positive = frac > 0 && pct == 0;
        w.availability = Availability::Known;
        w.has_reset = false;
        w.reset_at_utc = ExtractResetTime(b, w.has_reset);
        w.observed_at_utc = now_utc;
        w.observed_mono = now_mono;
        r.windows.push_back(std::move(w));
    };
    if (groups.IsArray()) {
        for (const auto& g : groups.AsArray()) {
            if (!g.IsObject()) continue;
            const JsonValue& buckets = g.Find("buckets");
            if (!buckets.IsArray()) continue;
            for (const auto& b : buckets.AsArray()) add_bucket(b);
        }
    } else {
        for (const auto& b : loose.AsArray()) add_bucket(b);
    }

    if (r.windows.empty()) {
        r.error = "窗口数据不可解析（结构不兼容）";
        r.protocol_error = true;
        return r;
    }
    bool have_5h = false, have_week = false;
    for (const auto& w : r.windows) {
        if (w.stable_key == "ag_5h") have_5h = true;
        if (w.stable_key == "ag_week") have_week = true;
    }
    r.ok = true;
    // 完整 = Gemini 池 5h+周两个窗口齐备
    r.completeness = have_5h && have_week ? "full" : "partial";
    r.error.clear();
    return r;
}

// ---------- 测试模式合成数据 ----------
ProviderSnapshot MakeKimiTestSnapshot(int64_t now_utc) {
    ProviderSnapshot s;
    s.provider_id = kProviderKimi;
    s.status = FetchStatus::Ok;
    s.last_success_at_utc = now_utc;
    s.contract_revision = kKimiContract;
    s.completeness = "full";
    QuotaWindow weekly;
    weekly.stable_key = "kimi_weekly";
    weekly.label = "7d余";
    weekly.period_kind = PeriodKind::Weekly;
    weekly.has_limit = true; weekly.limit = 1000;
    weekly.has_remaining = true; weekly.remaining = 620;
    ComputePercent(CountTriple{true, false, true, 1000, 0, 620}, weekly);
    weekly.has_reset = true; weekly.reset_at_utc = now_utc + 3 * 86400;
    weekly.observed_at_utc = now_utc;
    QuotaWindow five_h;
    five_h.stable_key = "kimi_rolling_18000s";
    five_h.label = "5h余";
    five_h.period_kind = PeriodKind::Rolling;
    five_h.duration_seconds = 18000;
    five_h.has_limit = true; five_h.limit = 100;
    five_h.has_remaining = true; five_h.remaining = 81;
    ComputePercent(CountTriple{true, false, true, 100, 0, 81}, five_h);
    five_h.has_reset = true; five_h.reset_at_utc = now_utc + 2 * 3600;
    five_h.observed_at_utc = now_utc;
    s.windows = {weekly, five_h};
    return s;
}
ProviderSnapshot MakeCodexTestSnapshot(int64_t now_utc) {
    ProviderSnapshot s;
    s.provider_id = kProviderCodex;
    s.status = FetchStatus::Ok;
    s.last_success_at_utc = now_utc;
    s.contract_revision = kCodexContract;
    s.completeness = "full";
    QuotaWindow five_h;
    five_h.stable_key = "codex_5h";
    five_h.label = "5h余";
    five_h.period_kind = PeriodKind::Rolling;
    five_h.duration_seconds = 18000;
    five_h.used_percent_raw = 30;
    five_h.remaining_percent = 70;
    five_h.availability = Availability::Known;
    five_h.has_reset = true; five_h.reset_at_utc = now_utc + 2 * 3600;
    five_h.observed_at_utc = now_utc;
    QuotaWindow seven_d;
    seven_d.stable_key = "codex_7d";
    seven_d.label = "7d余"; // 与 Kimi 周窗口同义,统一标签
    seven_d.period_kind = PeriodKind::Rolling;
    seven_d.duration_seconds = 604800;
    seven_d.used_percent_raw = 55;
    seven_d.remaining_percent = 45;
    seven_d.availability = Availability::Known;
    seven_d.has_reset = true; seven_d.reset_at_utc = now_utc + 5 * 86400;
    seven_d.observed_at_utc = now_utc;
    s.windows = {five_h, seven_d};
    return s;
}
ProviderSnapshot MakeGlmTestSnapshot(int64_t now_utc) {
    ProviderSnapshot s;
    s.provider_id = kProviderGlm;
    s.status = FetchStatus::Ok;
    s.last_success_at_utc = now_utc;
    s.contract_revision = kGlmContract;
    s.completeness = "full";
    QuotaWindow five_h;
    five_h.stable_key = "glm_5h";
    five_h.label = "5h余";
    five_h.period_kind = PeriodKind::Rolling;
    five_h.duration_seconds = 18000;
    five_h.has_limit = true; five_h.limit = 100;
    five_h.has_used = true; five_h.used = 30;
    five_h.has_remaining = true; five_h.remaining = 70;
    ComputePercent(CountTriple{true, true, true, 100, 30, 70}, five_h);
    five_h.has_reset = true; five_h.reset_at_utc = now_utc + 2 * 3600;
    five_h.observed_at_utc = now_utc;
    QuotaWindow week;
    week.stable_key = "glm_week";
    week.label = "7d余";
    week.period_kind = PeriodKind::Rolling;
    week.duration_seconds = 604800;
    week.has_limit = true; week.limit = 100;
    week.has_used = true; week.used = 55;
    week.has_remaining = true; week.remaining = 45;
    ComputePercent(CountTriple{true, true, true, 100, 55, 45}, week);
    week.has_reset = true; week.reset_at_utc = now_utc + 5 * 86400;
    week.observed_at_utc = now_utc;
    QuotaWindow month;
    month.stable_key = "glm_month";
    month.label = "月余";
    month.period_kind = PeriodKind::Monthly;
    month.has_limit = true; month.limit = 1000;
    month.has_used = true; month.used = 280;
    month.has_remaining = true; month.remaining = 720;
    ComputePercent(CountTriple{true, true, true, 1000, 280, 720}, month);
    month.observed_at_utc = now_utc;
    s.windows = {five_h, week, month};
    return s;
}
ProviderSnapshot MakeDeepSeekTestSnapshot(int64_t now_utc) {
    ProviderSnapshot s;
    s.provider_id = kProviderDeepSeek;
    s.status = FetchStatus::Ok;
    s.last_success_at_utc = now_utc;
    s.contract_revision = kDeepSeekContract;
    s.completeness = "full";
    MoneyBalance b;
    b.currency = "CNY";
    b.has_total = true; b.total_raw = "23.48";
    b.has_granted = true; b.granted_raw = "10.00";
    b.has_topped = true; b.topped_raw = "13.48";
    b.is_available = true;
    b.observed_at_utc = now_utc;
    s.balances = {b};
    return s;
}
ProviderSnapshot MakeAntigravityTestSnapshot(int64_t now_utc) {
    // 形态对齐真机样本（Gemini 池 5h/周 + Claude&GPT 池 5h/周,比例口径）
    ProviderSnapshot s;
    s.provider_id = kProviderAntigravity;
    s.status = FetchStatus::Ok;
    s.last_success_at_utc = now_utc;
    s.contract_revision = kAntigravityContract;
    s.completeness = "full";
    auto add = [&](const char* key, const char* label, int64_t duration, double frac,
                   bool with_reset, int64_t reset_in) {
        QuotaWindow w;
        w.stable_key = key;
        w.label = label;
        w.period_kind = PeriodKind::Rolling;
        w.duration_seconds = duration;
        w.used_percent_raw = (1.0 - frac) * 100.0;
        w.remaining_percent = (int32_t)std::floor(frac * 100.0 + 1e-6);
        w.availability = Availability::Known;
        if (with_reset) {
            w.has_reset = true;
            w.reset_at_utc = now_utc + reset_in;
        }
        w.observed_at_utc = now_utc;
        s.windows.push_back(std::move(w));
    };
    add("ag_week", "7d余", 7 * 86400, 0.86, true, 5 * 86400); // 服务端 weekly 在前
    add("ag_5h", "5h余", 5 * 3600, 0.48, true, 2 * 3600);
    add("ag_3p_week", "7d余", 7 * 86400, 1.0, false, 0);     // 未动过的桶无 resetTime
    add("ag_3p_5h", "5h余", 5 * 3600, 1.0, false, 0);
    return s;
}

} // namespace qb
