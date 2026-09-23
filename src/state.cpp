#include "state.hpp"
#include "config.hpp"
#include "providers.hpp"
#include "util.hpp"
#include "json.hpp"
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <ctime>
#include <utility>

namespace qb {

// ---------- 序列化 ----------
namespace {
const char* PeriodKindName(PeriodKind k) {
    switch (k) {
        case PeriodKind::Monthly: return "monthly";
        case PeriodKind::Weekly: return "weekly";
        case PeriodKind::Rolling: return "rolling";
        case PeriodKind::Other: return "other";
        default: return "unknown";
    }
}
PeriodKind PeriodKindFrom(const std::string& s) {
    if (s == "monthly") return PeriodKind::Monthly;
    if (s == "weekly") return PeriodKind::Weekly;
    if (s == "rolling") return PeriodKind::Rolling;
    if (s == "other") return PeriodKind::Other;
    return PeriodKind::Unknown;
}
const char* AvailabilityName(Availability a) {
    switch (a) {
        case Availability::Known: return "known";
        case Availability::Invalid: return "invalid";
        default: return "unknown";
    }
}
Availability AvailabilityFrom(const std::string& s) {
    if (s == "known") return Availability::Known;
    if (s == "invalid") return Availability::Invalid;
    return Availability::Unknown;
}
const char* StatusName(FetchStatus s) {
    switch (s) {
        case FetchStatus::Unconfigured: return "unconfigured";
        case FetchStatus::Fetching: return "fetching";
        case FetchStatus::Ok: return "ok";
        case FetchStatus::Offline: return "offline";
        case FetchStatus::AuthError: return "auth_error";
        case FetchStatus::QueryRateLimited: return "query_rate_limited";
        case FetchStatus::ProtocolError: return "protocol_error";
        case FetchStatus::RequestError: return "request_error";
        case FetchStatus::Paused: return "paused";
    }
    return "unknown";
}
FetchStatus StatusFrom(const std::string& s) {
    if (s == "unconfigured") return FetchStatus::Unconfigured;
    if (s == "fetching") return FetchStatus::Fetching;
    if (s == "ok") return FetchStatus::Ok;
    if (s == "offline") return FetchStatus::Offline;
    if (s == "auth_error") return FetchStatus::AuthError;
    if (s == "query_rate_limited") return FetchStatus::QueryRateLimited;
    if (s == "protocol_error") return FetchStatus::ProtocolError;
    if (s == "request_error") return FetchStatus::RequestError;
    if (s == "paused") return FetchStatus::Paused;
    return FetchStatus::RequestError;
}

// 可选 double 字段序列化
void PutNum(JsonObject& o, const char* key, bool has, double v) {
    if (!has) return;
    o.emplace_back(key, JsonValue::MakeNumber(I64((int64_t)v)));
}
} // namespace

std::string SnapshotJson::Dump(const ProviderSnapshot& s) {
    JsonValue root = JsonValue::MakeObject();
    JsonObject& o = root.MutObject();
    o.emplace_back("provider_id", JsonValue::MakeString(s.provider_id));
    o.emplace_back("account_id", JsonValue::MakeString(s.account_id));
    o.emplace_back("credential_revision", JsonValue::MakeNumber(I64(s.credential_revision)));
    o.emplace_back("status", JsonValue::MakeString(StatusName(s.status)));
    o.emplace_back("last_attempt_at_utc", JsonValue::MakeNumber(I64(s.last_attempt_at_utc)));
    o.emplace_back("last_success_at_utc", JsonValue::MakeNumber(I64(s.last_success_at_utc)));
    o.emplace_back("contract_revision", JsonValue::MakeString(s.contract_revision));
    o.emplace_back("completeness", JsonValue::MakeString(s.completeness));
    o.emplace_back("http_status", JsonValue::MakeNumber(I64(s.http_status)));
    o.emplace_back("error_text", JsonValue::MakeString(s.error_text));

    JsonValue wins = JsonValue::MakeArray();
    for (const auto& w : s.windows) {
        JsonValue jw = JsonValue::MakeObject();
        JsonObject& wo = jw.MutObject();
        wo.emplace_back("stable_key", JsonValue::MakeString(w.stable_key));
        wo.emplace_back("label", JsonValue::MakeString(w.label));
        wo.emplace_back("period_kind", JsonValue::MakeString(PeriodKindName(w.period_kind)));
        wo.emplace_back("duration_seconds", JsonValue::MakeNumber(I64(w.duration_seconds)));
        PutNum(wo, "limit", w.has_limit, w.limit);
        PutNum(wo, "used", w.has_used, w.used);
        PutNum(wo, "remaining", w.has_remaining, w.remaining);
        wo.emplace_back("remaining_percent", JsonValue::MakeNumber(I64(w.remaining_percent)));
        if (w.used_percent_raw >= 0)
            wo.emplace_back("used_percent_raw", JsonValue::MakeNumber(I64((int64_t)w.used_percent_raw)));
        wo.emplace_back("tiny_positive", JsonValue::MakeBool(w.tiny_positive));
        wo.emplace_back("has_reset", JsonValue::MakeBool(w.has_reset));
        wo.emplace_back("reset_at_utc", JsonValue::MakeNumber(I64(w.reset_at_utc)));
        wo.emplace_back("observed_at_utc", JsonValue::MakeNumber(I64(w.observed_at_utc)));
        wo.emplace_back("availability", JsonValue::MakeString(AvailabilityName(w.availability)));
        wins.MutArray().push_back(std::move(jw));
    }
    o.emplace_back("windows", std::move(wins));

    JsonValue bals = JsonValue::MakeArray();
    for (const auto& b : s.balances) {
        JsonValue jb = JsonValue::MakeObject();
        JsonObject& bo = jb.MutObject();
        bo.emplace_back("currency", JsonValue::MakeString(b.currency));
        bo.emplace_back("has_total", JsonValue::MakeBool(b.has_total));
        bo.emplace_back("total_raw", JsonValue::MakeString(b.total_raw));
        bo.emplace_back("has_granted", JsonValue::MakeBool(b.has_granted));
        bo.emplace_back("granted_raw", JsonValue::MakeString(b.granted_raw));
        bo.emplace_back("has_topped", JsonValue::MakeBool(b.has_topped));
        bo.emplace_back("topped_raw", JsonValue::MakeString(b.topped_raw));
        if (b.is_available.has_value())
            bo.emplace_back("is_available", JsonValue::MakeBool(*b.is_available));
        bo.emplace_back("availability_invalid", JsonValue::MakeBool(b.availability_invalid));
        bo.emplace_back("observed_at_utc", JsonValue::MakeNumber(I64(b.observed_at_utc)));
        bals.MutArray().push_back(std::move(jb));
    }
    o.emplace_back("balances", std::move(bals));
    return JsonDump(root);
}

bool SnapshotJson::Parse(const std::string& text, ProviderSnapshot& out) {
    auto root = JsonParse(text);
    if (!root || !root->IsObject()) return false;
    const JsonValue& v = *root;
    out.provider_id = v.Find("provider_id").AsString();
    out.account_id = v.Find("account_id").AsString();
    v.Find("credential_revision").AsInt(out.credential_revision);
    out.status = StatusFrom(v.Find("status").AsString());
    v.Find("last_attempt_at_utc").AsInt(out.last_attempt_at_utc);
    v.Find("last_success_at_utc").AsInt(out.last_success_at_utc);
    out.contract_revision = v.Find("contract_revision").AsString();
    out.completeness = v.Find("completeness").AsString();
    v.Find("http_status").AsInt(out.http_status);
    out.error_text = v.Find("error_text").AsString();
    for (const auto& jw : v.Find("windows").AsArray()) {
        QuotaWindow w;
        w.stable_key = jw.Find("stable_key").AsString();
        w.label = jw.Find("label").AsString();
        w.period_kind = PeriodKindFrom(jw.Find("period_kind").AsString());
        jw.Find("duration_seconds").AsInt(w.duration_seconds);
        w.limit = jw.Find("limit").AsDouble();
        w.has_limit = jw.Find("limit").IsNumber();
        w.used = jw.Find("used").AsDouble();
        w.has_used = jw.Find("used").IsNumber();
        w.remaining = jw.Find("remaining").AsDouble();
        w.has_remaining = jw.Find("remaining").IsNumber();
        int64_t pct = -1;
        if (jw.Find("remaining_percent").AsInt(pct)) w.remaining_percent = (int32_t)pct;
        // AsDouble 按值返回：键缺失返回默认值（旧缓存无此键 → -1）
        w.used_percent_raw = jw.Find("used_percent_raw").AsDouble(-1.0);
        w.tiny_positive = jw.Find("tiny_positive").AsBool();
        w.has_reset = jw.Find("has_reset").AsBool();
        jw.Find("reset_at_utc").AsInt(w.reset_at_utc);
        jw.Find("observed_at_utc").AsInt(w.observed_at_utc);
        w.availability = AvailabilityFrom(jw.Find("availability").AsString());
        out.windows.push_back(std::move(w));
    }
    for (const auto& jb : v.Find("balances").AsArray()) {
        MoneyBalance b;
        b.currency = jb.Find("currency").AsString();
        b.has_total = jb.Find("has_total").AsBool();
        b.total_raw = jb.Find("total_raw").AsString();
        b.total = jb.Find("total_raw").AsDouble();
        b.has_granted = jb.Find("has_granted").AsBool();
        b.granted_raw = jb.Find("granted_raw").AsString();
        b.has_topped = jb.Find("has_topped").AsBool();
        b.topped_raw = jb.Find("topped_raw").AsString();
        const JsonValue& av = jb.Find("is_available");
        if (av.IsBool()) b.is_available = av.AsBool();
        b.availability_invalid = jb.Find("availability_invalid").AsBool();
        jb.Find("observed_at_utc").AsInt(b.observed_at_utc);
        out.balances.push_back(std::move(b));
    }
    return true;
}

// ---------- 新鲜度 ----------
static bool ProviderLevelStale(const ProviderSnapshot& snap, int interval_sec, int64_t now_mono) {
    if (snap.from_cache) return true; // 启动加载，尚未联网验证
    if (snap.last_success_mono <= 0) return true;
    return (now_mono - snap.last_success_mono) > 2LL * interval_sec * 1000;
}

bool IsWindowStale(const QuotaWindow& w, const ProviderSnapshot& snap, int interval_sec,
                   int64_t now_mono) {
    if (snap.status == FetchStatus::Ok && !snap.from_cache && snap.last_success_mono > 0 &&
        w.observed_mono == snap.last_success_mono) {
        return false; // 与平台级成功同批
    }
    if (snap.from_cache) return true;
    if (w.observed_mono <= 0) return true;
    return (now_mono - w.observed_mono) > 2LL * interval_sec * 1000;
}
bool IsBalanceStale(const MoneyBalance& b, const ProviderSnapshot& snap, int interval_sec,
                    int64_t now_mono) {
    (void)b;
    return ProviderLevelStale(snap, interval_sec, now_mono);
}

// ---------- 显示 ----------
namespace {
const wchar_t kStaleMark = L'\u2020'; // †
const wchar_t kPauseMark = L'\u23F8'; // ⏸

// 剩余百分比片段："62%" / "<1%" / "0%"
std::wstring PercentText(const QuotaWindow& w) {
    if (w.availability == Availability::Invalid) return L"!";
    if (w.remaining_percent < 0) return L"?";
    if (w.tiny_positive) return L"<1%";
    wchar_t buf[8];
    swprintf_s(buf, L"%d%%", w.remaining_percent);
    return buf;
}
COLORREF PickColor(const DisplayOptions& opt, COLORREF base, bool is_low, bool is_empty) {
    if (is_empty) return opt.dark_text ? RGB(200, 40, 40) : RGB(255, 92, 92);
    if (is_low) return opt.dark_text ? RGB(180, 105, 0) : RGB(255, 176, 32);
    return base;
}
bool LowPercent(const QuotaWindow& w, int threshold) {
    if (threshold < 0) return false; // 逐账户关闭（alert_pct=1）
    if (w.availability == Availability::Invalid) return true;
    if (w.tiny_positive) return true;
    if (w.remaining_percent < 0) return false;
    return w.remaining_percent <= threshold;
}

// 常驻显示查找：周精确 key；滚动窗口 stable_key 带时长后缀（如 "kimi_rolling_18000s"），
// 只取第一个已知窗口，避免数组次序/多窗口导致布局膨胀。
const QuotaWindow* FindDisplayWindow(const ProviderSnapshot& s, const std::string& prefix) {
    const QuotaWindow* best = nullptr;
    const std::string rolling = prefix + "_"; // 前缀串提出循环外，避免每次迭代堆分配
    for (const auto& w : s.windows) {
        if (w.availability == Availability::Unknown) continue;
        if (w.stable_key == prefix || StartsWith(w.stable_key, rolling)) {
            if (!best) best = &w;
        }
    }
    return best;
}

} // namespace

// 公共色/阈值入口（selftest 覆盖）：值段走开关门控，状态告警恒生效。
// 与 state.hpp 声明同域（匿名命名空间内再放一份会与外层声明重载歧义）。
COLORREF ValueColor(const DisplayOptions& opt, COLORREF base, bool is_low, bool is_empty) {
    if (!opt.low_quota_color) return base;
    return PickColor(opt, base, is_low, is_empty);
}
COLORREF WarnColor(const DisplayOptions& opt, COLORREF base) {
    return PickColor(opt, base, true, false);
}
int EffectiveLowPct(const AccountConfig& a, int global_pct) {
    if (a.alert_pct == 1) return -1;                       // 用户关闭
    if (a.alert_pct >= 5 && a.alert_pct <= 95) return a.alert_pct;
    return global_pct;                                     // 0=继承全局
}

const MoneyBalance* PrimaryBalance(const std::vector<MoneyBalance>& balances) {
    for (const auto& b : balances)
        if (b.currency == "CNY") return &b;
    return balances.empty() ? nullptr : &balances[0];
}

// 单账户显示行：首段恒为「别名 」前缀段（嵌入条按此拆名称/值），其后为值段。
// 类型决定窗口口径与文案；名称一律用别名（用户可改，空别名回落平台默认名）。
static std::vector<DisplaySegment> BuildAccountLine(const AccountView& av,
                                                    const DisplayOptions& opt,
                                                    int64_t now_mono, int interval_sec) {
    std::vector<DisplaySegment> line;
    const ProviderSnapshot& s = *av.snap;
    const AccountConfig& a = *av.cfg;
    std::wstring alias = Utf8ToWide(a.alias.empty() ? ProviderDefaultAlias(a.type) : a.alias);
    COLORREF base = opt.dark_text ? RGB(16, 16, 16) : RGB(255, 255, 255);
    auto warn = [&]() { return WarnColor(opt, base); };
    // 逐账户低额阈值（alert_pct：0=继承 1=关闭 5..95=自定义）；金额账户只认关闭
    const int low_thr = EffectiveLowPct(a, opt.low_percent_threshold);
    // 品牌色开关：账户名前缀段用平台品牌近似色（值段仍走语义色，低额警示不混色）
    const COLORREF name_color =
        opt.brand_color_names ? ProviderOpsOf(a.type).brand_color : base;
    line.push_back({alias + L" ", name_color});

    switch (a.type) {
        case AccountType::Kimi: {
            if (s.status == FetchStatus::Unconfigured) {
                line.push_back({L"待配置", base});
                break;
            }
            bool stale = false;
            if (s.status == FetchStatus::AuthError && s.last_success_at_utc == 0) {
                line.push_back({L"需更新密钥", warn()});
                break;
            }
            if (s.last_success_at_utc == 0 && s.status == FetchStatus::Fetching) {
                line.push_back({L"查询中", base});
                break;
            }
            std::vector<const QuotaWindow*> shown;
            // 显示顺序：5h 滚动窗优先 → 周
            static const char* keys[] = {"kimi_rolling", "kimi_weekly"};
            for (const char* k : keys) {
                const QuotaWindow* w = FindDisplayWindow(s, k);
                if (w) shown.push_back(w);
            }
            if (shown.empty()) {
                switch (s.status) {
                    case FetchStatus::AuthError:
                        line.push_back({L"需更新密钥", warn()});
                        break;
                    case FetchStatus::Paused:
                        line.push_back({L"已暂停", base});
                        break;
                    case FetchStatus::ProtocolError:
                        line.push_back({L"接口变化", warn()});
                        break;
                    case FetchStatus::Fetching:
                        line.push_back({L"查询中", base});
                        break;
                    default:
                        line.push_back({L"未提供", base});
                }
            } else {
                for (size_t i = 0; i < shown.size(); ++i) {
                    const QuotaWindow* w = shown[i];
                    if (i > 0) line.push_back({L" · ", base});
                    line.push_back({Utf8ToWide(w->label) + L" ", base});
                    line.push_back({PercentText(*w),
                                    ValueColor(opt, base, LowPercent(*w, low_thr),
                                              w->remaining_percent == 0 && !w->tiny_positive)});
                    if (IsWindowStale(*w, s, interval_sec, now_mono)) stale = true;
                }
            }
            if (s.status == FetchStatus::AuthError) {
                line.push_back({L" · 需更新密钥", warn()});
            } else if (s.status == FetchStatus::Paused) {
                line.push_back({std::wstring(1, kPauseMark), base});
            }
            if (stale) line.push_back({std::wstring(1, kStaleMark), base});
            break;
        }
        case AccountType::Deepseek: {
            if (s.status == FetchStatus::Unconfigured) {
                line.push_back({L"待配置", base});
                break;
            }
            if (s.status == FetchStatus::AuthError && s.last_success_at_utc == 0) {
                line.push_back({L"需更新密钥", warn()});
                break;
            }
            if (s.last_success_at_utc == 0 && s.status == FetchStatus::Fetching) {
                line.push_back({L"查询中", base});
                break;
            }
            // 主币种：CNY 优先，否则第一项
            const MoneyBalance* primary = PrimaryBalance(s.balances);
            if (!primary) {
                if (s.status == FetchStatus::AuthError)
                    line.push_back({L"需更新密钥", warn()});
                else if (s.status == FetchStatus::Paused)
                    line.push_back({L"已暂停", base});
                else
                    line.push_back({L"未提供", base});
            } else {
                Decimal d = ParseDecimal(primary->total_raw);
                std::wstring money = Utf8ToWide(FormatMoney(d, primary->currency));
                bool low = false, empty = false;
                Decimal zero{}; zero.valid = true;
                if (primary->currency == "CNY") {
                    // 阈值来自设置（CNY 口径），十进制比较；alert_pct=1 视为逐账户关闭
                    if (a.alert_pct == 1) {
                        low = false;
                    } else {
                        char thr_buf[32];
                        sprintf_s(thr_buf, "%.2f", opt.low_money_cny);
                        Decimal thr = ParseDecimal(thr_buf);
                        low = primary->has_total && CompareDecimal(d, thr) <= 0 &&
                              CompareDecimal(d, zero) > 0;
                    }
                    empty = primary->has_total && CompareDecimal(d, zero) <= 0;
                }
                line.push_back({money, ValueColor(opt, base, low, empty)});
                if (primary->is_available.has_value() && !*primary->is_available) {
                    line.push_back({L" · 不可调用", warn()});
                }
                if (s.status == FetchStatus::AuthError)
                    line.push_back({L" · 需更新密钥", warn()});
                else if (s.status == FetchStatus::Paused)
                    line.push_back({std::wstring(1, kPauseMark), base});
                else if (IsBalanceStale(*primary, s, interval_sec, now_mono))
                    line.push_back({std::wstring(1, kStaleMark), base});
            }
            break;
        }
        case AccountType::Codex: {
            // wham：仅百分比窗口，无绝对额度
            if (s.status == FetchStatus::Unconfigured) {
                line.push_back({L"待配置", base});
                break;
            }
            if (s.status == FetchStatus::AuthError && s.last_success_at_utc == 0) {
                line.push_back({L"需更新令牌", warn()});
                break;
            }
            if (s.last_success_at_utc == 0 && s.status == FetchStatus::Fetching) {
                line.push_back({L"查询中", base});
                break;
            }
            std::vector<const QuotaWindow*> shown;
            for (const auto& w : s.windows) {
                if (w.availability != Availability::Unknown && w.used_percent_raw >= 0)
                    shown.push_back(&w);
            }
            if (shown.empty()) {
                line.push_back({L"未提供", base});
            } else {
                for (size_t i = 0; i < shown.size(); ++i) {
                    const QuotaWindow* w = shown[i];
                    if (i > 0) line.push_back({L" · ", base});
                    // 百分比口径窗口：保留窗口标签（5h/7天）以免混淆
                    line.push_back({Utf8ToWide(w->label) + L" " + PercentText(*w),
                                    ValueColor(opt, base, LowPercent(*w, low_thr),
                                              w->remaining_percent == 0 && !w->tiny_positive)});
                    if (IsWindowStale(*w, s, interval_sec, now_mono))
                        line.push_back({std::wstring(1, kStaleMark), base});
                }
            }
            break;
        }
        case AccountType::Glm: {
            // 智谱 Coding Plan：5h/周 token 百分比窗口；月度 MCP 只进详情
            if (s.status == FetchStatus::Unconfigured) {
                line.push_back({L"待配置", base});
                break;
            }
            bool stale = false;
            if (s.status == FetchStatus::AuthError && s.last_success_at_utc == 0) {
                line.push_back({L"需更新密钥", warn()});
                break;
            }
            if (s.last_success_at_utc == 0 && s.status == FetchStatus::Fetching) {
                line.push_back({L"查询中", base});
                break;
            }
            std::vector<const QuotaWindow*> shown;
            // 顺序与 Kimi/Codex 一致：5h 在前、周在后
            static const char* gkeys[] = {"glm_5h", "glm_week"};
            for (const char* k : gkeys) {
                const QuotaWindow* w = FindDisplayWindow(s, k);
                if (w) shown.push_back(w);
            }
            if (shown.empty()) {
                switch (s.status) {
                    case FetchStatus::AuthError:
                        line.push_back({L"需更新密钥", warn()});
                        break;
                    case FetchStatus::Paused:
                        line.push_back({L"已暂停", base});
                        break;
                    case FetchStatus::ProtocolError:
                        line.push_back({L"接口变化", warn()});
                        break;
                    case FetchStatus::Fetching:
                        line.push_back({L"查询中", base});
                        break;
                    default:
                        line.push_back({L"未提供", base});
                }
            } else {
                for (size_t i = 0; i < shown.size(); ++i) {
                    const QuotaWindow* w = shown[i];
                    if (i > 0) line.push_back({L" · ", base});
                    // 百分比口径窗口：保留窗口标签（5h/周）以免混淆（同 Codex）
                    line.push_back({Utf8ToWide(w->label) + L" " + PercentText(*w),
                                    ValueColor(opt, base, LowPercent(*w, low_thr),
                                              w->remaining_percent == 0 && !w->tiny_positive)});
                    if (IsWindowStale(*w, s, interval_sec, now_mono)) stale = true;
                }
            }
            if (s.status == FetchStatus::AuthError) {
                line.push_back({L" · 需更新密钥", warn()});
            } else if (s.status == FetchStatus::Paused) {
                line.push_back({std::wstring(1, kPauseMark), base});
            }
            if (stale) line.push_back({std::wstring(1, kStaleMark), base});
            break;
        }
    }
    return line;
}

DisplayModel BuildDisplayModel(const std::vector<AccountView>& accounts,
                               const DisplayOptions& opt,
                               int64_t now_unix, int64_t now_mono, int interval_sec) {
    (void)now_unix;
    DisplayModel model;
    std::vector<std::vector<DisplaySegment>> lines;
    for (const auto& av : accounts) {
        if (!av.cfg || !av.snap) continue;
        lines.push_back(BuildAccountLine(av, opt, now_mono, interval_sec));
    }
    if (opt.two_lines) {
        for (auto& l : lines)
            if (!l.empty()) model.lines.push_back(std::move(l));
    } else {
        // 单行合并：行间统一加 " | "
        std::vector<DisplaySegment> one;
        COLORREF base = opt.dark_text ? RGB(16, 16, 16) : RGB(255, 255, 255);
        for (auto& l : lines) {
            if (l.empty()) continue;
            if (!one.empty()) one.push_back({L" | ", base});
            for (auto& s : l) one.push_back(std::move(s));
        }
        if (!one.empty()) model.lines.push_back(std::move(one));
    }
    return model;
}

std::vector<EmbedRowItem> BuildEmbedItems(const std::vector<AccountView>& accounts,
                                          const DisplayOptions& opt, int64_t now_unix,
                                          int64_t now_mono, int interval_sec) {
    // 两行语义：首段=别名前缀段，其余=值段（" · "压"·"、窗口标签去"余"）
    DisplayOptions o = opt;
    o.two_lines = true;
    (void)now_unix;
    std::vector<EmbedRowItem> out;
    COLORREF base = opt.dark_text ? RGB(16, 16, 16) : RGB(255, 255, 255);
    for (const auto& av : accounts) {
        if (!av.cfg || !av.snap) continue;
        const auto line = BuildAccountLine(av, o, now_mono, interval_sec);
        if (line.empty()) continue;
        EmbedRowItem it;
        it.name = Utf8ToWide(av.cfg->alias.empty() ? ProviderDefaultAlias(av.cfg->type)
                                                   : av.cfg->alias);
        it.name_color = opt.brand_color_names
                            ? ProviderOpsOf(av.cfg->type).brand_color
                            : base;
        it.type = av.cfg->type;
        for (size_t k = 1; k < line.size(); ++k) {
            DisplaySegment s2 = line[k];
            // 值行窗口分隔" · "压成"·"(段间已有 4DIP 间距):多账户并排时长行明显变短;
            // 浮窗模式空间充裕,保持" · "不动(用户拍板 2026-09-12)
            if (s2.text == L" · ") s2.text = L"·";
            if (!opt.embed_show_windows) {
                // 窗口标签关闭：纯标签段（"5h余 "）整段丢弃，合并段（"5h余 70%"）剥成"70%"
                const size_t rp = s2.text.find(L"余 ");
                if (rp != std::wstring::npos) {
                    if (rp + 2 >= s2.text.size()) continue; // 纯标签段：丢弃
                    s2.text = s2.text.substr(rp + 2);
                }
            } else if (s2.text.size() >= 2 && s2.text.back() == L' ' &&
                       s2.text[s2.text.size() - 2] == L'余') {
                // 值行标签去"余"("5h余 "→"5h ")：名称行已表明账户，腾出宽度给数字
                s2.text = s2.text.substr(0, s2.text.size() - 2) + L" ";
            } else if (s2.text.size() >= 4) {
                // 合并段形态（label+pct 一段）："5h余 43%" / "周余 89%"
                size_t pos = s2.text.find(L"余 ");
                if (pos != std::wstring::npos && pos <= 2) {
                    s2.text = s2.text.substr(0, pos) + s2.text.substr(pos + 1); // 去"余"
                }
            }
            it.values.push_back(std::move(s2));
        }
        out.push_back(std::move(it));
    }
    return out;
}

const char* FetchStatusText(FetchStatus s) {
    switch (s) {
        case FetchStatus::Unconfigured: return "未配置";
        case FetchStatus::Fetching: return "查询中";
        case FetchStatus::Ok: return "正常";
        case FetchStatus::Offline: return "网络异常";
        case FetchStatus::AuthError: return "凭证失效/权限不足";
        case FetchStatus::QueryRateLimited: return "查询受限";
        case FetchStatus::ProtocolError: return "接口不支持或已变化";
        case FetchStatus::RequestError: return "请求错误";
        case FetchStatus::Paused: return "已暂停";
    }
    return "未知";
}

const QuotaWindow* ProviderSnapshot::FindWindow(const std::string& key) const {
    for (const auto& w : windows) {
        if (w.stable_key == key) return &w;
    }
    return nullptr;
}

// ---------- 圆环模式条目 ----------
std::vector<RingItem> BuildRingItems(const std::vector<AccountView>& accounts,
                                     const DisplayOptions& opt, int64_t now_unix,
                                     int64_t now_mono, int interval_sec) {
    (void)now_unix;
    std::vector<RingItem> out;
    // 圆环是自包含小部件(overlay 垫深色圆盘),配色固定亮色系,不随壁纸文字色反转
    const COLORREF base = RGB(255, 255, 255);
    const COLORREF track = RGB(255, 255, 255);
    const COLORREF gray = RGB(150, 150, 150);
    auto pick = [&](bool low, bool empty) {
        if (!opt.low_quota_color) return RGB(255, 255, 255); // 变色关闭：环心/弧恒白
        if (empty) return RGB(255, 92, 92);
        if (low) return RGB(255, 176, 32);
        return RGB(255, 255, 255);
    };
    auto brand = [](AccountType t) { return ProviderOpsOf(t).brand_color; };

    // 百分比窗口账户:主窗口(5h)优先,其次 7d/周;状态语义与 BuildDisplayModel 对齐
    auto ring_percent = [&](const AccountView& av, const char* key1, const char* key2) {
        const ProviderSnapshot& s = *av.snap;
        const AccountConfig& a = *av.cfg;
        RingItem it;
        std::wstring alias = Utf8ToWide(a.alias.empty() ? ProviderDefaultAlias(a.type) : a.alias);
        it.badge = alias.empty() ? ProviderBadgeOf(a.type) : alias.front();
        it.type = a.type;
        it.accent = brand(a.type);
        it.value_color = base;
        it.track_color = track;
        if (s.status == FetchStatus::Unconfigured) {
            it.unconfigured = true;
            it.accent = gray;
            it.center = L"—";
            return it;
        }
        if (s.status == FetchStatus::AuthError && s.last_success_at_utc == 0) {
            it.center = L"!";
            it.low = true;
            it.value_color = pick(true, false);
            return it;
        }
        if (s.last_success_at_utc == 0 && s.status == FetchStatus::Fetching) {
            it.center = L"…";
            return it;
        }
        const QuotaWindow* w = FindDisplayWindow(s, key1);
        if (!w) w = FindDisplayWindow(s, key2);
        if (!w) {
            if (s.status == FetchStatus::AuthError) {
                it.center = L"!";
                it.low = true;
                it.value_color = pick(true, false);
            } else if (s.status == FetchStatus::Paused) {
                it.paused = true;
                it.center = L"—";
            } else {
                it.center = L"?";
            }
            return it;
        }
        it.center = PercentText(*w);
        it.arc_percent = w->tiny_positive ? 1 : std::max(0, (int)w->remaining_percent);
        it.low = LowPercent(*w, EffectiveLowPct(a, opt.low_percent_threshold));
        it.empty = w->remaining_percent == 0 && !w->tiny_positive;
        it.value_color = pick(it.low, it.empty);
        it.stale = IsWindowStale(*w, s, interval_sec, now_mono);
        if (s.status == FetchStatus::AuthError) {
            it.low = true; // 有旧值但凭证失效:琥珀提示
            it.value_color = pick(true, false);
        }
        if (s.status == FetchStatus::Paused) it.paused = true;
        return it;
    };

    for (const auto& av : accounts) {
        if (!av.cfg || !av.snap) continue;
        switch (av.cfg->type) {
            case AccountType::Kimi:
                out.push_back(ring_percent(av, "kimi_rolling", "kimi_weekly"));
                break;
            case AccountType::Codex:
                out.push_back(ring_percent(av, "codex_5h", "codex_7d"));
                break;
            case AccountType::Glm:
                out.push_back(ring_percent(av, "glm_5h", "glm_week"));
                break;
            case AccountType::Deepseek: {
                const ProviderSnapshot& s = *av.snap;
                const AccountConfig& a = *av.cfg;
                RingItem it;
                std::wstring alias =
                    Utf8ToWide(a.alias.empty() ? ProviderDefaultAlias(a.type) : a.alias);
                it.badge = alias.empty() ? ProviderBadgeOf(a.type) : alias.front();
                it.type = a.type;
                it.accent = brand(a.type);
                it.value_color = base;
                it.track_color = track;
                if (s.status == FetchStatus::Unconfigured) {
                    it.unconfigured = true;
                    it.accent = gray;
                    it.center = L"—";
                } else if (s.status == FetchStatus::AuthError &&
                           s.last_success_at_utc == 0) {
                    it.center = L"!";
                    it.low = true;
                    it.value_color = pick(true, false);
                } else if (s.last_success_at_utc == 0 &&
                           s.status == FetchStatus::Fetching) {
                    it.center = L"…";
                } else {
                    const MoneyBalance* primary = PrimaryBalance(s.balances);
                    if (!primary) {
                        if (s.status == FetchStatus::Paused) {
                            it.paused = true;
                            it.center = L"—";
                        } else {
                            it.center = L"?";
                        }
                    } else {
                        Decimal d = ParseDecimal(primary->total_raw);
                        std::wstring money = Utf8ToWide(FormatMoney(d, primary->currency));
                        if (money.size() > 5) {
                            // 环内空间有限:整数位表达("¥23"),精确值看详情/提示
                            std::wstring sym;
                            for (wchar_t ch : money) {
                                if (ch >= L'0' && ch <= L'9') break;
                                sym += ch;
                            }
                            money = sym + Utf8ToWide(DecimalToString(Rescale(d, 0), 0));
                        }
                        it.center = money;
                        it.full_ring = true; // 金额无比例语义:满环只表达状态色
                        Decimal zero{};
                        zero.valid = true;
                        if (a.alert_pct != 1) { // alert_pct=1：逐账户关闭低额判定
                            char thr_buf[32];
                            sprintf_s(thr_buf, "%.2f", opt.low_money_cny);
                            Decimal thr = ParseDecimal(thr_buf);
                            it.low = primary->has_total && CompareDecimal(d, thr) <= 0 &&
                                     CompareDecimal(d, zero) > 0;
                        }
                        it.empty = primary->has_total && CompareDecimal(d, zero) <= 0;
                        it.value_color = pick(it.low, it.empty);
                        it.stale = IsBalanceStale(*primary, s, interval_sec, now_mono);
                        if (s.status == FetchStatus::AuthError) {
                            it.low = true;
                            it.value_color = pick(true, false);
                        }
                        if (s.status == FetchStatus::Paused) it.paused = true;
                    }
                }
                out.push_back(std::move(it));
                break;
            }
        }
    }
    return out;
}

// ---------- 用量历史采样/速率常量与卡片趋势填充（A 迭代 2026-09-15） ----------
namespace {
// 保留 30 天；每序列样本封顶（防磁盘/内存膨胀；到达封顶后丢最旧）
constexpr int64_t kHistRetentionSec = 30LL * 24 * 3600;
constexpr size_t kHistMaxSamples = 2000;
// 去重：与尾样本同值且间隔 <10 分钟不重复记录（同速率下 history.json 不随查询频率膨胀）
constexpr int64_t kHistDedupSec = 10 * 60;

// 主窗口（趋势/速率口径）：与 BuildDetailCards 行序 key1/key2 一致
void PrimaryWindowKeys(AccountType t, const char** k1, const char** k2) {
    switch (t) {
        case AccountType::Codex: *k1 = "codex_5h"; *k2 = "codex_7d"; break;
        case AccountType::Glm: *k1 = "glm_5h"; *k2 = "glm_week"; break;
        default: *k1 = "kimi_rolling"; *k2 = "kimi_weekly"; break;
    }
}

// 十进制 → double（仅趋势绘图/速率拟合；金额业务口径仍走 Decimal）
double DecimalToDouble(const Decimal& d) {
    if (!d.valid) return 0.0;
    double v = (double)d.unit;
    for (int32_t i = 0; i < d.scale; ++i) v /= 10.0;
    return d.neg ? -v : v;
}

// 非负十进制减法（今日消耗用；任一 invalid → invalid）
Decimal SubNonNegDecimal(const Decimal& a, const Decimal& b) {
    if (!a.valid || !b.valid) return Decimal{};
    int32_t s = MaxScale(a, b);
    Decimal x = Rescale(a, s), y = Rescale(b, s);
    Decimal r;
    r.scale = s;
    r.valid = true;
    if (x.unit >= y.unit) {
        r.unit = x.unit - y.unit;
        r.neg = false;
    } else {
        r.unit = y.unit - x.unit;
        r.neg = true;
    }
    return r;
}

// 卡片趋势带填充（BuildDetailCards 末段调用；hist 为空 = 功能关闭态全空）
void FillCardTrend(DetailCard& c, const AccountConfig& a, int64_t now_unix,
                   const UsageHistory* hist) {
    if (!hist) return;
    const AccountHistory* h = hist->Find(a.id);
    if (!h) return;
    // 选取序列：百分比账户按主窗口 key（找不到退回任意 ≥2 样本序列）；金额账户用余额
    const WindowHistory* wh = nullptr;
    if (a.type != AccountType::Deepseek) {
        const char *k1 = nullptr, *k2 = nullptr;
        PrimaryWindowKeys(a.type, &k1, &k2);
        auto match = [&](const WindowHistory& w, const char* key) {
            return w.stable_key == key || StartsWith(w.stable_key, std::string(key) + "_");
        };
        for (const auto& w : h->windows)
            if (match(w, k1)) { wh = &w; break; }
        if (!wh)
            for (const auto& w : h->windows)
                if (match(w, k2)) { wh = &w; break; }
        if (!wh) {
            for (const auto& w : h->windows)
                if (w.samples.size() >= 2) { wh = &w; break; }
        }
    }
    const std::vector<UsageSample>* pct_series = wh ? &wh->samples : nullptr;
    const std::vector<BalanceSample>* bal_series =
        (a.type == AccountType::Deepseek && h->balance.samples.size() >= 2)
            ? &h->balance.samples
            : nullptr;
    if (!pct_series && !bal_series) return;

    // spark：最近 ≤48 个样本
    auto take_tail = [&](size_t n) { return n > 48 ? n - 48 : 0; };
    auto span_text = [](const std::vector<UsageSample>& s, size_t from) {
        if (s.empty()) return std::wstring();
        int64_t span = s.back().t_utc - s[from].t_utc;
        if (span < 60) span = 60;
        return L"近 " + Utf8ToWide(FormatRemainingZh(span, span <= 6LL * 3600));
    };
    if (pct_series && pct_series->size() >= 2) {
        c.has_trend = true;
        c.trend_is_percent = true;
        size_t from = take_tail(pct_series->size());
        for (size_t i = from; i < pct_series->size(); ++i)
            c.spark.push_back((double)pct_series->at(i).pct);
        c.trend_span = span_text(*pct_series, from);
        // ETA：有效且 ≤24h 才可操作（更长的估计噪声大于信息）
        DepletionEstimate e = EstimateDepletion(*pct_series, now_unix);
        const double pct_now = c.spark.back();
        if (e.valid && e.eta_seconds > 0 && e.eta_seconds <= 24LL * 3600 && pct_now > 0.5) {
            const bool minute_prec = e.eta_seconds <= 6LL * 3600;
            c.eta_text = L"按当前速率 " +
                         Utf8ToWide(FormatRemainingZh(e.eta_seconds, minute_prec)) +
                         L"后用尽";
        }
    } else if (bal_series) {
        c.has_trend = true;
        c.trend_is_percent = false;
        size_t from = take_tail(bal_series->size());
        for (size_t i = from; i < bal_series->size(); ++i)
            c.spark.push_back(DecimalToDouble(ParseDecimal(bal_series->at(i).total_raw)));
        // 金额序列的时间跨度（与百分比序列同口径的展示说明）
        {
            std::vector<UsageSample> proxy;
            proxy.reserve(bal_series->size());
            for (size_t i = from; i < bal_series->size(); ++i)
                proxy.push_back({bal_series->at(i).t_utc, 0});
            c.trend_span = span_text(proxy, 0);
        }
        std::string spend;
        if (TodaySpend(h->balance, now_unix, spend)) {
            Decimal d = ParseDecimal(spend);
            c.spend_text = L"今日消耗 " + Utf8ToWide(FormatMoney(d, h->balance.currency));
        }
    }
}
} // namespace

// ---------- 详情仪表盘卡片 ----------
// 与常驻显示同口径（状态/旧数据/低额语义一致），输出结构化卡片：
// 关键数字独立成行、错误信息变成可行动的引导。每账户一张卡，标题=别名。
std::vector<DetailCard> BuildDetailCards(const std::vector<AccountView>& accounts,
                                         int64_t now_unix, int interval_sec,
                                         const UsageHistory* hist) {
    std::vector<DetailCard> out;
    const int64_t now_mono = NowMono();
    // 详情卡固定口径（DisplayOptions 默认值）：低额 20% / 低余额 ¥5
    constexpr int kLowPct = 20;
    constexpr double kLowCny = 5.0;

    auto status_tone = [](FetchStatus s) {
        switch (s) {
            case FetchStatus::Ok: return DetailTone::Ok;
            case FetchStatus::Fetching: return DetailTone::Info;
            case FetchStatus::Offline:
            case FetchStatus::QueryRateLimited: return DetailTone::Warn;
            case FetchStatus::AuthError:
            case FetchStatus::ProtocolError:
            case FetchStatus::RequestError: return DetailTone::Bad;
            case FetchStatus::Unconfigured:
            case FetchStatus::Paused: return DetailTone::Muted;
        }
        return DetailTone::Muted;
    };
    auto chip_text = [](FetchStatus s) {
        std::wstring t = Utf8ToWide(FetchStatusText(s));
        if (s == FetchStatus::Fetching) t += L"…"; // 加载中文案以省略号收尾
        return t;
    };
    // 卡片带内空间有限：年份省略（"2026-09-14 01:58" → "09-14 01:58"）
    auto short_local = [](int64_t t) {
        std::wstring f = Utf8ToWide(FormatLocalShort(t)); // "YYYY-MM-DD HH:MM"
        return f.size() > 5 ? f.substr(5) : f;
    };
    auto last_update_text = [&short_local](const ProviderSnapshot& s) {
        if (s.last_success_at_utc > 0) return L"最后成功 " + short_local(s.last_success_at_utc);
        if (s.last_attempt_at_utc > 0)
            return L"最后尝试 " + short_local(s.last_attempt_at_utc) + L"（未成功）";
        return std::wstring();
    };
    auto reset_short = [now_unix](const QuotaWindow& w) {
        if (!w.has_reset || w.reset_at_utc <= 0) return std::wstring(L"到期未知");
        const int64_t left = w.reset_at_utc - now_unix;
        if (left <= 0) return std::wstring(L"已到期");
        const bool minute_prec = w.duration_seconds > 0 && w.duration_seconds <= 86400;
        return L"还有 " + Utf8ToWide(FormatRemainingZh(left, minute_prec));
    };
    // 百分比/次数窗口行：剩余比例决定环与语义色；旧数据附加标注
    auto push_window_row = [&](DetailCard& c, const QuotaWindow& w,
                               const ProviderSnapshot& s) {
        if (c.rows.size() >= 3) return;
        DetailWindowRow r;
        std::wstring label = Utf8ToWide(w.label);
        if (!label.empty() && label.back() == L'余') label.pop_back(); // "5h余"→"5h"
        if (label.empty()) label = L"窗口";
        r.label = label;
        if (w.availability == Availability::Invalid) {
            r.value_text = L"数据异常";
            r.tone = DetailTone::Bad;
        } else if (w.remaining_percent < 0) {
            r.value_text = L"未知";
            r.tone = DetailTone::Muted;
        } else {
            const bool empty = w.remaining_percent == 0 && !w.tiny_positive;
            const bool low = LowPercent(w, kLowPct);
            r.tone = empty ? DetailTone::Bad : (low ? DetailTone::Warn : DetailTone::Neutral);
            if (w.stable_key == "glm_month" && w.has_remaining) {
                char buf[40];
                sprintf_s(buf, "%.0f 次", w.remaining);
                r.value_text = Utf8ToWide(buf);
            } else {
                r.value_text = PercentText(w);
            }
            r.ring_percent = w.tiny_positive ? 1.0f : (float)w.remaining_percent;
        }
        r.reset_text = reset_short(w);
        if (IsWindowStale(w, s, interval_sec, now_mono)) r.reset_text += L" · 旧数据";
        if (s.status == FetchStatus::AuthError && r.tone == DetailTone::Neutral)
            r.tone = DetailTone::Warn; // 有旧值但凭证失效：琥珀提示（与圆环同语义）
        c.rows.push_back(std::move(r));
    };
    // 窗口行顺序与常驻显示一致：主窗口(5h/rolling)第一、周期窗第二、其余保持原序
    auto push_ordered_rows = [&](DetailCard& c, const ProviderSnapshot& s, const char* key1,
                                 const char* key2) {
        const std::string k1 = key1, k1p = std::string(key1) + "_", k2 = key2;
        auto rank = [&](const QuotaWindow& w) {
            if (w.stable_key == k1 || StartsWith(w.stable_key, k1p)) return 0;
            if (w.stable_key == k2) return 1;
            return 2;
        };
        for (int r = 0; r < 3 && c.rows.size() < 3; ++r) {
            for (const auto& w : s.windows) {
                if (rank(w) != r) continue;
                push_window_row(c, w, s);
            }
        }
    };
    auto type_subtitle = [](AccountType t) -> std::wstring {
        return ProviderOpsOf(t).subtitle; // 副题文案收敛在 providers 分派表（单点）
    };
    auto begin_card = [&](const AccountConfig& a) {
        DetailCard c;
        const ProviderOps& ops = ProviderOpsOf(a.type);
        c.badge = ops.badge;
        c.accent = ops.brand_color;
        c.title = Utf8ToWide(a.alias.empty() ? ProviderDefaultAlias(a.type) : a.alias);
        c.subtitle = type_subtitle(a.type);
        c.account_id = a.id;
        c.uses_api_key = ProviderUsesApiKey(a.type);
        c.console_url = ProviderConsoleUrl(a.type);
        return c;
    };
    auto fill_status = [&](DetailCard& c, const ProviderSnapshot& s) {
        c.status_text = chip_text(s.status);
        c.status_tone = status_tone(s.status);
        c.dimmed = s.status == FetchStatus::Paused;
    };

    for (const auto& av : accounts) {
        if (!av.cfg || !av.snap) continue;
        const AccountConfig& a = *av.cfg;
        const ProviderSnapshot& s = *av.snap;
        DetailCard c = begin_card(a);

        switch (a.type) {
            case AccountType::Kimi: {
                if (s.status == FetchStatus::Unconfigured) {
                    c.unconfigured = true;
                    c.status_text = L"未配置";
                    c.note = L"填写 Kimi Code API Key 后显示周期额度（仅 Code 额度，非会员全部）。";
                } else {
                    fill_status(c, s);
                    push_ordered_rows(c, s, "kimi_rolling", "kimi_weekly");
                    if (!s.error_text.empty()) c.note = Utf8ToWide(s.error_text);
                    else if (s.status == FetchStatus::AuthError)
                        c.note = L"凭证失效：展开「设置」更新 Key 后恢复查询。";
                    else if (s.status == FetchStatus::QueryRateLimited &&
                             s.next_allowed_mono > 0) {
                        int64_t wait = s.next_allowed_mono - now_mono;
                        if (wait > 0)
                            c.note = L"下次允许查询：" + std::to_wstring((wait + 999) / 1000) +
                                     L" 秒后";
                    } else {
                        c.note = L"会员月额度：未获取（仅显示 Code 周期额度）。";
                    }
                }
                break;
            }
            case AccountType::Codex: {
                if (s.status == FetchStatus::Unconfigured) {
                    c.unconfigured = true;
                    c.status_text = L"未配置";
                    c.note = L"先用 Codex CLI 登录（生成 ~/.codex/auth.json），本工具只读令牌。";
                } else {
                    fill_status(c, s);
                    push_ordered_rows(c, s, "codex_5h", "codex_7d");
                    if (!s.error_text.empty()) c.note = Utf8ToWide(s.error_text);
                    else if (s.status == FetchStatus::AuthError)
                        c.note = L"令牌过期：运行 Codex CLI 刷新后重试。";
                    else c.note = L"仅百分比口径（官方端点不返回绝对额度数）。";
                }
                break;
            }
            case AccountType::Deepseek: {
                if (s.status == FetchStatus::Unconfigured) {
                    c.unconfigured = true;
                    c.status_text = L"未配置";
                    c.note = L"填写 DeepSeek API Key 后显示账户余额。";
                } else {
                    fill_status(c, s);
                    const MoneyBalance* primary = PrimaryBalance(s.balances);
                    for (const auto& b : s.balances) {
                        if (c.rows.size() >= 3) break;
                        DetailWindowRow r;
                        r.label = Utf8ToWide(b.currency);
                        Decimal d = ParseDecimal(b.total_raw);
                        r.value_text = Utf8ToWide(FormatMoney(d, b.currency));
                        if (b.currency == "CNY" && b.has_total) {
                            Decimal zero{};
                            zero.valid = true;
                            char thr_buf[32];
                            sprintf_s(thr_buf, "%.2f", kLowCny);
                            Decimal thr = ParseDecimal(thr_buf);
                            const bool empty = CompareDecimal(d, zero) <= 0;
                            const bool low = !empty && CompareDecimal(d, thr) <= 0;
                            r.tone = empty ? DetailTone::Bad : (low ? DetailTone::Warn
                                                                    : DetailTone::Neutral);
                        }
                        c.rows.push_back(std::move(r));
                    }
                    std::vector<std::wstring> note_parts;
                    if (s.status == FetchStatus::AuthError) {
                        note_parts.push_back(L"凭证失效：展开「设置」更新 Key 后恢复查询。");
                    }
                    if (primary && IsBalanceStale(*primary, s, interval_sec, now_mono))
                        note_parts.push_back(L"余额数据可能已过期");
                    for (const auto& b : s.balances) {
                        if ((b.has_granted || b.has_topped) && b.currency == "CNY") {
                            note_parts.push_back(L"总额已含赠金/充值拆分，不重复相加");
                            break;
                        }
                    }
                    for (const auto& b : s.balances) {
                        if (b.is_available.has_value() && !*b.is_available) {
                            note_parts.push_back(L"当前不可调用");
                            break;
                        }
                    }
                    for (size_t i = 0; i < note_parts.size(); ++i) {
                        if (i) c.note += L"；";
                        c.note += note_parts[i];
                    }
                    if (!s.error_text.empty()) c.note = Utf8ToWide(s.error_text);
                }
                break;
            }
            case AccountType::Glm: {
                if (s.status == FetchStatus::Unconfigured) {
                    c.unconfigured = true;
                    c.status_text = L"未配置";
                    c.note = L"填写智谱 GLM API Key（bigmodel.cn）后显示 5h/周额度与月 MCP 次数。";
                } else {
                    fill_status(c, s);
                    push_ordered_rows(c, s, "glm_5h", "glm_week");
                    if (!s.error_text.empty()) c.note = Utf8ToWide(s.error_text);
                    else if (s.status == FetchStatus::AuthError)
                        c.note = L"凭证失效：展开「设置」更新 Key 后恢复查询。";
                    else c.note = L"月余为 MCP 调用次数，不计入常驻显示。";
                }
                break;
            }
        }
        c.last_update = last_update_text(s);
        FillCardTrend(c, a, now_unix, hist);
        out.push_back(std::move(c));
    }

    return out;
}

// ---------- 用量历史：公共入口 ----------
AccountHistory* UsageHistory::Find(const std::string& id) {
    for (auto& a : accounts)
        if (a.account_id == id) return &a;
    return nullptr;
}
const AccountHistory* UsageHistory::Find(const std::string& id) const {
    for (const auto& a : accounts)
        if (a.account_id == id) return &a;
    return nullptr;
}
void UsageHistory::Purge(int64_t now_utc) {
    const int64_t cutoff = now_utc - kHistRetentionSec;
    auto purge_samples = [&](auto& samples, auto&& keep_fn) {
        size_t w = 0;
        for (size_t i = 0; i < samples.size(); ++i)
            if (keep_fn(samples[i])) samples[w++] = std::move(samples[i]);
        samples.resize(w);
    };
    for (auto& acc : accounts) {
        for (auto& w : acc.windows) {
            purge_samples(w.samples, [&](const UsageSample& x) { return x.t_utc >= cutoff; });
            if (w.samples.size() > kHistMaxSamples)
                w.samples.erase(w.samples.begin(), w.samples.end() - kHistMaxSamples);
        }
        purge_samples(acc.balance.samples,
                      [&](const BalanceSample& x) { return x.t_utc >= cutoff; });
        if (acc.balance.samples.size() > kHistMaxSamples)
            acc.balance.samples.erase(acc.balance.samples.begin(),
                                      acc.balance.samples.end() - kHistMaxSamples);
    }
    accounts.erase(std::remove_if(accounts.begin(), accounts.end(),
                                  [](const AccountHistory& a) {
                                      return a.windows.empty() && a.balance.samples.empty();
                                  }),
                   accounts.end());
}

bool ApplyIdentityKey(AccountHistory& h, const std::string& key) {
    if (key.empty() || h.identity_key == key) return false;
    if (h.identity_key.empty()) {
        // 旧版磁盘段无键：首次补锚，数据保留（无键即无从比较，不误删）
        h.identity_key = key;
        return false;
    }
    h.windows.clear();
    h.balance = BalanceHistory{};
    h.identity_key = key;
    return true;
}

void RecordSnapshotSamples(AccountHistory& h, const AccountConfig& acc,
                           const ProviderSnapshot& snap, int64_t now_utc) {
    if (snap.status != FetchStatus::Ok) return;
    h.account_id = acc.id;
    auto append_pct = [&](std::vector<UsageSample>& samples, int32_t pct) {
        if (!samples.empty()) {
            const UsageSample& last = samples.back();
            if (last.pct == pct && now_utc - last.t_utc < kHistDedupSec) return;
        }
        samples.push_back({now_utc, pct});
        if (samples.size() > kHistMaxSamples)
            samples.erase(samples.begin(), samples.end() - kHistMaxSamples);
    };
    for (const auto& w : snap.windows) {
        if (w.availability != Availability::Known || w.remaining_percent < 0) continue;
        WindowHistory* target = nullptr;
        for (auto& wh : h.windows)
            if (wh.stable_key == w.stable_key) { target = &wh; break; }
        if (!target) {
            WindowHistory nh;
            nh.stable_key = w.stable_key;
            h.windows.push_back(std::move(nh));
            target = &h.windows.back();
        }
        append_pct(target->samples, w.tiny_positive ? 0 : w.remaining_percent);
    }
    if (acc.type == AccountType::Deepseek && !snap.balances.empty()) {
        const MoneyBalance* primary = PrimaryBalance(snap.balances);
        if (primary && primary->has_total && !primary->total_raw.empty()) {
            h.balance.currency = primary->currency;
            auto& samples = h.balance.samples;
            if (samples.empty() ||
                now_utc - samples.back().t_utc >= kHistDedupSec ||
                samples.back().total_raw != primary->total_raw) {
                samples.push_back({now_utc, primary->total_raw});
                if (samples.size() > kHistMaxSamples)
                    samples.erase(samples.begin(), samples.end() - kHistMaxSamples);
            }
        }
    }
}

DepletionEstimate EstimateDepletion(const std::vector<UsageSample>& samples,
                                    int64_t now_utc) {
    // 近 2h 窗口；至少 3 个样本、跨度 ≥20 分钟才拟合（过滤单次抖动）
    constexpr int64_t kSpan = 2 * 3600, kMinSpan = 20 * 60;
    std::vector<const UsageSample*> pts;
    for (const auto& s : samples)
        if (s.pct >= 0 && s.t_utc <= now_utc && s.t_utc >= now_utc - kSpan)
            pts.push_back(&s);
    if (pts.size() < 3) return {};
    const int64_t span = pts.back()->t_utc - pts.front()->t_utc;
    if (span < kMinSpan) return {};
    // 最小二乘：x=小时，y=剩余%。x 先减时间均值再累积：绝对 epoch 小时 (~5e4) 做
    // n*sxx - sx*sx 会灾难性消去，20 分钟跨度时 denom 的舍入误差可达百分之几，
    // 极端时 <=0 误判"无法估计"；中心化后斜率数学恒等、数值稳定
    const double n = (double)pts.size();
    double sx = 0, sy = 0;
    for (const auto* p : pts) {
        sx += (double)p->t_utc / 3600.0;
        sy += (double)p->pct;
    }
    const double mx = sx / n;
    double sxx = 0, sxy = 0;
    for (const auto* p : pts) {
        const double dx = (double)p->t_utc / 3600.0 - mx;
        sxx += dx * dx;
        sxy += dx * ((double)p->pct - sy / n);
    }
    const double denom = sxx;
    if (denom <= 0) return {};
    const double slope = sxy / denom; // %/小时
    // ≥-0.5%/h 视为平稳噪声；斜率为正（净恢复）不估"用尽"
    if (slope > -0.5) return {};
    const double pct_now = (double)pts.back()->pct;
    const double eta_h = pct_now / (-slope);
    DepletionEstimate out;
    out.valid = true;
    out.pct_per_hour = slope;
    out.eta_seconds = (int64_t)(eta_h * 3600.0 + 0.5);
    if (out.eta_seconds <= 0) return {};
    return out;
}

bool TodaySpend(const BalanceHistory& b, int64_t now_utc, std::string& spend_raw) {
    const auto& samples = b.samples;
    if (samples.size() < 2) return false;
    // 今日 0 点（本地时区）→ UTC 秒
    time_t tt = (time_t)now_utc;
    struct tm ltm{};
    localtime_s(&ltm, &tt);
    ltm.tm_hour = 0;
    ltm.tm_min = 0;
    ltm.tm_sec = 0;
    const int64_t midnight = (int64_t)mktime(&ltm);
    // 基线 = 今日 0 点前最后一个样本；无则取今日第一个样本
    const BalanceSample* base = nullptr;
    for (const auto& s : samples) {
        if (s.t_utc <= midnight) base = &s;
        else break;
    }
    if (!base) base = &samples.front();
    const BalanceSample& latest = samples.back();
    if (latest.t_utc <= midnight || latest.t_utc == base->t_utc) return false;
    Decimal a = ParseDecimal(base->total_raw);
    Decimal d = ParseDecimal(latest.total_raw);
    Decimal diff = SubNonNegDecimal(a, d);
    if (!diff.valid || diff.neg || diff.unit == 0) return false; // 净增加/持平不显示
    spend_raw = DecimalToString(Rescale(diff, 2), 2);
    return true;
}

bool AccountHasTrend(const UsageHistory& hist, const AccountConfig& acc,
                     const ProviderSnapshot& snap) {
    (void)snap;
    const AccountHistory* h = hist.Find(acc.id);
    if (!h) return false;
    if (acc.type == AccountType::Deepseek) return h->balance.samples.size() >= 2;
    for (const auto& w : h->windows)
        if (w.samples.size() >= 2) return true;
    return false;
}

} // namespace qb
