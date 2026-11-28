// tests.cpp — 合成自测（设计文档 §13.1 数据正确性 + 纯逻辑）。
// 运行方式：QuotaBar.exe --selftest；不访问网络、不写 %LOCALAPPDATA%\QuotaBar、
// 不保存真实账户数据。所有夹具均为构造数据。
#include "providers.hpp"
#include "scheduler.hpp"
#include "taskbar_slot.hpp"
#include "http.hpp"
#include "state.hpp"
#include "storage.hpp"
#include "theme.hpp"
#include "ui_layout.hpp"
#include "quota_layout.hpp"
#include "disp_layout.hpp"
#include "overlay.hpp" // ResolveBarDrop（拖放落位纯函数）
#include "util.hpp"
#include "json.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>

// main.cpp 提供
void SelftestPrint(const char* utf8_line);
void SelftestPrintW(const wchar_t* line);

namespace qb {

namespace {

struct T {
    int pass = 0, fail = 0;
    void check(bool cond, const char* name) {
        char buf[256];
        if (cond) {
            ++pass;
            sprintf_s(buf, "[PASS] %s\n", name);
        } else {
            ++fail;
            sprintf_s(buf, "[FAIL] %s\n", name);
        }
        SelftestPrint(buf);
    }
    void info(const char* s) {
        std::string line = "  -- ";
        line += s;
        line += "\n";
        SelftestPrint(line.c_str());
    }
};

// ---------- 测试夹具 ----------
std::string KimiBody100_38() {
    return R"({"usage":{"limit":100,"remaining":62,"resetTime":"2026-09-08T08:00:00Z"},
              "limits":[{"window":{"duration":300,"timeUnit":"TIME_UNIT_MINUTE"},
                         "detail":{"limit":100,"remaining":81,
                                   "resetTime":"2026-09-05T12:00:00Z"}}]})";
}
std::string KimiBodyOnlyUsage() {
    return R"({"usage":{"limit":50,"used":38}})";
}
std::string DsBodyDoubleCurrency() {
    return R"({"is_available":true,"balance_infos":[
        {"currency":"CNY","total_balance":"110.00","granted_balance":"10.00","topped_up_balance":"100.00"},
        {"currency":"USD","total_balance":"5.50"}]})";
}
// GLM monitor/quota/limit 真机响应（2026-09-12 探针导出,experiments/glm_probe/ 原样锁定）
std::string GlmBodyReal() {
    return R"({"code":200,"msg":"Operation successful","data":{"limits":[
        {"type":"CREDIT_LIMIT","unit":3,"number":5,"usage":12000,"currentValue":4,
         "remaining":11995,"percentage":1,"nextResetTime":1789161139635},
        {"type":"CREDIT_LIMIT","unit":6,"number":1,"usage":60000,"currentValue":4,
         "remaining":59995,"percentage":1,"nextResetTime":1789747459993}],
        "level":"pro"},"success":true})";
}
// 旧社区形态（2026-09 前样本:TOKENS_LIMIT/TIME_LIMIT）,解析器保留兼容分支
std::string GlmBodyLegacy() {
    return R"({"code":200,"msg":"操作成功","success":true,"data":{"level":"pro","limits":[
        {"type":"TIME_LIMIT","percentage":7,"usage":1000,"currentValue":72,"remaining":928},
        {"type":"TOKENS_LIMIT","percentage":44},
        {"type":"TOKENS_LIMIT","percentage":53}]}})";
}
// ParseResult → 展示用快照（详情/显示模型输入）
ProviderSnapshot SnapFromParse(const ParseResult& pr, const char* id) {
    ProviderSnapshot s;
    s.provider_id = id;
    s.account_id = id;
    s.status = FetchStatus::Ok;
    s.last_success_at_utc = NowUnix();
    s.windows = pr.windows;
    s.balances = pr.balances;
    return s;
}

// ---- 账户视图夹具：默认四账户（K/C/D/G 序，默认别名）----
std::vector<AccountConfig> FixtureAccounts() {
    AppConfig c;
    c.accounts = AppConfig::Defaults();
    return c.accounts;
}
std::vector<AccountView> ViewsOf(const std::vector<AccountConfig>& accs,
                                 const ProviderSnapshot* snaps /*4 个，同序*/) {
    std::vector<AccountView> out;
    for (size_t i = 0; i < accs.size(); ++i) out.push_back({&accs[i], &snaps[i]});
    return out;
}
ProviderSnapshot UnconfSnap(AccountType t) {
    ProviderSnapshot s;
    s.provider_id = ProviderIdOf(t);
    s.account_id = ProviderIdOf(t);
    return s;
}

// ---------- 额度计算（§6/§13.1） ----------
void TestComputePercent(T& t) {
    QuotaWindow w;
    // limit=100 used=38 → 剩余 62%，方向不能反转
    ComputePercent(CountTriple{true, true, false, 100, 38, 0}, w);
    t.check(w.availability == Availability::Known && w.remaining_percent == 62,
            "percent: limit100 used38 → 62%");
    // 只给 remaining+limit
    w = QuotaWindow{};
    ComputePercent(CountTriple{true, false, true, 100, 0, 62}, w);
    t.check(w.remaining_percent == 62 && w.availability == Availability::Known,
            "percent: 仅 remaining+limit 可计算");
    // 0 < 剩余 < 1% → tiny_positive（不可显示 0%）
    w = QuotaWindow{};
    ComputePercent(CountTriple{true, false, true, 1000, 0, 3}, w);
    t.check(w.tiny_positive && w.remaining_percent == 0, "percent: 0.3% → <1%");
    // 明确耗尽 → 0%
    w = QuotaWindow{};
    ComputePercent(CountTriple{true, true, false, 100, 100, 0}, w);
    t.check(!w.tiny_positive && w.remaining_percent == 0, "percent: 耗尽 → 0%");
    // limit=0 无业务定义 → Unknown（不当无限/耗尽）
    w = QuotaWindow{};
    ComputePercent(CountTriple{true, true, false, 0, 10, 0}, w);
    t.check(w.availability == Availability::Unknown, "percent: limit=0 → 未知");
    // 矛盾值（used 与 remaining 对不上）→ Invalid
    w = QuotaWindow{};
    ComputePercent(CountTriple{true, true, true, 100, 10, 60}, w);
    t.check(w.availability == Availability::Invalid, "percent: 矛盾值 → 异常");
    // 负 used → Invalid
    w = QuotaWindow{};
    ComputePercent(CountTriple{true, true, false, 100, -5, 0}, w);
    t.check(w.availability == Availability::Invalid, "percent: 负已用 → 异常");
    // 剩余大于上限 → Invalid（不得直接显示正常百分比）
    w = QuotaWindow{};
    ComputePercent(CountTriple{true, false, true, 100, 0, 200}, w);
    t.check(w.availability == Availability::Invalid, "percent: 剩余>上限 → 异常");
    // 使用量略超限 → 显示剩余 0（不显示负数）
    w = QuotaWindow{};
    ComputePercent(CountTriple{true, true, false, 100, 105, 0}, w);
    t.check(w.remaining_percent == 0 && w.availability == Availability::Known,
            "percent: 已用>限(余量 clamp 0)");
}

// ---------- Kimi 解析 ----------
void TestKimiParse(T& t) {
    int64_t now = NowUnix();
    auto r = ParseKimiResponse(KimiBody100_38(), now, NowMono());
    t.check(r.ok && r.windows.size() == 2, "kimi: usage+limits 解析出两个窗口");
    const QuotaWindow* wk = nullptr;
    const QuotaWindow* wr = nullptr;
    for (const auto& w : r.windows) {
        if (w.stable_key == "kimi_weekly") wk = &w;
        if (w.stable_key == "kimi_rolling_18000s") wr = &w;
    }
    t.check(wk && wk->label == "7d余" && wk->period_kind == PeriodKind::Weekly &&
                wk->remaining_percent == 62,
            "kimi: 顶层 usage → 7d余 62%");
    t.check(wk && wk->has_reset, "kimi: 重置时间已解析");
    t.check(wr && wr->label == "5h余" && wr->duration_seconds == 18000 &&
                wr->remaining_percent == 81,
            "kimi: limits[].detail → 5h余 81%");
    t.check(r.completeness == std::string("full"), "kimi: completeness=full");

    // 只有周额度：5h 与月池均不复制/不猜测
    auto r2 = ParseKimiResponse(KimiBodyOnlyUsage(), now, NowMono());
    t.check(r2.ok && r2.windows.size() == 1 &&
                r2.windows[0].stable_key == "kimi_weekly",
            "kimi: 仅 usage → 只返回周窗口");
    t.check(r2.completeness == std::string("partial"), "kimi: completeness=partial");

    // 缺计数/非法值：窗口 Unknown/Invalid 而非崩溃
    auto r3 = ParseKimiResponse(R"({"usage":{"resetTime":"2026-09-08T08:00:00Z"}})", now,
                                NowMono());
    t.check(r3.ok && r3.windows.empty(), "kimi: 无计数 → 空窗口（未知）");
    auto r4 = ParseKimiResponse(
        R"({"usage":{"limit":0,"used":10},"limits":[{"window":{"duration":60,"timeUnit":"TIME_UNIT_MINUTE"},"detail":{"limit":100,"remaining":-1}}]})",
        now, NowMono());
    t.check(r4.ok && r4.windows.size() == 2, "kimi: limit=0/负剩余 不使解析失败");

    // 重置时间过去：不自动恢复 100%（本地不做"到期重算"）
    auto r5 = ParseKimiResponse(
        R"({"usage":{"limit":100,"remaining":62,"resetTime":"2020-01-01T00:00:00Z"}})",
        now, NowMono());
    t.check(r5.ok && r5.windows[0].remaining_percent == 62 &&
                r5.windows[0].has_reset,
            "kimi: 重置已过 → 显示原剩余，不自动变 100%");

    // 未识别的新结构 → 未知周期，不自动套用周/月
    auto r6 = ParseKimiResponse(
        R"({"limits":[{"window":{"duration":7,"timeUnit":"TIME_UNIT_FORTNIGHT"},
                        "detail":{"limit":10,"remaining":3}}]})",
        now, NowMono());
    t.check(r6.ok && !r6.windows.empty() &&
                r6.windows[0].period_kind == PeriodKind::Other &&
                r6.windows[0].label == "未知周期",
            "kimi: 未知时间单位 → 未知周期");

    // 纳秒精度 ISO 8601
    auto ts = ParseIso8601Utc("2026-09-08T08:00:00.123456789Z");
    t.check(ts.has_value() && *ts == ParseIso8601Utc("2026-09-08T08:00:00Z").value_or(0),
            "time: 纳秒精度截断到秒");
    t.check(!ParseIso8601Utc("2026-13-99T00:00:00Z").has_value() &&
                !ParseIso8601Utc("not-a-date").has_value(),
            "time: 非法时间 → nullopt（不使请求失败）");
    // 带时区偏移
    auto tz = ParseIso8601Utc("2026-09-08T16:00:00+08:00");
    t.check(tz.has_value() && *tz == ParseIso8601Utc("2026-09-08T08:00:00Z").value_or(-1),
            "time: +08:00 偏移换算正确");
}

// ---------- DeepSeek ----------
void TestDeepSeekParse(T& t) {
    int64_t now = NowUnix();
    auto r = ParseDeepSeekResponse(DsBodyDoubleCurrency(), now, NowMono());
    t.check(r.ok && r.balances.size() == 2, "ds: 双币种分别列出");
    t.check(r.balances[0].currency == "CNY" && r.balances[1].currency == "USD",
            "ds: 币种保序且不合并");
    // 总额 110 已含赠金 10+充值 100：卡片只出现 110，不出现相加结果 220
    ProviderSnapshot ds_snap = SnapFromParse(r, kProviderDeepSeek);
    auto accs_ds = FixtureAccounts();
    accs_ds[2].type = AccountType::Deepseek; // 卡片序：K/C/D/G → ds 在 [2]
    auto cards_ds = BuildDetailCards({{&accs_ds[2], &ds_snap}}, now, 300);
    std::wstring ds_vals;
    for (const auto& cd : cards_ds)
        for (const auto& rw : cd.rows) ds_vals += rw.value_text + L"|";
    t.check(ds_vals.find(L"¥110.00") != std::wstring::npos &&
                ds_vals.find(L"220") == std::wstring::npos,
            "ds: 总额 110，不把赠金/充值再加一遍");
    // is_available=false：金额保留且标注不可调用，不解析成密钥错误
    auto r2 = ParseDeepSeekResponse(
        R"({"is_available":false,"balance_infos":[{"currency":"CNY","total_balance":"8.00"}]})",
        now, NowMono());
    t.check(r2.ok && r2.balances[0].has_total &&
                r2.balances[0].is_available.has_value() && !*r2.balances[0].is_available,
            "ds: is_available=false 与金额独立保存");
    ProviderSnapshot ds2 = SnapFromParse(r2, kProviderDeepSeek);
    auto cards_ds2 = BuildDetailCards({{&accs_ds[2], &ds2}}, now, 300);
    std::wstring ds2_vals;
    for (const auto& cd : cards_ds2)
        for (const auto& rw : cd.rows) ds2_vals += rw.value_text + L"|";
    std::wstring ds2_note;
    for (const auto& cd : cards_ds2) ds2_note += cd.note + L"|";
    t.check(ds2_vals.find(L"¥8.00") != std::wstring::npos &&
                ds2_note.find(L"不可调用") != std::wstring::npos,
            "ds: 卡片保留金额并标注不可调用");
    // 非法金额（指数/超长）
    auto r3 = ParseDeepSeekResponse(
        R"({"balance_infos":[{"currency":"CNY","total_balance":"1e5"},
                              {"currency":"USD","total_balance":"99999999999999999999999999"}]})",
        now, NowMono());
    t.check(r3.ok && r3.balances.size() == 2 && !r3.balances[0].has_total &&
                r3.balances[0].availability_invalid && !r3.balances[1].has_total,
            "ds: 非法金额 → 标记不可用，不崩溃");
    // 空数组 → ok=partial
    auto r4 = ParseDeepSeekResponse(R"({"is_available":true,"balance_infos":[]})", now,
                                    NowMono());
    t.check(r4.ok && r4.completeness == std::string("partial") && r4.balances.empty(),
            "ds: 空数组 → partial");
    // 缺字段 → 结构不兼容
    auto r5 = ParseDeepSeekResponse(R"({"foo":1})", now, NowMono());
    t.check(!r5.ok && r5.protocol_error, "ds: 结构不兼容 → protocol_error");
}

// ---------- GLM 解析（monitor/quota/limit,2026-09-12 真机锁定） ----------
void TestGlmParse(T& t) {
    int64_t now = NowUnix();
    // 真机样本：两窗 CREDIT_LIMIT,带绝对点数与毫秒重置时间
    auto r = ParseGlmResponse(GlmBodyReal(), now, NowMono());
    t.check(r.ok && !r.protocol_error && r.windows.size() == 2,
            "glm: 真机样本解析两窗口");
    const QuotaWindow* h5 = nullptr;
    const QuotaWindow* week = nullptr;
    for (const auto& w : r.windows) {
        if (w.stable_key == "glm_5h") h5 = &w;
        if (w.stable_key == "glm_week") week = &w;
    }
    // (unit=3,number=5) → 5h 窗：计数 12000/4/11995 → 剩余 99%
    t.check(h5 && h5->label == "5h余" && h5->duration_seconds == 18000 &&
                h5->period_kind == PeriodKind::Rolling && h5->has_limit &&
                h5->limit == 12000 && h5->used == 4 && h5->remaining == 11995 &&
                h5->remaining_percent == 99 && h5->availability == Availability::Known,
            "glm: (3,5) → 5h余 99%（计数口径）");
    // 毫秒 nextResetTime 换算为秒
    t.check(h5 && h5->has_reset && h5->reset_at_utc == 1789161139,
            "glm: 毫秒重置时间 → 秒");
    // (unit=6,number=1) → 周窗：60000/4/59995 → 99%,重置 ≈7 天后
    t.check(week && week->label == "7d余" && week->duration_seconds == 604800 &&
                week->remaining_percent == 99 && week->limit == 60000 &&
                week->reset_at_utc == 1789747459,
            "glm: (6,1) → 7d余 99% + 重置时间");
    t.check(r.completeness == std::string("full"), "glm: 5h+周齐备 → full");

    // 旧社区形态（TOKENS_LIMIT/TIME_LIMIT）保持兼容
    auto legacy = ParseGlmResponse(GlmBodyLegacy(), now, NowMono());
    t.check(legacy.ok && legacy.windows.size() == 3, "glm: 旧形态兼容（TOKENS/TIME_LIMIT）");
    const QuotaWindow* lw = nullptr;
    const QuotaWindow* lm = nullptr;
    for (const auto& w : legacy.windows) {
        if (w.stable_key == "glm_week") lw = &w;
        if (w.stable_key == "glm_month") lm = &w;
    }
    t.check(lw && lw->used_percent_raw == 53 && lw->remaining_percent == 47 && !lw->has_limit,
            "glm: 旧形态 TOKENS_LIMIT → 7d余百分比口径");
    t.check(lm && lm->label == "月余" && lm->period_kind == PeriodKind::Monthly &&
                lm->limit == 1000 && lm->remaining_percent == 92,
            "glm: 旧形态 TIME_LIMIT → 月余（MCP 次数）");
    t.check(legacy.completeness == std::string("full"), "glm: 旧形态双 token 窗口 → full");

    // 未知窗口时长组合 → 仅详情,不猜周期,不计入 full
    auto odd = ParseGlmResponse(
        R"({"code":200,"success":true,"data":{"limits":[
              {"type":"CREDIT_LIMIT","unit":4,"number":2,"usage":100,"currentValue":0,
               "remaining":100,"percentage":0}]}})",
        now, NowMono());
    t.check(odd.ok && odd.windows.size() == 1 &&
                odd.windows[0].label == "未知周期" &&
                odd.windows[0].period_kind == PeriodKind::Other &&
                odd.completeness == std::string("partial"),
            "glm: 未知 unit/number → 未知周期 + partial");

    // 信封失败/缺 limits/全窗口越界 → protocol_error
    auto bad = ParseGlmResponse(R"({"code":401,"msg":"unauthorized","success":false,"data":{}})",
                                now, NowMono());
    t.check(!bad.ok && bad.protocol_error, "glm: success=false → 结构不兼容");
    auto nolimits = ParseGlmResponse(R"({"code":200,"success":true,"data":{}})", now, NowMono());
    t.check(!nolimits.ok && nolimits.protocol_error, "glm: 缺 limits → 结构不兼容");
    auto oob = ParseGlmResponse(
        R"({"code":200,"success":true,"data":{"limits":[{"type":"TOKENS_LIMIT","percentage":144}]}})",
        now, NowMono());
    t.check(!oob.ok && oob.protocol_error, "glm: 全窗口百分比越界 → 结构不兼容");

    // 快照往返：计数与重置时间都保留
    ProviderSnapshot snap = SnapFromParse(r, kProviderGlm);
    std::string j = SnapshotJson::Dump(snap);
    ProviderSnapshot back;
    bool rt = SnapshotJson::Parse(j, back) && back.windows.size() == 2;
    const QuotaWindow* back_week = nullptr;
    const QuotaWindow* back_5h = nullptr;
    if (rt) {
        for (const auto& w : back.windows) {
            if (w.stable_key == "glm_week") back_week = &w;
            if (w.stable_key == "glm_5h") back_5h = &w;
        }
    }
    t.check(rt && back_5h && back_5h->remaining == 11995 && back_5h->reset_at_utc == 1789161139 &&
                back_week && back_week->limit == 60000 && back_week->remaining_percent == 99,
            "glm: 快照序列化往返保留字段");
}

// ---------- JSON 有界拒绝 ----------
void TestJsonBounds(T& t) {
    std::string err;
    t.check(!JsonParse("{", &err), "json: 截断对象拒绝");
    // 嵌套超 16
    std::string deep;
    for (int i = 0; i < 20; ++i) deep += "[";
    deep += "0";
    for (int i = 0; i < 20; ++i) deep += "]";
    t.check(!JsonParse(deep, &err), "json: 深度>16 拒绝");
    t.check(JsonParse("[1,2,[3,[4]]]").get() != nullptr, "json: 浅嵌套正常");
    // 字符串超 32KiB
    std::string huge = "\"";
    huge.append(34 * 1024, 'a');
    huge += "\"";
    t.check(!JsonParse(huge, &err), "json: 34KiB 字符串拒绝");
}

// ---------- 金额十进制 ----------
void TestMoney(T& t) {
    Decimal d = ParseDecimal("123.45");
    t.check(d.valid && d.unit == 12345 && d.scale == 2, "decimal: 123.45");
    Decimal neg = ParseDecimal("-0.001");
    t.check(neg.valid && neg.neg && neg.unit == 1 && neg.scale == 3, "decimal: -0.001");
    t.check(!ParseDecimal("abc").valid && !ParseDecimal("1e5").valid &&
                !ParseDecimal("").valid,
            "decimal: 非法输入 invalid");
    // 精度不丢：0.1+0.2 语义用定点
    Decimal a = ParseDecimal("0.1"), b = ParseDecimal("0.2");
    int32_t s = MaxScale(a, b);
    int64_t sum = Rescale(a, s).unit + Rescale(b, s).unit;
    t.check(sum == 3 && s == 1, "decimal: 0.1+0.2 → 3（无浮点误差）");
    // FormatMoney：0 < 值 < 0.01 → "<¥0.01"
    Decimal tiny = ParseDecimal("0.004");
    t.check(FormatMoney(tiny, "CNY") == "<¥0.01", "money: 0.004 → <¥0.01");
    t.check(FormatMoney(ParseDecimal("23.48"), "CNY") == "¥23.48", "money: ¥23.48");
    t.check(FormatMoney(ParseDecimal("5.5"), "USD") == "$5.50", "money: $5.50");
    t.check(FormatMoney(ParseDecimal("12.3"), "EUR") == "€12.30", "money: €12.30");
    // 未知币种保留代码，不默认人民币
    t.check(FormatMoney(ParseDecimal("7"), "JPY") == "JPY 7.00", "money: 未知币种用代码");
    // 比较：0.004 为正
    Decimal zero{};
    zero.valid = true;
    t.check(CompareDecimal(tiny, zero) > 0, "money: 0.004 > 0（不误判为空）");
}

// ---------- 显示模型 ----------
void TestDisplayModel(T& t) {
    DisplayOptions opt;
    int64_t now = NowUnix(), mono = NowMono();
    auto accs = FixtureAccounts(); // K/C/D/G，默认别名

    // 未配置 → 各别名 + 「待配置」
    ProviderSnapshot snaps_unc[4] = {UnconfSnap(AccountType::Kimi),
                                     UnconfSnap(AccountType::Codex),
                                     UnconfSnap(AccountType::Deepseek),
                                     UnconfSnap(AccountType::Glm)};
    auto m0 = BuildDisplayModel(ViewsOf(accs, snaps_unc), opt, now, mono, 300);
    std::wstring flat0;
    for (const auto& line : m0.lines)
        for (const auto& sg : line) flat0 += sg.text;
    t.check(flat0.find(L"Kimi Code 待配置") != std::wstring::npos &&
                flat0.find(L"DeepSeek 待配置") != std::wstring::npos &&
                flat0.find(L"Codex 待配置") != std::wstring::npos &&
                flat0.find(L"GLM 待配置") != std::wstring::npos,
            "display: 未配置 → 别名 + 待配置");

    // 测试夹具数据 → 周余 62% + 5h余 81%（此前滚动 key 前缀 bug 的回归测试）
    ProviderSnapshot k = MakeKimiTestSnapshot(now);
    ProviderSnapshot d = MakeDeepSeekTestSnapshot(now);
    ProviderSnapshot c = MakeCodexTestSnapshot(now);
    ProviderSnapshot g = MakeGlmTestSnapshot(now);
    k.from_cache = false;
    d.from_cache = false;
    c.from_cache = false;
    g.from_cache = false;
    ProviderSnapshot snaps[4] = {k, c, d, g};
    auto m1 = BuildDisplayModel(ViewsOf(accs, snaps), opt, now, mono, 300);
    std::wstring flat;
    for (const auto& line : m1.lines)
        for (const auto& sg : line) flat += sg.text;
    t.check(flat.find(L"62%") != std::wstring::npos, "display: 7d余 62% 出现");
    t.check(flat.find(L"5h") != std::wstring::npos && flat.find(L"81%") != std::wstring::npos,
            "display: 5h余 81% 出现（rolling 前缀匹配）");
    t.check(flat.find(L"¥23.48") != std::wstring::npos, "display: DeepSeek ¥23.48 出现");
    t.check(flat.find(L"5h余 70%") != std::wstring::npos &&
                flat.find(L"7d余 45%") != std::wstring::npos,
            "display: Codex/GLM 5h/7d 百分比出现");
    t.check(flat.find(L"月余") == std::wstring::npos,
            "display: GLM 月度 MCP 不出现在常驻");

    // 多账户：同类型两张卡（别名区分）各自成行
    auto accs2 = accs;
    AccountConfig extra = accs2[0];
    extra.id = "acc9";
    extra.alias = "备用 Kimi";
    std::vector<AccountView> views2 = ViewsOf(accs2, snaps);
    views2.push_back({&extra, &k});
    auto m3 = BuildDisplayModel(views2, opt, now, mono, 300);
    t.check(m3.lines.size() == 5, "multi: 5 账户 → 5 行");
    std::wstring flat3;
    for (const auto& line : m3.lines)
        for (const auto& sg : line) flat3 += sg.text;
    t.check(flat3.find(L"备用 Kimi") != std::wstring::npos &&
                flat3.find(L"Kimi Code") != std::wstring::npos,
            "multi: 别名作为行名（同类型多卡并存）");

    // 网络异常且有旧值 → †
    auto k2 = k;
    k2.status = FetchStatus::Offline;
    k2.error_text = "网络异常";
    ProviderSnapshot snaps2[4] = {k2, c, d, g};
    auto m2 = BuildDisplayModel(ViewsOf(accs, snaps2), opt, now, mono, 300);
    std::wstring flat2;
    for (const auto& line : m2.lines)
        for (const auto& sg : line) flat2 += sg.text;
    t.check(flat2.find(L"†") != std::wstring::npos, "display: 旧值带 †");

    // ---------- 详情仪表盘卡片（BuildDetailCards） ----------
    auto cards = BuildDetailCards(ViewsOf(accs, snaps), now, 300);
    t.check(cards.size() == 4, "cards: 四账户各一卡");
    t.check(cards[0].badge == L'K' && cards[1].badge == L'C' && cards[2].badge == L'D' &&
                cards[3].badge == L'G',
            "cards: 卡序=视图序（徽章 K/C/D/G）");
    t.check(cards[0].title == L"Kimi Code" && cards[0].account_id == "kimi" &&
                cards[0].uses_api_key && !cards[1].uses_api_key,
            "cards: 标题=别名；uses_api_key 区分 codex 免密");
    t.check(!cards[0].subtitle.empty() && cards[1].subtitle.find(L"ChatGPT") !=
                                              std::wstring::npos,
            "cards: 副标题=类型说明");
    // Kimi 行序：5h 在前、周(7d)在后（与常驻显示同序）
    t.check(cards[0].rows.size() >= 2 && cards[0].rows[0].value_text == L"81%" &&
                cards[0].rows[1].value_text == L"62%",
            "cards: Kimi 5h 行先于 7d 行（值 81%/62%）");
    // Kimi 注解：月池未获取
    t.check(cards[0].note.find(L"会员月额度：未获取") != std::wstring::npos,
            "cards: 月池默认未获取（不宣称完整支持）");
    // Codex 百分比口径：环比例存在
    t.check(cards[1].rows.size() >= 2 && cards[1].rows[0].ring_percent >= 0,
            "cards: Codex 行带状态环比例");
    // DeepSeek 金额行：无比例语义（轨道环），值=FormatMoney
    t.check(cards[2].rows.size() == 1 && cards[2].rows[0].value_text == L"¥23.48" &&
                cards[2].rows[0].ring_percent < 0,
            "cards: DeepSeek 金额行 ¥23.48 无比例环");
    // 到期倒计时：5h 窗分钟粒度（夹具 reset=now+2h → "还有 2小时"）；7d 小时粒度
    t.check(cards[0].rows.size() >= 2 &&
                cards[0].rows[0].reset_text.find(L"还有 2小时") != std::wstring::npos,
            "cards: 5h 窗口到期倒计时（分钟粒度）");
    t.check(cards[0].rows[1].reset_text.find(L"还有 3天") != std::wstring::npos,
            "cards: 7d 窗口到期倒计时（小时粒度）");
    // 未配置卡：显示引导 + 控制台链接
    ProviderSnapshot uncs[4] = {UnconfSnap(AccountType::Kimi), UnconfSnap(AccountType::Codex),
                                UnconfSnap(AccountType::Deepseek),
                                UnconfSnap(AccountType::Glm)};
    auto cards_unc = BuildDetailCards(ViewsOf(accs, uncs), now, 300);
    t.check(cards_unc[0].unconfigured && cards_unc[0].console_url != nullptr,
            "cards: Kimi 未配置 → 引导 + 控制台链接");
    t.check(cards_unc[1].unconfigured && cards_unc[1].console_url != nullptr,
            "cards: Codex 未配置 → 引导（令牌由 CLI 维护）+ 用量面板链接");
    // 凭证失效：状态色 Bad + 状态文本
    auto k_bad = k;
    k_bad.status = FetchStatus::AuthError;
    ProviderSnapshot snaps_bad[4] = {k_bad, c, d, g};
    auto cards_bad = BuildDetailCards(ViewsOf(accs, snaps_bad), now, 300);
    t.check(cards_bad[0].status_tone == DetailTone::Bad &&
                cards_bad[0].status_text == Utf8ToWide(FetchStatusText(FetchStatus::AuthError)),
            "cards: 凭证失效 → Bad 状态");
    // 到期未知（无重置时间）
    auto k_noreset = k;
    for (auto& w : k_noreset.windows) w.has_reset = false;
    ProviderSnapshot snaps_nr[4] = {k_noreset, c, d, g};
    auto cards_nr = BuildDetailCards(ViewsOf(accs, snaps_nr), now, 300);
    t.check(!cards_nr[0].rows.empty() &&
                cards_nr[0].rows[0].reset_text.find(L"到期未知") != std::wstring::npos,
            "cards: 无重置时间 → 到期未知");
}

// ---------- 配置账户模型（v1 迁移 / v2 往返） ----------
void TestConfigAccounts(T& t) {
    // v1 → v2：无 accounts 键时按旧平铺字段迁移（show_* → card_visible，cred_rev_* → 账户）
    std::string v1 =
        R"({"config_version":1,"display":{"mode":3,"two_lines":true,"show_kimi":true,)"
        R"("show_deepseek":false,"show_codex":true,"show_glm":true},)"
        R"("query":{"interval_sec":300},"notify":{"enabled":false},)"
        R"("first_run_done":true,"cred_rev_kimi":3,"cred_rev_deepseek":5,"cred_rev_glm":2})";
    AppConfig c;
    t.check(ConfigIo::Load(v1, c), "config: v1 解析成功");
    t.check(c.accounts.size() == 4, "config: v1 迁移出 4 个默认账户");
    bool ok_order = c.accounts.size() == 4 &&
                    c.accounts[0].id == "kimi" && c.accounts[1].id == "codex" &&
                    c.accounts[2].id == "deepseek" && c.accounts[3].id == "glm";
    t.check(ok_order, "config: 迁移顺序=K/C/D/G");
    t.check(c.accounts[0].cred_rev == 3 && c.accounts[2].cred_rev == 5 &&
                c.accounts[3].cred_rev == 2 && c.accounts[1].cred_rev == 1,
            "config: cred_rev 平铺字段迁移到各账户（codex 恒 1）");
    t.check(c.accounts[0].card_visible && !c.accounts[2].card_visible,
            "config: show_* → card_visible");
    t.check(c.accounts[0].enabled && c.accounts[0].alias == std::string("Kimi Code"),
            "config: 迁移账户默认启用 + 默认别名");

    // v2 往返：账户增删/别名/开关在 Dump→Load 后保留
    c.account_seq = 9;
    AccountConfig a;
    a.id = "acc9";
    a.type = AccountType::Codex;
    a.alias = "某某某的 codex";
    a.enabled = false;
    a.card_visible = false;
    a.cred_rev = 1;
    c.accounts.push_back(a);
    AppConfig back;
    t.check(ConfigIo::Load(ConfigIo::Dump(c), back), "config: v2 往返解析成功");
    t.check(back.accounts.size() == 5 && back.account_seq == 9, "config: 账户列表往返保留");
    const AccountConfig* pa = back.FindAccount("acc9");
    t.check(pa && pa->alias == "某某某的 codex" && !pa->enabled && !pa->card_visible &&
                pa->type == AccountType::Codex && pa->cred_rev == 1,
            "config: 别名/开关/类型往返一致");
    AppConfig back2;
    t.check(ConfigIo::Load(ConfigIo::Dump(back), back2) &&
                back2.FindAccount("acc9") && back2.accounts.size() == 5,
            "config: 重复往返稳定（codex cred_rev 恒 1）");
}

// ---------- ui_layout 纯逻辑（2026-09-18 从 tray_ui 抽出） ----------
void TestUiLayout(T& t) {
    using ui::HitW;
    auto rc = [](int l, int tp, int r, int b) { return RECT{l, tp, r, b}; };

    // 滑杆：端点/中点/越界钳制（rc=10..210，96dpi 内缩 7 → 轨道 17..203）
    RECT slider = rc(10, 0, 210, 20);
    t.check(ui::SliderValueFrom(slider, 60, 3600, 17, 96) == 60 &&
                ui::SliderValueFrom(slider, 60, 3600, 203, 96) == 3600,
            "slider: 端点取 vmin/vmax");
    t.check(ui::SliderValueFrom(slider, 60, 3600, 110, 96) == 1830,
            "slider: 中点对半（93/186 轨道）");
    t.check(ui::SliderValueFrom(slider, -200, 200, 0, 96) == -200 &&
                ui::SliderValueFrom(slider, -200, 200, 500, 96) == 200,
            "slider: 越界钳到端点");
    t.check(ui::SliderValueFrom(rc(0, 0, 8, 20), 60, 3600, 4, 96) == 60,
            "slider: 轨道退化返回 vmin");
    t.check(ui::SliderValueFrom(slider, 60, 3600, 24, 192) == 60 &&
                ui::SliderValueFrom(slider, 60, 3600, 196, 192) == 3600,
            "slider: 高 DPI 内缩换算正确");

    // 拖拽落位：单列三卡（0/60/120 起，各高 50）
    std::vector<RECT> col3 = {rc(0, 0, 100, 50), rc(0, 60, 100, 110),
                              rc(0, 120, 100, 170)};
    t.check(ui::ComputeDragTarget({50, 55}, 0, col3) == 0, "drag: 拖到卡1 上方=插其前");
    t.check(ui::ComputeDragTarget({30, 70}, 0, col3) == 0, "drag: 卡1 行内左半=插其前");
    t.check(ui::ComputeDragTarget({50, 70}, 0, col3) == 1, "drag: 行内 x 落中线判后（>=）");
    t.check(ui::ComputeDragTarget({70, 95}, 0, col3) == 1, "drag: 卡1 行内右半=插其后");
    t.check(ui::ComputeDragTarget({50, 200}, 0, col3) == 2, "drag: 拖到空白区=移到末位");
    t.check(ui::ComputeDragTarget({50, 70}, -1, col3) == -1 &&
                ui::ComputeDragTarget({50, 70}, 3, col3) == -1,
            "drag: 非法源序返回 -1");
    // 同行水平中线：两卡并排（0..100 与 110..210）
    std::vector<RECT> row2 = {rc(0, 0, 100, 50), rc(110, 0, 210, 50)};
    t.check(ui::ComputeDragTarget({150, 25}, 0, row2) == 0, "drag: 同行左半=插其前");
    t.check(ui::ComputeDragTarget({170, 25}, 0, row2) == 1, "drag: 同行右半=插其后");

    // 命中测试：无内容区限制 = 纯命中 + 先注册者优先
    RECT empty{};
    std::vector<HitW> hits = {{rc(0, 0, 100, 50), 11}, {rc(50, 0, 150, 50), 12}};
    t.check(ui::HitTestAt(hits, {60, 25}, empty, empty, empty) == 11,
            "hit: 重叠区先注册者胜");
    t.check(ui::HitTestAt(hits, {120, 25}, empty, empty, empty) == 12,
            "hit: 非重叠区命中对应 id");
    t.check(ui::HitTestAt(hits, {200, 25}, empty, empty, empty) == -1,
            "hit: 未命中返回 -1");

    // 滚动视口裁剪：滚出视口的行不可点；chrome（侧栏）恒可点；豁免区可点
    RECT content = rc(200, 0, 600, 800), vp = rc(200, 100, 600, 800);
    std::vector<HitW> scrolled = {
        {rc(0, 0, 180, 800), 21},    // 侧栏 chrome（与内容区无交集）
        {rc(200, 0, 600, 50), 22},   // 已滚出视口上方
        {rc(200, 200, 600, 250), 23} // 视口内
    };
    t.check(ui::HitTestAt(scrolled, {50, 400}, content, vp, empty) == 21,
            "hit: chrome 区恒可点");
    t.check(ui::HitTestAt(scrolled, {300, 25}, content, vp, empty) == -1,
            "hit: 滚出视口的行不可误触");
    t.check(ui::HitTestAt(scrolled, {300, 220}, content, vp, empty) == 23,
            "hit: 视口内正常命中");
    RECT extra = rc(200, 780, 600, 880);
    std::vector<HitW> foot = {{rc(200, 800, 600, 860), 31}}; // 视口外、豁免区内
    t.check(ui::HitTestAt(foot, {300, 820}, content, vp, extra) == 31,
            "hit: 页脚豁免区可点");

    // 命中注册裁剪（2026-09-19 额度页滚动覆盖标题栏事故）：命中区裁到滚动视口内
    // 的可见部分，滚到页头/标题栏底下的卡片不留幽灵点击带
    RECT vp2 = rc(200, 100, 600, 800);
    RECT part = ui::ClipHitRect(rc(200, 40, 600, 160), vp2);
    t.check(part.left == 200 && part.top == 100 && part.right == 600 && part.bottom == 160,
            "hitclip: 越界命中裁到视口上缘（下半保留）");
    t.check(ui::ClipHitRect(rc(200, 0, 600, 50), vp2).right == 0,
            "hitclip: 完全滚出视口 = 空矩形（调用方不注册）");
    t.check(ui::ClipHitRect(rc(0, 0, 100, 50), empty).right == 0,
            "hitclip: 空裁剪 = 不裁（页头/chrome 恒可点路径）");
    // 裁剪后的命中配合视口判定：页头带内不可点、可见部分正常命中
    std::vector<HitW> clipped = {{ui::ClipHitRect(rc(200, 40, 600, 160), vp2), 41}};
    t.check(ui::HitTestAt(clipped, {300, 60}, content, vp2, empty) == -1,
            "hitclip: 裁剪后的命中在页头带内不响应");
    t.check(ui::HitTestAt(clipped, {300, 130}, content, vp2, empty) == 41,
            "hitclip: 裁剪后的命中在可见部分正常响应");
}

// ---------- quota_layout 额度页布局（2026-09-18 从 tray_ui ComputeQuota 抽出） ----------
void TestQuotaLayout(T& t) {
    using ui::QuotaPageIn;
    const RECT content{240, 90, 1000, 700}; // 760 宽视口
    auto valid = [](const RECT& r) { return r.right > r.left && r.bottom > r.top; };
    auto mk = [](int n) {
        QuotaPageIn in;
        for (int i = 0; i < n; ++i) in.cards.push_back({});
        return in;
    };

    // 四卡全可见：2×2 网格，同行顶对齐/等高，列间/行间不重叠
    {
        auto in = mk(4);
        int sc = 0;
        auto L = ui::ComputeQuotaLayout(in, content, 96, sc);
        t.check(L.vis_idx.size() == 4 && L.cards.size() == 4 && !L.has_hidden_chip &&
                    !L.empty_state,
                "qgrid: 四卡全可见无隐藏 chip");
        t.check(valid(L.stats_block) && L.stats_block.right == L.refresh_btn.left - 14,
                "qgrid: 无隐藏 chip 时统计块贴刷新按钮原位");
        t.check(L.cards[0].top == L.cards[1].top && L.cards[0].bottom == L.cards[1].bottom &&
                    L.cards[2].top == L.cards[3].top,
                "qgrid: 同行顶对齐且行内等高");
        t.check(L.cards[0].right <= L.cards[1].left && L.cards[1].right <= content.right &&
                    L.cards[2].top >= L.cards[1].bottom,
                "qgrid: 列间/行间不重叠");
        t.check(L.cards[0].right - L.cards[0].left == L.cards[1].right - L.cards[1].left,
                "qgrid: 两列等宽");
        t.check(L.content_bottom > L.cards[3].bottom && sc == 0,
                "qgrid: content_bottom 在末卡之下；内容装得下滚动为 0");
    }

    // 隐藏分组：vis/hidden 序列、chip、展开行
    {
        auto in = mk(4);
        in.cards[1].visible = false;
        int sc = 0;
        auto L = ui::ComputeQuotaLayout(in, content, 96, sc);
        t.check(L.hidden_idx.size() == 1 && L.hidden_idx[0] == 1 &&
                    L.vis_idx == (std::vector<int>{0, 2, 3}) && L.has_hidden_chip &&
                    L.hidden_rows.empty(),
                "qhide: 隐藏/可见分组序正确（收起时无行）");
        // 隐藏 chip 后画：统计块必须让出其左界，否则 chip 盖住「N 项服务正常计费中」
        t.check(L.stats_block.right + 10 <= L.hidden_chip.left &&
                    L.stats_block.right < L.refresh_btn.left,
                "qhide: 统计块让出隐藏 chip（不重叠盖字）");
        in.hidden_open = true;
        auto L2 = ui::ComputeQuotaLayout(in, content, 96, sc);
        t.check(L2.hidden_rows.size() == 1 && valid(L2.hidden_box),
                "qhide: 展开后隐藏区一行");
    }

    // 展开卡：expand_h>0、展开控件有效、卡高恰增 expand_h（单卡无行高等高干扰）
    {
        auto in0 = mk(1);
        int sc = 0;
        auto B0 = ui::ComputeQuotaLayout(in0, content, 96, sc);
        auto in1 = mk(1);
        in1.cards[0].expanded = true;
        in1.cards[0].alert_capable = true;
        auto B1 = ui::ComputeQuotaLayout(in1, content, 96, sc);
        t.check(B1.cr[0].expand_h > 0 && valid(B1.cr[0].ex_alias) &&
                    valid(B1.cr[0].ex_key) && valid(B1.cr[0].ex_alert) &&
                    valid(B1.cr[0].ex_save) && valid(B1.cr[0].ex_delete),
                "qexpand: 展开区矩形有效（含阈值行）");
        t.check((B1.cards[0].bottom - B1.cards[0].top) -
                        (B0.cards[0].bottom - B0.cards[0].top) ==
                    B1.cr[0].expand_h,
                "qexpand: 卡高恰增 expand_h");
        t.check(!valid(B0.cr[0].ex_key) && B0.cr[0].expand_h == 0,
                "qexpand: 未展开卡无展开区");
    }

    // 趋势卡：trend 占位 122 DIP、卡加高 142 DIP
    {
        auto in = mk(1);
        int sc = 0;
        auto L0 = ui::ComputeQuotaLayout(in, content, 96, sc);
        in.cards[0].has_trend = true;
        auto L1 = ui::ComputeQuotaLayout(in, content, 96, sc);
        t.check(valid(L1.cr[0].trend) && !valid(L0.cr[0].trend) &&
                    L1.cr[0].trend.bottom - L1.cr[0].trend.top == 122,
                "qtrend: 趋势带占位 122 DIP");
        t.check(L1.cards[0].bottom - L0.cards[0].bottom == 122 + 20,
                "qtrend: 趋势卡加高 142 DIP");
    }

    // 添加面板：面板/chips/输入/创建键有效，首卡下移
    {
        auto in = mk(2);
        int sc = 0;
        auto L0 = ui::ComputeQuotaLayout(in, content, 96, sc);
        in.add_open = true;
        auto L1 = ui::ComputeQuotaLayout(in, content, 96, sc);
        t.check(valid(L1.add_panel) && valid(L1.ap_create) && valid(L1.ap_alias) &&
                    L1.ap_type[0].top < L1.ap_alias.top,
                "qadd: 面板控件有效且 chips 在输入框之上");
        t.check(L1.cards[0].top > L0.cards[0].top, "qadd: 面板展开后首卡下移");
    }

    // 滚动钳制与整体平移
    {
        auto in = mk(40); // 20 行卡片必然超高
        in.scroll = 100000;
        int sc = 0;
        auto L = ui::ComputeQuotaLayout(in, content, 96, sc);
        t.check(sc == L.content_bottom + 8 - L.viewport_bottom && sc < 100000,
                "qscroll: 超大滚动钳到 max_scroll");
        in.scroll = 100;
        int sc100 = 100;
        auto L100 = ui::ComputeQuotaLayout(in, content, 96, sc100);
        in.scroll = 0;
        int sc0 = 0;
        auto L0 = ui::ComputeQuotaLayout(in, content, 96, sc0);
        t.check(sc100 == 100 && L100.cards[0].top == L0.cards[0].top - 100,
                "qscroll: 滚动 100 整体上移 100px");
        auto inf = mk(1);
        inf.scroll = 500;
        int sc1 = inf.scroll;
        ui::ComputeQuotaLayout(inf, content, 96, sc1);
        t.check(sc1 == 0, "qscroll: 内容装得下时钳到 0");
    }

    // DPI：固定 DIP 控件尺寸随 DPI 加倍
    {
        auto in = mk(2);
        int sc = 0;
        auto L96 = ui::ComputeQuotaLayout(in, content, 96, sc);
        auto L192 = ui::ComputeQuotaLayout(in, content, 192, sc);
        t.check(L96.add_btn.right - L96.add_btn.left == 124 &&
                    L192.add_btn.right - L192.add_btn.left == 248,
                "qdpi: 页头按钮宽 124→248");
        t.check(L192.cr[0].toggle.bottom - L192.cr[0].toggle.top == 48,
                "qdpi: 开关高 24→48");
    }
}

// ---------- disp_layout 显示设置页布局（2026-09-18 从 tray_ui ComputeDisp 抽出） ----------
void TestDispLayout(T& t) {
    using ui::DispPageIn;
    const RECT content{240, 90, 1000, 700};
    auto valid = [](const RECT& r) { return r.right > r.left && r.bottom > r.top; };

    // Embed：嵌入说明 + 偏移滑杆区 + 嵌入内容行；排版/字号隐藏，字色/低额/品牌色可见
    {
        DispPageIn in; // 默认 Embed、notify off
        int sc = 0;
        auto L = ui::ComputeDispLayout(in, content, 96, sc);
        t.check(L.embed && L.pos_visible && valid(L.embed_note) && valid(L.big_val) &&
                    valid(L.b_zero) && valid(L.slider) && valid(L.embname_toggle) &&
                    valid(L.embwin_toggle),
                "disp: Embed=嵌入说明+偏移滑杆+嵌入内容行");
        t.check(!valid(L.layout_label) && !valid(L.font_val) && valid(L.color_seg) &&
                    valid(L.lowc_toggle) && valid(L.brand_toggle),
                "disp: Embed 隐藏排版/字号，字色/低额变色/品牌色全模式可见");
        t.check(!L.thr_visible && !valid(L.thr_k) && !valid(L.thr_d),
                "disp: 预警关→阈值行隐藏");
        t.check(valid(L.mode[0]) && valid(L.mode[3]) && L.mode[0].top < L.mode[3].top &&
                    L.card[0].top < L.card[1].top && L.card[1].top < L.card[2].top,
                "disp: 模式四行与分区卡有序");
    }
    // 浮条（未吸附）：吸附开关 + X/Y 滑杆 + 拖动提示；预警开→阈值两行
    {
        DispPageIn in;
        in.mode = DisplayMode::Free;
        in.notify_enabled = true;
        int sc = 0;
        auto L = ui::ComputeDispLayout(in, content, 96, sc);
        t.check(L.free && L.pos_visible && valid(L.snap_toggle) && valid(L.x_slider) &&
                    valid(L.y_slider) && valid(L.pos_hint) && !valid(L.slider) &&
                    !valid(L.embed_note) && valid(L.layout_label) && valid(L.font_val) &&
                    valid(L.color_seg),
                "disp: 浮条=吸附开关+XY滑杆+提示，外观行齐全");
        t.check(L.thr_visible && valid(L.thr_k) && valid(L.thr_d) &&
                    L.thr_k.top < L.thr_d.top,
                "disp: 预警开→阈值两行有序");
        // 吸附开 → Y 行隐藏，X 行仍在
        in.snap = true;
        int sc2 = 0;
        auto L2 = ui::ComputeDispLayout(in, content, 96, sc2);
        t.check(L2.snap && valid(L2.x_slider) && !valid(L2.y_slider) &&
                    L2.content_bottom < L.content_bottom,
                "disp: 浮条吸附开 → Y 行隐藏内容变短");
    }
    // 圆环：吸附偏移滑杆，无吸附开关/Y 行
    {
        DispPageIn in;
        in.mode = DisplayMode::Rings;
        int sc = 0;
        auto L = ui::ComputeDispLayout(in, content, 96, sc);
        t.check(L.rings && L.pos_visible && valid(L.x_slider) && !valid(L.snap_toggle) &&
                    !valid(L.y_slider) && !valid(L.embed_note),
                "disp: 圆环=吸附偏移滑杆，无开关/Y行");
    }
    // 仅托盘：位置区整体隐藏且内容更短（分区序号紧凑不空占）
    {
        DispPageIn in;
        in.mode = DisplayMode::TrayOnly;
        int sc = 0;
        auto L = ui::ComputeDispLayout(in, content, 96, sc);
        DispPageIn ie;
        int sce = 0;
        auto Le = ui::ComputeDispLayout(ie, content, 96, sce);
        t.check(!L.pos_visible && !valid(L.big_val) && !valid(L.snap_row) &&
                    !valid(L.slider) && L.content_bottom < Le.content_bottom,
                "disp: 仅托盘=位置区隐藏且内容更短");
    }
    // 设置列 640 DIP 居中
    {
        DispPageIn in;
        int sc = 0;
        auto L = ui::ComputeDispLayout(in, content, 96, sc);
        t.check(L.mode[0].right - L.mode[0].left == 640 &&
                    L.mode[0].left - content.left == content.right - L.mode[0].right,
                "disp: 设置列 640 DIP 居中");
    }
    // 滚动钳制 + DPI
    {
        DispPageIn in;
        in.scroll = 100000;
        int sc = 0;
        auto L = ui::ComputeDispLayout(in, content, 96, sc);
        t.check(sc == L.content_bottom + 12 - L.viewport_bottom && sc < 100000,
                "disp: 滚动钳到 max_scroll");
        DispPageIn in2;
        int sc2 = 0;
        auto L192 = ui::ComputeDispLayout(in2, content, 192, sc2);
        t.check(L192.mode[0].bottom - L192.mode[0].top == 108,
                "disp: DPI 192 模式行高 54→108");
    }
}

// ---------- ProviderOps 分派表一致性 ----------
void TestProviderOps(T& t) {
    struct Row { AccountType ty; const char* id; wchar_t badge; const char* alias; bool key; };
    const Row rows[] = {
        {AccountType::Kimi, "kimi", L'K', "Kimi Code", true},
        {AccountType::Codex, "codex", L'C', "Codex", false},
        {AccountType::Deepseek, "deepseek", L'D', "DeepSeek", true},
        {AccountType::Glm, "glm", L'G', "GLM", true},
    };
    bool all = true;
    for (const auto& r : rows) {
        const ProviderOps& ops = ProviderOpsOf(r.ty);
        all = all && std::string(ops.id) == r.id && ops.badge == r.badge &&
              std::string(ops.default_alias) == r.alias && ops.uses_api_key == r.key &&
              ops.build_request && ops.parse && ops.contract && *ops.contract;
    }
    t.check(all, "ops: 四平台 id/徽章/别名/用Key/函数指针齐全");
    t.check(ProviderOpsById("glm") && std::string(ProviderOpsById("glm")->id) == "glm" &&
                ProviderOpsById("kimi")->badge == L'K',
            "ops: 按 id 查表命中");
    t.check(ProviderOpsById("unknown") == nullptr, "ops: 未知 id 返回 nullptr");
    bool contracts = true;
    for (const auto& r : rows) {
        const ProviderOps* ops = ProviderOpsById(r.id);
        contracts = contracts && ops && ops->contract == ProviderOpsOf(r.ty).contract;
    }
    t.check(contracts, "ops: 契约版本两种查法一致");
    // 视觉身份单点（品牌色/副题/类型章；2026-09-18 从 theme/state/tray_ui 收敛）
    bool vis = true;
    const COLORREF brand_cols[4] = {RGB(0x56, 0x60, 0x74), RGB(0x10, 0xA3, 0x7F),
                                    RGB(0x4D, 0x6B, 0xFE), RGB(0x7C, 0x5C, 0xFF)};
    const wchar_t* subs[4] = {L"Kimi Code 周期额度", L"Codex · ChatGPT 订阅",
                              L"Pay-as-you-go 结算账户", L"智谱 BigModel 编程订阅"};
    const wchar_t* tags[4] = {L"周期配额", L"免密直连", L"按量付费", L"编码订阅"};
    for (int i = 0; i < 4; ++i) {
        const ProviderOps& ops = ProviderOpsOf(rows[i].ty);
        vis = vis && ops.brand_color == brand_cols[i] &&
              wcscmp(ops.subtitle, subs[i]) == 0 && wcscmp(ops.type_tag, tags[i]) == 0;
    }
    t.check(vis, "ops: 品牌色/副题/类型章与原映射逐字一致");
    t.check(ProviderOpsByBadge(L'G').brand_color == ProviderOpsOf(AccountType::Glm).brand_color &&
                ProviderOpsByBadge(L'一').id == std::string("kimi"),
            "ops: 按徽章查表；未知字母回落 Kimi（与旧 switch 同）");
}

// ---------- 2026-09-18 检视修复回归 ----------
void TestRegressions(T& t) {
    // TrimW 宽字符空白：CJK 低字节 <=0x20（一=U+4E00/三=U+4E09）不得被当空白剪掉
    t.check(TrimW(L" \t一号 GLM \r\n") == L"一号 GLM", "trimW: CJK 别名首尾保留");
    t.check(TrimW(L"三号") == L"三号" && TrimW(L"  x  ") == L"x",
            "trimW: 纯空白仍被剥离");

    // JsonQuote 控制字符转义 → 自家解析器可往返（此前 JStr 漏 \t/\r 会写出不可解析 JSON）
    const std::string nasty = "a\"b\\c\td\re\nf\x01g中";
    std::string quoted = JsonQuote(nasty);
    std::string doc = "{\"v\":" + quoted + "}";
    auto j = JsonParse(doc);
    t.check(j && j->Find("v").IsString() && j->Find("v").AsString() == nasty,
            "json: 控制字符串 Quote→Parse 往返一致");

    // AsInt：溢出拒绝（此前 _strtoui64 静默返回 _UI64_MAX）；负数按有符号解析
    auto big = JsonParse("{\"v\":99999999999999999999}");
    int64_t out = 0;
    t.check(big && !big->Find("v").AsInt(out), "json: 溢出整数 AsInt 拒绝");
    auto neg = JsonParse("{\"v\":-5}");
    int64_t nv = 0;
    t.check(neg && neg->Find("v").AsInt(nv) && nv == -5, "json: 负整数有符号解析");

    // PrimaryBalance：CNY 优先、否则首项、空表 nullptr（通知/显示/历史统一口径）
    std::vector<MoneyBalance> bs(2);
    bs[0].currency = "USD";
    bs[1].currency = "CNY";
    t.check(PrimaryBalance(bs) == &bs[1], "balance: CNY 优先");
    bs[1].currency = "EUR";
    t.check(PrimaryBalance(bs) == &bs[0], "balance: 无 CNY 回落首项");
    t.check(PrimaryBalance({}) == nullptr, "balance: 空表 nullptr");

    // 别名钳制：外部写入的超长别名按宽字符截到 64（与 EM_SETLIMITTEXT 同口径）
    std::string long_alias; // 200 个 U+4000（3 字节/字符的合法 UTF-8）
    for (int i = 0; i < 200; ++i) long_alias += "\xe4\x80\x80";
    std::string v2long = R"({"config_version":2,"display":{},"query":{},"notify":{},)"
                         R"("accounts":[{"id":"x1","type":0,"alias":")" + long_alias +
                         R"(","enabled":true,"card_visible":true,"cred_rev":1}],"account_seq":1})";
    AppConfig cl;
    t.check(ConfigIo::Load(v2long, cl) && cl.accounts.size() == 1 &&
                Utf8ToWide(cl.accounts[0].alias).size() == 64,
            "config: 超长别名钳制到 64 宽字符");

    // SecureFailureText：SECURE_FAILURE 回调标志位（0x80000000 标记）解码
    char sb[80];
    const char* st = SecureFailureText(0x80000006u, sb, sizeof(sb)); // 0x02|0x04
    t.check(st && std::strstr(st, "证书过期") && std::strstr(st, "未知CA"),
            "http: 安全失败位解码 过期|未知CA");
    st = SecureFailureText(0x80000001u, sb, sizeof(sb));
    t.check(st && std::strstr(st, "CN不符"), "http: 安全失败位解码 CN");
    t.check(SecureFailureText(12175u, sb, sizeof(sb)) == nullptr,
            "http: 无标记位的 Win32 码不误判");
    st = SecureFailureText(0x80000000u, sb, sizeof(sb));
    t.check(st && std::strstr(st, "0x0"), "http: 空位回退十六进制显示");
}

// ---------- 嵌入两行（名称行/值行 + 列布局） ----------
void TestEmbedRows(T& t) {
    int64_t now = NowUnix(), mono = NowMono();
    DisplayOptions opt;
    ProviderSnapshot k = MakeKimiTestSnapshot(now);
    ProviderSnapshot d = MakeDeepSeekTestSnapshot(now);
    ProviderSnapshot c = MakeCodexTestSnapshot(now);
    ProviderSnapshot g = MakeGlmTestSnapshot(now);
    auto accs = FixtureAccounts();
    ProviderSnapshot snaps[4] = {k, c, d, g};
    auto rows = BuildEmbedItems(ViewsOf(accs, snaps), opt, now, mono, 300);
    t.check(rows.size() == 4, "embed: 四账户都出现");
    t.check(rows[0].name == L"Kimi Code" && rows[1].name == L"Codex" &&
                rows[2].name == L"DeepSeek" && rows[3].name == L"GLM",
            "embed: 名称行=账户别名（默认平台名）");
    std::wstring v;
    for (const auto& s : rows[0].values) v += s.text;
    t.check(v.find(L"62%") != std::wstring::npos && v.find(L"5h") != std::wstring::npos,
            "embed: Kimi 值行含 62%/5h");
    std::wstring cv;
    for (const auto& s : rows[1].values) cv += s.text;
    t.check(cv.find(L"70%") != std::wstring::npos && cv.find(L"45%") != std::wstring::npos,
            "embed: Codex 值行含 5h/7天 百分比");
    std::wstring dv;
    for (const auto& s : rows[2].values) dv += s.text;
    t.check(dv.find(L"¥23.48") != std::wstring::npos, "embed: DeepSeek 值行含余额");
    std::wstring gv;
    for (const auto& s : rows[3].values) gv += s.text;
    t.check(gv.find(L"70%") != std::wstring::npos && gv.find(L"45%") != std::wstring::npos &&
                gv.find(L"月") == std::wstring::npos,
            "embed: GLM 值行含 5h/7d 百分比（无月度）");
    // 窗口分隔在嵌入条压成紧凑"·"(省宽,浮窗保持" · ")
    t.check(gv.find(L"·") != std::wstring::npos && gv.find(L" · ") == std::wstring::npos,
            "embed: 值行分隔紧凑化");
    // 未配置 → 名称行=别名、值行"待配置"（首段为别名前缀段）
    ProviderSnapshot uncs[4] = {UnconfSnap(AccountType::Kimi), UnconfSnap(AccountType::Codex),
                                UnconfSnap(AccountType::Deepseek),
                                UnconfSnap(AccountType::Glm)};
    auto r2 = BuildEmbedItems(ViewsOf(accs, uncs), opt, now, mono, 300);
    t.check(r2.size() == 4 && !r2[0].values.empty() && r2[0].values[0].text == L"待配置",
            "embed: 未配置 → 值行'待配置'");
    t.check(r2[0].name == L"Kimi Code" && r2[1].name == L"Codex" &&
                r2[2].name == L"DeepSeek" && r2[3].name == L"GLM",
            "embed: 未配置时名称仍按账户别名");
    // 列布局：放得下=自然宽(max(name,value))；超宽=比例收缩占满 avail
    std::vector<float> nw{40.0f, 70.0f, 50.0f, 60.0f}, vw{120.0f, 90.0f, 80.0f, 100.0f};
    std::vector<float> colw;
    float total = 0.0f;
    ComputeEmbedCols(nw, vw, 8.0f, 1e9f, colw, total);
    t.check(colw.size() == 4 && total == 120.0f + 90.0f + 80.0f + 100.0f + 24.0f,
            "embed: 列宽=max(名称,值) 自然排布");
    ComputeEmbedCols(nw, vw, 8.0f, 200.0f, colw, total);
    t.check(total == 200.0f && colw[0] < 120.0f, "embed: 超宽收缩占满 avail");
}

// ---------- 模式合并/颜色开关/拖放落位（2026-09-21 B 迭代） ----------
void TestBarColorsAndModes(T& t) {
    int64_t now = NowUnix(), mono = NowMono();
    auto accs = FixtureAccounts();
    ProviderSnapshot k = MakeKimiTestSnapshot(now);
    // 低额夹具：5h 窗压到 15%（≤默认阈值 20 → 琥珀；周窗 62% 保持基色）
    ProviderSnapshot lowk = k;
    for (auto& w : lowk.windows) {
        if (w.stable_key == "kimi_rolling_18000s") {
            w.has_remaining = true;
            w.remaining = 15;
            w.remaining_percent = 15;
        }
    }
    std::vector<AccountView> v{{&accs[0], &lowk}};
    auto seg_color = [](const DisplayModel& m, const std::wstring& text) {
        for (const auto& line : m.lines)
            for (const auto& s : line)
                if (s.text == text) return s.color;
        return (COLORREF)0;
    };
    const COLORREF amber = RGB(255, 176, 32), base = RGB(255, 255, 255);

    // 低额变色默认开：15% 琥珀
    DisplayOptions opt;
    DisplayModel m = BuildDisplayModel(v, opt, now, mono, 300);
    t.check(seg_color(m, L"15%") == amber, "barcolor: 15% → 琥珀（默认开）");

    // 开关关闭 → 值段基色（状态告警色不受影响，见 WarnColor 单测）
    opt.low_quota_color = false;
    m = BuildDisplayModel(v, opt, now, mono, 300);
    t.check(seg_color(m, L"15%") == base, "barcolor: 变色关 → 值段基色");
    opt = DisplayOptions{};

    // 逐账户阈值（alert_pct）：1=关闭 10=自定义(15%>10 → 基色) 50=自定义(命中) 0=继承
    accs[0].alert_pct = 1;
    m = BuildDisplayModel(v, opt, now, mono, 300);
    t.check(seg_color(m, L"15%") == base, "barcolor: alert_pct=1 → 关闭低额变色");
    accs[0].alert_pct = 10;
    m = BuildDisplayModel(v, opt, now, mono, 300);
    t.check(seg_color(m, L"15%") == base, "barcolor: alert_pct=10 → 15% 不判低");
    accs[0].alert_pct = 50;
    m = BuildDisplayModel(v, opt, now, mono, 300);
    t.check(seg_color(m, L"15%") == amber, "barcolor: alert_pct=50 → 15% 判低");
    accs[0].alert_pct = 0;
    t.check(EffectiveLowPct(accs[0], 20) == 20 && EffectiveLowPct(accs[1], 20) == 20,
            "barcolor: alert_pct=0 → 继承全局");

    // 账户名品牌色开关：别名前缀段用平台品牌色，值段语义色不变
    std::vector<AccountView> vok{{&accs[0], &k}};
    opt.brand_color_names = true;
    m = BuildDisplayModel(vok, opt, now, mono, 300);
    COLORREF brand = ProviderOpsOf(AccountType::Kimi).brand_color;
    t.check(!m.lines.empty() && m.lines[0][0].color == brand &&
                m.lines[0][0].text == L"Kimi Code ",
            "barcolor: 品牌色开 → 别名段品牌色");
    t.check(seg_color(m, L"81%") == base, "barcolor: 品牌色开 → 值段仍基色");
    opt.brand_color_names = false;
    m = BuildDisplayModel(vok, opt, now, mono, 300);
    t.check(!m.lines.empty() && m.lines[0][0].color == base,
            "barcolor: 品牌色关 → 别名段基色");

    // 嵌入条窗口标签开关：关 → 纯标签段丢弃、合并段剥成纯百分比
    DisplayOptions owin;
    ProviderSnapshot karr[4] = {k, k, k, k}; // ViewsOf 需与账户同长的快照数组
    auto rows = BuildEmbedItems(ViewsOf(accs, karr), owin, now, mono, 300);
    std::wstring vk;
    for (const auto& s : rows[0].values) vk += s.text;
    t.check(vk.find(L"5h") != std::wstring::npos && vk.find(L"81%") != std::wstring::npos,
            "embedfmt: 标签默认显示（5h 81%）");
    owin.embed_show_windows = false;
    rows = BuildEmbedItems(ViewsOf(accs, karr), owin, now, mono, 300);
    vk.clear();
    for (const auto& s : rows[0].values) vk += s.text;
    t.check(vk.find(L"5h") == std::wstring::npos && vk.find(L"周") == std::wstring::npos &&
                vk.find(L"余") == std::wstring::npos &&
                vk.find(L"81%") != std::wstring::npos &&
                vk.find(L"62%") != std::wstring::npos,
            "embedfmt: 标签关 → 只剩百分比（81%·62%）");
    // 嵌入名称行品牌色
    DisplayOptions obrand;
    obrand.brand_color_names = true;
    rows = BuildEmbedItems(ViewsOf(accs, karr), obrand, now, mono, 300);
    t.check(rows[0].name_color == brand, "embedfmt: 品牌色开 → 名称行品牌色");

    // ---- 配置迁移：旧 Docked(0) + dock_offset_* → Free + 吸附 ----
    std::string legacy =
        R"({"config_version":2,"display":{"mode":0,"free_pos_x":140,"free_pos_y":60,)"
        R"("dock_offset_x":-77,"dock_offset_y":6,"font_size_pt":9,"text_color_mode":0,)"
        R"("slot_offset_x_dip":0,"two_lines":true},)"
        R"("query":{"interval_sec":300},"notify":{"enabled":false}})";
    AppConfig c;
    t.check(ConfigIo::Load(legacy, c), "migrate: 旧 v2(mode=0) 解析成功");
    t.check(c.display.mode == DisplayMode::Free && c.display.bar_snap,
            "migrate: mode=0 → Free + 吸附任务栏");
    t.check(c.display.bar_snap_x_dip == -77 && c.display.bar_snap_y_dip == 6,
            "migrate: dock_offset_* → bar_snap_*");
    // 新键往返稳定
    AppConfig back;
    t.check(ConfigIo::Load(ConfigIo::Dump(c), back), "migrate: 新键 Dump→Load 成功");
    t.check(back.display.mode == DisplayMode::Free && back.display.bar_snap &&
                back.display.bar_snap_x_dip == -77,
            "migrate: 吸附字段往返保留");
    back.display.low_quota_color = false;
    back.display.brand_color_names = true;
    back.display.embed_brand_icons = true;
    back.display.embed_show_names = false;
    back.display.embed_show_windows = false;
    AppConfig back2;
    t.check(ConfigIo::Load(ConfigIo::Dump(back), back2), "migrate: 外观新键往返成功");
    t.check(!back2.display.low_quota_color && back2.display.brand_color_names &&
                back2.display.embed_brand_icons &&
                !back2.display.embed_show_names && !back2.display.embed_show_windows,
            "migrate: 外观开关往返一致");

    // ---- 拖放落位判定（ResolveBarDrop：吸附/自由互逆于锚定式） ----
    const RECT work{0, 0, 2560, 1392}, mon{0, 0, 2560, 1440}, tb{0, 1392, 2560, 1440};
    RECT far_win{1000, 900, 1300, 940};
    BarDrop d1 = ResolveBarDrop(far_win, work, mon, tb, 96);
    t.check(!d1.snap && d1.free_pos_x_dip == 1000 && d1.free_pos_y_dip == 900,
            "drop: 远离任务栏 → 自由位置（工作区 DIP）");
    RECT near_win{2400, 1340, 2540, 1380}; // 底缘距任务栏上缘 12px
    BarDrop d2 = ResolveBarDrop(near_win, work, mon, tb, 96);
    t.check(d2.snap && d2.snap_x_dip == -20 && d2.snap_y_dip == 8,
            "drop: 近任务栏上缘 → 吸附（右缘 -20 / 间隙 8）");
    // 反演：锚定式按 d2 落位应还原窗口矩形
    int px = mon.right - (near_win.right - near_win.left) + d2.snap_x_dip * 96 / 96;
    int py = tb.top - (near_win.bottom - near_win.top) -
             (4 + d2.snap_y_dip * 96 / 96);
    t.check(px == near_win.left && py == near_win.top,
            "drop: 吸附参数锚定式反演还原落点");
}

// ---------- Codex 解析（wham/usage 样本锁定 2026-09-05） ----------
void TestCodexParse(T& t) {
    int64_t now = NowUnix();
    // 实机响应结构（字段名/数值类型与 2026-09-05 实机样本一致;百分比为数字）
    auto r = ParseCodexResponse(
        R"({"plan_type":"plus","rate_limit":{"allowed":true,"limit_reached":false,
              "primary_window":{"used_percent":56,"limit_window_seconds":18000,
                                "reset_after_seconds":14727,"reset_at":1788637824},
              "secondary_window":{"used_percent":93,"limit_window_seconds":604800,
                                  "reset_after_seconds":134581,"reset_at":1788757679}},
            "credits":{"has_credits":false,"unlimited":false,"balance":0}})",
        now, NowMono());
    t.check(r.ok && !r.protocol_error && r.windows.size() == 2, "codex: 两窗口解析");
    t.check(r.windows[0].stable_key == "codex_5h" && r.windows[0].duration_seconds == 18000 &&
                r.windows[0].remaining_percent == 44 && r.windows[0].used_percent_raw == 56 &&
                !r.windows[0].has_limit && r.windows[0].availability == Availability::Known,
            "codex: 5h 窗口 56% 已用 → 剩余 44%（无绝对额度）");
    t.check(r.windows[1].stable_key == "codex_7d" && r.windows[1].remaining_percent == 7 &&
                r.windows[1].has_reset && r.windows[1].reset_at_utc == 1788757679,
            "codex: 7天窗口 93% → 剩余 7% + 重置时间");
    // 结构不兼容 → protocol_error
    auto bad = ParseCodexResponse(R"({"foo":1})", now, NowMono());
    t.check(!bad.ok && bad.protocol_error, "codex: 缺 rate_limit → 结构不兼容");
    // 单窗口残缺 → partial(有一个窗口可用,不判整体失败)
    auto partial = ParseCodexResponse(
        R"({"plan_type":"plus","rate_limit":{"allowed":true,"primary_window":{"used_percent":56,
              "limit_window_seconds":18000,"reset_at":1788637824},"secondary_window":{}}})",
        now, NowMono());
    t.check(partial.ok && partial.windows.size() == 1 &&
                partial.completeness == std::string("partial"),
            "codex: 单窗口残缺 → partial");
    // 双窗口都不可解析 → 协议错误(无任何已知窗口)
    auto empty = ParseCodexResponse(
        R"({"plan_type":"plus","rate_limit":{"allowed":true,"primary_window":{},
              "secondary_window":{}}})", now, NowMono());
    t.check(empty.protocol_error, "codex: 窗口全不可解析 → 结构不兼容");
    // 快照往返:仅百分比窗口保留 used_percent_raw
    ProviderSnapshot snap = SnapFromParse(r, kProviderCodex);
    std::string j = SnapshotJson::Dump(snap);
    ProviderSnapshot back;
    t.check(SnapshotJson::Parse(j, back) && back.windows.size() == 2 &&
                back.windows[0].used_percent_raw == 56 && back.windows[1].remaining_percent == 7,
            "codex: 快照序列化往返保留百分比字段");
}

// ---------- Antigravity（本机 hub RetrieveUserQuotaSummary 真机样本锁定 2026-11-27） ----------
// 真机样本 experiments/ag_probe/ag_response.json（文案字段截短;字段名/数值/数组序原样）
std::string AgBodyReal() {
    return R"({"response":{"groups":[
        {"displayName":"Gemini Models","description":"Models within this group: ...","buckets":[
          {"bucketId":"gemini-weekly","displayName":"Weekly Limit Remaining","description":"...",
           "window":"weekly","remainingFraction":0.86472285,"resetTime":"2026-12-03T11:52:46Z"},
          {"bucketId":"gemini-5h","displayName":"Five Hour Limit Remaining","description":"...",
           "window":"5h","remainingFraction":0.48366153,"resetTime":"2026-11-27T08:14:17Z"}]},
        {"displayName":"Claude and GPT models","description":"Models within this group: ...","buckets":[
          {"bucketId":"3p-weekly","displayName":"Weekly Limit Remaining","window":"weekly","remainingFraction":1},
          {"bucketId":"3p-5h","displayName":"Five Hour Limit Remaining","window":"5h","remainingFraction":1}]}],
        "description":"Within each group, models share a weekly limit and a 5-hour limit."}})";
}

void TestAntigravity(T& t) {
    const int64_t now = NowUnix(), mono = NowMono();
    const size_t npos = std::wstring::npos;

    // ---- 请求规格 / 分派表 ----
    const ProviderOps& aops = ProviderOpsOf(AccountType::Antigravity);
    t.check(std::string(aops.id) == "antigravity" && aops.badge == L'A' &&
                !aops.uses_api_key && aops.local_hub &&
                std::string(aops.contract) == kAntigravityContract &&
                std::wstring(aops.type_tag).size() > 1 &&
                std::wstring(aops.subtitle).size() > 1,
            "ag: 分派表行 id/徽章/无 Key/本机 hub/契约/类型章/副题");
    auto spec = BuildAntigravityRequest("http://127.0.0.1:48342");
    t.check(spec.url == "http://127.0.0.1:48342/exa.language_server_pb.LanguageServerService/"
                        "RetrieveUserQuotaSummary" &&
                spec.method == "POST" && spec.body == "{}" && !spec.auth_bearer &&
                std::string(spec.auth_header) == "x-codeium-csrf-token",
            "ag: 请求 = 现场基址 + 固定 RPC 路径,POST {},CSRF 令牌头");
    auto kspec = BuildKimiRequest("http://127.0.0.1:1"); // 基址参数对固定地址 provider 必须无影响
    t.check(kspec.url == "https://api.kimi.com/coding/v1/usages" && kspec.method == "GET" &&
                kspec.body.empty() && std::string(kspec.auth_header) == "Authorization" &&
                !BuildGlmRequest("x").auth_bearer,
            "ag: 既有 provider 请求规格不随基址参数改变（GET/无体/Authorization）");
    t.check(ProviderSingleCard(AccountType::Antigravity) &&
                ProviderSingleCard(AccountType::Codex) &&
                !ProviderSingleCard(AccountType::Kimi) && !ProviderSingleCard(AccountType::Glm) &&
                !ProviderSingleCard(AccountType::Deepseek),
            "ag: 无 Key 类仅一张卡片,其余可多卡");
    t.check(ProviderAlertCapable(AccountType::Antigravity) && ProviderAlertCapable(AccountType::Glm) &&
                ProviderAlertCapable(AccountType::Kimi) &&
                !ProviderAlertCapable(AccountType::Codex) &&
                !ProviderAlertCapable(AccountType::Deepseek),
            "ag: 预警资格 = 比例窗口类（codex/deepseek 维持不参与）");
    t.check(std::string(ProviderOpsByBadge(L'A').id) == "antigravity" &&
                std::string(ProviderOpsById("antigravity")->default_alias) == "Antigravity" &&
                ProviderOpsById("antigravity")->local_hub && !ProviderOpsOf(AccountType::Codex).local_hub,
            "ag: 按 id/徽章查表;本机 hub 标志仅 antigravity 置位");

    // ---- 解析器：真机样本 ----
    ParseResult r = ParseAntigravityResponse(AgBodyReal(), now, mono);
    auto find = [](const ParseResult& pr, const char* key) -> const QuotaWindow* {
        for (const auto& w : pr.windows)
            if (w.stable_key == key) return &w;
        return nullptr;
    };
    t.check(r.ok && !r.protocol_error && r.windows.size() == 4 && r.completeness == "full",
            "ag: 真机样本四桶全解析,Gemini 池齐备 = full");
    const QuotaWindow* w5 = find(r, "ag_5h");
    const QuotaWindow* ww = find(r, "ag_week");
    t.check(w5 && w5->remaining_percent == 48 && !w5->tiny_positive && w5->label == "5h余" &&
                w5->duration_seconds == 5 * 3600 && w5->period_kind == PeriodKind::Rolling &&
                !w5->has_limit && w5->availability == Availability::Known && w5->has_reset &&
                std::fabs(w5->used_percent_raw - 51.633847) < 1e-4,
            "ag: gemini-5h 0.4836 → 5h余 48%（向下取整,比例口径）+ 重置时刻");
    t.check(ww && ww->remaining_percent == 86 && ww->label == "7d余" && ww->has_reset &&
                ww->duration_seconds == 7 * 86400,
            "ag: gemini-weekly 0.8647 → 7d余 86%");
    // 服务端数组序原样保留(weekly 在前):显示层必须按 key 取窗,不得按数组序
    t.check(r.windows.size() == 4 && r.windows[0].stable_key == "ag_week" &&
                r.windows[1].stable_key == "ag_5h",
            "ag: 解析保留服务端数组序（weekly 在 5h 之前）");
    const QuotaWindow* p5 = find(r, "ag_3p_5h");
    const QuotaWindow* pw = find(r, "ag_3p_week");
    t.check(p5 && p5->remaining_percent == 100 && !p5->has_reset && pw &&
                pw->remaining_percent == 100 && !pw->has_reset,
            "ag: 未动过的 Claude&GPT 桶 100% 且无 resetTime（到期未知）");

    // ---- 解析器：信封变体 ----
    ParseResult top = ParseAntigravityResponse(
        R"({"groups":[{"buckets":[{"bucketId":"gemini-5h","window":"5h","remainingFraction":0.5}]}]})",
        now, mono);
    t.check(top.ok && top.windows.size() == 1 && top.windows[0].stable_key == "ag_5h" &&
                top.completeness == "partial",
            "ag: 顶层 groups 信封兼容,单窗 = partial");
    ParseResult loose = ParseAntigravityResponse(
        R"({"buckets":[{"bucketId":"gemini-weekly","window":"weekly","remainingFraction":0.25}]})",
        now, mono);
    t.check(loose.ok && loose.windows.size() == 1 &&
                loose.windows[0].stable_key == "ag_week" && loose.windows[0].remaining_percent == 25,
            "ag: 顶层 buckets（未分组）兼容");

    // ---- 解析器：proto3 零值省略 / disabled / 钳制 / 未映射池 ----
    ParseResult zero = ParseAntigravityResponse(
        R"({"response":{"groups":[{"buckets":[
            {"bucketId":"gemini-5h","window":"5h","resetTime":"2999-01-01T00:00:00Z"},
            {"bucketId":"gemini-weekly","window":"weekly","resetTime":"2001-01-01T00:00:00Z"},
            {"bucketId":"3p-5h","window":"5h","disabled":true},
            {"bucketId":"3p-weekly","window":"weekly"}]}]}})",
        now, mono);
    const QuotaWindow* z5 = find(zero, "ag_5h");
    t.check(zero.ok && zero.windows.size() == 2 && z5 && z5->remaining_percent == 0 &&
                !z5->tiny_positive && !find(zero, "ag_week") && find(zero, "ag_3p_5h") &&
                zero.completeness == "partial",
            "ag: 零值省略——未过期 reset 佐证为 0%;reset 已过/无佐证 = 跳过;disabled = 0%");
    ParseResult edge = ParseAntigravityResponse(
        R"({"response":{"groups":[{"buckets":[
            {"bucketId":"gemini-5h","window":"5h","remainingFraction":0.004},
            {"bucketId":"gemini-weekly","window":"weekly","remainingFraction":1.2},
            {"bucketId":"gemini-argon-wtu-weekly","window":"weekly","remainingFraction":0.6},
            {"bucketId":"weird","remainingFraction":0.3},
            {"bucketId":"gemini-5h","window":"5h","remainingFraction":-1}]}]}})",
        now, mono);
    const QuotaWindow* e5 = find(edge, "ag_5h");
    const QuotaWindow* ew = find(edge, "ag_week");
    t.check(edge.ok && edge.windows.size() == 4 && e5 && e5->tiny_positive &&
                e5->remaining_percent == 0 && ew && ew->remaining_percent == 100,
            "ag: 0.4% → <1%（tiny_positive）;>1 钳到 100%;负值 = 无数据跳过");
    t.check(edge.windows.size() == 4 && edge.windows[2].stable_key == "ag_other_0" &&
                edge.windows[2].label == "7d余" &&
                edge.windows[2].duration_seconds == 7 * 86400 &&
                edge.windows[3].stable_key == "ag_other_1" &&
                edge.windows[3].period_kind == PeriodKind::Other &&
                edge.windows[3].label == "未知周期",
            "ag: 未映射池（argon 等）不猜语义——按 window 归周期,仅进详情");

    // ---- 解析器：错误体 ----
    ParseResult err = ParseAntigravityResponse(
        R"({"code":"unauthenticated","message":"unauthenticated: invalid CSRF token"})", now, mono);
    t.check(!err.ok && err.protocol_error, "ag: Connect 错误体（无 groups/buckets）→ 结构不兼容");
    t.check(!ParseAntigravityResponse("not json", now, mono).ok &&
                ParseAntigravityResponse("not json", now, mono).protocol_error,
            "ag: 非 JSON → 结构不兼容");
    t.check(ParseAntigravityResponse(R"({"response":{}})", now, mono).protocol_error,
            "ag: 空响应 → 结构不兼容");

    // ---- 配置：type=4 落盘契约 / 凭证版本恒 1 / 未知类型丢弃 / 默认集不含 ----
    AppConfig c;
    t.check(ConfigIo::Load(
                R"({"config_version":2,"display":{},"query":{},"notify":{},"account_seq":5,"accounts":[)"
                R"({"id":"acc5","type":4,"alias":"AG","enabled":true,"card_visible":false,"cred_rev":0,"alert_pct":10},)"
                R"({"id":"acc6","type":5}]})",
                c) &&
                c.accounts.size() == 1 && c.accounts[0].type == AccountType::Antigravity &&
                c.accounts[0].cred_rev == 1 && c.accounts[0].alert_pct == 10 &&
                !c.accounts[0].card_visible,
            "ag: type=4 读回为 antigravity,凭证版本恒 1;未知 type 条目丢弃");
    AppConfig back;
    t.check(ConfigIo::Load(ConfigIo::Dump(c), back) && back.accounts.size() == 1 &&
                back.accounts[0].type == AccountType::Antigravity && back.accounts[0].cred_rev == 1,
            "ag: 配置往返保留类型");
    bool no_default = true;
    for (const auto& a : AppConfig::Defaults())
        no_default = no_default && a.type != AccountType::Antigravity;
    t.check(no_default && AppConfig::Defaults().size() == 4,
            "ag: 默认账户集不含 antigravity（按需添加,避免「未运行」噪声卡）");

    // ---- 显示模型四路（常驻行/嵌入条/圆环/详情卡） ----
    AccountConfig ag;
    ag.id = "acc5";
    ag.type = AccountType::Antigravity;
    ag.alias = "Antigravity";
    ag.cred_rev = 1;
    ProviderSnapshot snap = MakeAntigravityTestSnapshot(now);
    // 标记为"与本次成功同批"的新鲜数据（否则 observed_mono=0 一律按旧数据打 †）
    for (auto& w : snap.windows) w.observed_mono = mono;
    snap.last_success_mono = mono;
    DisplayOptions opt;
    std::vector<AccountView> views = {{&ag, &snap}};
    auto flat_of = [&](const DisplayModel& m, size_t row) {
        std::wstring s;
        if (row < m.lines.size())
            for (const auto& sg : m.lines[row]) s += sg.text;
        return s;
    };
    std::wstring flat = flat_of(BuildDisplayModel(views, opt, now, mono, 300), 0);
    t.check(flat.find(L"Antigravity ") == 0 && flat.find(L"5h余 48%") != npos &&
                flat.find(L"7d余 86%") != npos &&
                flat.find(L"5h余 48%") < flat.find(L"7d余 86%") && flat.find(L"100%") == npos,
            "ag: 常驻行 = 5h 在前、周在后（与服务端数组序相反）;Claude&GPT 池不进常驻");
    ProviderSnapshot unconf = UnconfSnap(AccountType::Antigravity);
    t.check(flat_of(BuildDisplayModel({{&ag, &unconf}}, opt, now, mono, 300), 0) ==
                L"Antigravity 未运行",
            "ag: hub 未运行 → 「未运行」（不是「待配置」）");
    ProviderSnapshot off = snap;
    off.status = FetchStatus::Offline;
    // "旧值" = 距观测已超过 2× 刷新间隔（新鲜样本离线不打 †,规则同其余平台）
    off.last_success_mono = mono - 900000;
    for (auto& w : off.windows) w.observed_mono = mono - 900000;
    t.check(flat_of(BuildDisplayModel({{&ag, &off}}, opt, now, mono, 300), 0).find(L"†") != npos,
            "ag: 有旧值 + 离线 → 旧数据标注 †");
    ProviderSnapshot auth = unconf;
    auth.status = FetchStatus::AuthError;
    t.check(flat_of(BuildDisplayModel({{&ag, &auth}}, opt, now, mono, 300), 0) ==
                L"Antigravity 连接失效",
            "ag: 无旧值 + 连接失效 → 「连接失效」");

    auto rows = BuildEmbedItems(views, opt, now, mono, 300);
    std::wstring ev;
    if (rows.size() == 1)
        for (const auto& s : rows[0].values) ev += s.text;
    t.check(rows.size() == 1 && rows[0].name == L"Antigravity" &&
                rows[0].type == AccountType::Antigravity && ev.find(L"5h 48%") != npos &&
                ev.find(L"7d 86%") != npos,
            "ag: 嵌入条值行（标签去「余」,5h/7d）");

    auto rings = BuildRingItems(views, opt, now, mono, 300);
    t.check(rings.size() == 1 && rings[0].badge == L'A' && rings[0].arc_percent == 48 &&
                rings[0].center == L"48%" && rings[0].accent == aops.brand_color &&
                !rings[0].stale,
            "ag: 圆环 = Gemini 池 5h 窗,品牌色随分派表");
    auto rings_u = BuildRingItems({{&ag, &unconf}}, opt, now, mono, 300);
    t.check(rings_u.size() == 1 && rings_u[0].unconfigured, "ag: 未运行 → 灰环");

    auto cards = BuildDetailCards(views, now, 300);
    t.check(cards.size() == 1 && cards[0].badge == L'A' && !cards[0].uses_api_key &&
                cards[0].rows.size() == 2 && cards[0].rows[0].label == L"5h" &&
                cards[0].rows[0].value_text == L"48%" && cards[0].rows[1].label == L"7d" &&
                cards[0].rows[1].value_text == L"86%" && cards[0].rows[0].ring_percent == 48.0f,
            "ag: 详情卡两行 = Gemini 池 5h/7d（按 key 排序,非数组序）");
    t.check(cards.size() == 1 && cards[0].note.find(L"Claude/GPT 池") != npos &&
                cards[0].note.find(L"5h 100%") != npos && cards[0].note.find(L"周 100%") != npos &&
                cards[0].note.find(L"其它池") == npos,
            "ag: Claude&GPT 池额度以注解补全（数值区只放主池两窗）;无未映射池不出现「其它池」");
    // 未映射池进注解,不进数值区行
    ProviderSnapshot with_other = snap;
    QuotaWindow other;
    other.stable_key = "ag_other_0";
    other.label = "7d余";
    other.availability = Availability::Known;
    other.remaining_percent = 60;
    other.observed_mono = mono;
    with_other.windows.push_back(other);
    auto cards_o = BuildDetailCards({{&ag, &with_other}}, now, 300);
    t.check(cards_o.size() == 1 && cards_o[0].rows.size() == 2 &&
                cards_o[0].note.find(L"其它池：7d余 60%") != npos,
            "ag: 未映射池 → 注解列出,数值区仍只有 Gemini 池两行");
    auto cards_u = BuildDetailCards({{&ag, &unconf}}, now, 300);
    t.check(cards_u.size() == 1 && cards_u[0].unconfigured &&
                cards_u[0].status_text == L"未运行" && cards_u[0].rows.empty() &&
                cards_u[0].note.find(L"启动 Antigravity") != npos,
            "ag: 未运行卡给出可行动引导");

    // 历史采样：四条序列（Gemini/Claude&GPT 各 5h+周）,趋势取主窗 ag_5h
    UsageHistory hist;
    hist.accounts.push_back(AccountHistory{});
    hist.accounts[0].account_id = "acc5";
    RecordSnapshotSamples(hist.accounts[0], ag, snap, now);
    RecordSnapshotSamples(hist.accounts[0], ag, snap, now + 700);
    t.check(AccountHasTrend(hist, ag, snap) && hist.accounts[0].windows.size() == 4,
            "ag: 历史采样四序列,≥2 样本即有趋势");

    // 快照往返（比例字段 + resetTime 有/无）
    ProviderSnapshot sb;
    t.check(SnapshotJson::Parse(SnapshotJson::Dump(snap), sb) && sb.windows.size() == 4 &&
                sb.windows[0].stable_key == "ag_week" && sb.windows[0].has_reset &&
                sb.windows[1].stable_key == "ag_5h" && sb.windows[1].remaining_percent == 48 &&
                sb.windows[1].used_percent_raw == 52 && !sb.windows[2].has_reset &&
                sb.windows[3].stable_key == "ag_3p_5h" && sb.windows[3].remaining_percent == 100,
            "ag: 快照序列化往返保留比例与重置时刻");

    // ---- 布局：无 Key 类展开卡「重新读取」在卡内（回归:旧布局画到卡外 ~74 DIP） ----
    {
        const RECT content{240, 90, 1000, 700};
        auto valid = [](const RECT& rc) { return rc.right > rc.left && rc.bottom > rc.top; };
        int sc = 0;
        ui::QuotaPageIn in;
        ui::QuotaCardIn cn;
        cn.expanded = true;
        cn.uses_api_key = false;
        cn.alert_capable = true;
        in.cards.push_back(cn);
        auto L = ui::ComputeQuotaLayout(in, content, 96, sc);
        const ui::QuotaCardR& cr = L.cr[0];
        t.check(valid(cr.ex_codex_sync) && valid(cr.ex_alert) &&
                    cr.ex_codex_sync.right <= L.cards[0].right - 24 &&
                    cr.ex_codex_sync.left >= cr.ex_key.right &&
                    cr.ex_key.right < L.cards[0].right - 24,
                "ag: 凭证行「重新读取」在卡内,状态框让位");
        ui::QuotaPageIn in2;
        ui::QuotaCardIn ck;
        ck.expanded = true;
        in2.cards.push_back(ck);
        auto L2 = ui::ComputeQuotaLayout(in2, content, 96, sc);
        t.check(!valid(L2.cr[0].ex_codex_sync) && L2.cr[0].ex_key.right == L2.cards[0].right - 24,
                "ag: API Key 卡无「重新读取」,输入框占满");
        ui::QuotaPageIn in3;
        in3.cards.push_back({});
        in3.add_open = true;
        auto L3 = ui::ComputeQuotaLayout(in3, content, 96, sc);
        bool chips = L3.ap_type[0].left >= L3.add_panel.left;
        for (int i = 0; i < ui::kAddTypeCount; ++i) {
            chips = chips && valid(L3.ap_type[i]) && L3.ap_type[i].right <= L3.add_panel.right;
            if (i)
                chips = chips && L3.ap_type[i].left > L3.ap_type[i - 1].right &&
                         L3.ap_type[i].right - L3.ap_type[i].left ==
                             L3.ap_type[i - 1].right - L3.ap_type[i - 1].left;
        }
        t.check(chips, "ag: 添加面板五枚 chip 等宽、不重叠、不出面板");
    }

    // ---- 刷新回显：hub 未运行不谎报「已开始刷新」 ----
    int kind = -1;
    std::wstring text;
    auto sum = SummarizeManualKicks({ManualKick::Started, ManualKick::SourceOffline});
    ManualKickEchoText(sum, &kind, &text);
    t.check(sum.started == 1 && sum.source_offline == 1 && kind == 1 &&
                text.find(L"1 张数据源未运行") != npos,
            "ag: 刷新回显——已启动 + 未运行尾注");
    sum = SummarizeManualKicks({ManualKick::SourceOffline});
    ManualKickEchoText(sum, &kind, &text);
    t.check(kind == 0 && text == L"Antigravity 未运行：启动并登录后自动连接",
            "ag: hub 未运行时刷新回显指引");
}

// ---------- 请求头 User-Agent 与版本资源同源 ----------
// 2026-11-27 修:请求头此前靠从未被构建定义的 APP_VERSION 宏拼版本,实际一直是兜底值
// QuotaBar/1.0.0——各平台访问日志里分不出版本。
void TestUserAgent(T& t) {
    const std::wstring ua = HttpUserAgent();
    t.check(ua.rfind(L"QuotaBar/", 0) == 0 && ua.size() > 9,
            "ua: 以 QuotaBar/ 开头且带版本段");
    const std::wstring& v = ExeVersion();
    if (v.empty()) {
        // --selftest 的 exe 正常都带版本资源（quota_bar.rc 经 build.bat 链入）;资源缺失时
        // 只验兜底不崩溃
        t.check(ua == L"QuotaBar/0.0.0", "ua: 无版本资源 → 明确的 0.0.0 兜底（不冒充 1.0.0）");
        return;
    }
    bool shape = true;
    int dots = 0;
    for (wchar_t c : v) {
        if (c == L'.') ++dots;
        else if (c < L'0' || c > L'9') shape = false;
    }
    t.check(shape && dots == 2, "ua: 版本资源读数为三段数字（取 FileVersion 前三段）");
    // 核心断言:请求头就是 QuotaBar/ + 版本资源,代码里不另写一份版本
    t.check(ua == L"QuotaBar/" + v, "ua: User-Agent = QuotaBar/ + exe 版本资源");
    t.check(ua != L"QuotaBar/1.0.0", "ua: 不再是 2026-11-27 之前的硬编码兜底值 QuotaBar/1.0.0");
}

// ---------- 序列化 roundtrip（内存，不写盘） ----------
void TestSnapshotRoundtrip(T& t) {
    ProviderSnapshot s = MakeKimiTestSnapshot(NowUnix());
    s.error_text = "示例错误";
    ProviderSnapshot s2;
    t.check(SnapshotJson::Parse(SnapshotJson::Dump(s), s2), "snapshot: dump→parse 成功");
    t.check(s2.windows.size() == s.windows.size() &&
                s2.windows[0].stable_key == "kimi_weekly" &&
                s2.windows[0].remaining_percent == 62,
            "snapshot: 窗口内容保留");
    ProviderSnapshot ds = MakeDeepSeekTestSnapshot(NowUnix());
    ProviderSnapshot ds2;
    t.check(SnapshotJson::Parse(SnapshotJson::Dump(ds), ds2) &&
                ds2.balances.size() == 1 && ds2.balances[0].total_raw == "23.48",
            "snapshot: DeepSeek 金额原文保留（不经浮点）");
}

// ---------- 调度纯逻辑 ----------
void TestSchedulerBasics(T& t) {
    t.check(BackoffSeconds(0) == 0 && BackoffSeconds(1) == 300 &&
                BackoffSeconds(2) == 600 && BackoffSeconds(3) == 1200 &&
                BackoffSeconds(4) == 1800 && BackoffSeconds(9) == 1800,
            "sched: 退避 5/10/20/30 后封顶 30 分钟");
    for (int i = 0; i < 200; ++i) {
        int64_t j = JitterSeconds(600, 10);
        if (j < 0 || j > 60) {
            t.check(false, "sched: 抖动 ≤10%");
            return;
        }
    }
    t.check(true, "sched: 抖动范围 0–10%");

    // 本机 hub 类（antigravity）永不停机:401（hub 重启换了 CSRF 令牌）/连接被拒/坏响应都走
    // ~60s 重探,而非通用退避或停机等用户——hub 随 Antigravity 启停,状态必须自愈
    struct HubHost : ISchedulerHost {
        AppConfig cfg;
        ProviderSnapshot snap;
        AppConfig& Cfg() override { return cfg; }
        size_t AccountCount() override { return cfg.accounts.size(); }
        const AccountConfig& AccountAt(size_t i) override { return cfg.accounts[i]; }
        ProviderSnapshot* FindSnap(const std::string& id) override {
            return id == "acc9" ? &snap : nullptr;
        }
        std::string GetKey(const AccountConfig&) override { return "csrf-token"; }
        std::string GetAccountId(const AccountConfig&) override { return {}; }
        std::string GetEndpointBase(const AccountConfig&) override {
            return "http://127.0.0.1:48342";
        }
        void OnSnapshotsChanged(bool) override {}
        void OnNotifyThreshold(const AccountConfig&) override {}
        bool IsShuttingDown() const override { return false; }
    } hub;
    AccountConfig hub_acc;
    hub_acc.id = "acc9";
    hub_acc.type = AccountType::Antigravity;
    hub_acc.cred_rev = 1;
    hub.cfg.accounts.push_back(hub_acc);
    auto reset_hub_snap = [&] {
        hub.snap = ProviderSnapshot{};
        hub.snap.provider_id = kProviderAntigravity;
        hub.snap.account_id = "acc9";
        hub.snap.credential_revision = 1;
        hub.snap.status = FetchStatus::Fetching;
    };
    auto reply = [](Scheduler& sch, int status, bool net_err, const char* body) {
        HttpResponse r;
        r.provider_id = kProviderAntigravity;
        r.account_id = "acc9";
        r.credential_revision = 1;
        r.http_status = status;
        r.network_error = net_err;
        if (body) r.body = body;
        sch.OnResponse(std::move(r));
    };
    Scheduler hs;
    hs.Init(&hub, nullptr, nullptr);
    hs.OnSettingsChanged(); // 同步调度表
    auto reprobe_soon = [&] {
        const int64_t ms = hs.NextDeadlineMs();
        return ms >= 50000 && ms <= 70000; // 60s ±10% 抖动（通用退避首档就是 300s）
    };
    reset_hub_snap();
    reply(hs, 401, false, nullptr);
    t.check(hub.snap.status == FetchStatus::AuthError &&
                hub.snap.error_text.find("自动重连") != std::string::npos && reprobe_soon(),
            "sched: hub 401 → 不断言凭证失效、不停机,~60s 重探");
    reset_hub_snap();
    reply(hs, 0, true, nullptr);
    t.check(hub.snap.status == FetchStatus::Offline &&
                hub.snap.error_text.find("无响应") != std::string::npos && reprobe_soon(),
            "sched: hub 连接被拒 → Offline + ~60s 重探（非 5 分钟退避）");
    reset_hub_snap();
    reply(hs, 200, false, "{}");
    t.check(hub.snap.status == FetchStatus::ProtocolError && reprobe_soon(),
            "sched: hub 坏响应 → ProtocolError 但不停机");
    reset_hub_snap();
    std::string real = AgBodyReal();
    reply(hs, 200, false, real.c_str());
    t.check(hub.snap.status == FetchStatus::Ok && hub.snap.windows.size() == 4 &&
                hub.snap.contract_revision == kAntigravityContract &&
                hub.snap.completeness == "full" && hs.NextDeadlineMs() > 70000,
            "sched: hub 成功 → 发布快照 + 契约版本,下次按正常刷新间隔");

    // 跨账户隔离：旧凭证版本的响应被丢弃
    struct FakeHost : ISchedulerHost {
        AppConfig cfg;
        ProviderSnapshot snap;
        bool changed = false;
        AppConfig& Cfg() override { return cfg; }
        size_t AccountCount() override { return cfg.accounts.size(); }
        const AccountConfig& AccountAt(size_t i) override { return cfg.accounts[i]; }
        ProviderSnapshot* FindSnap(const std::string& id) override {
            return id == "kimi" ? &snap : nullptr;
        }
        std::string GetKey(const AccountConfig&) override { return "test-key"; }
        std::string GetAccountId(const AccountConfig&) override { return {}; }
        std::string GetEndpointBase(const AccountConfig&) override { return {}; }
        void OnSnapshotsChanged(bool /*dirty*/) override { changed = true; }
        void OnNotifyThreshold(const AccountConfig&) override {}
        bool IsShuttingDown() const override { return false; }
    };
    FakeHost host;
    AccountConfig acc;
    acc.id = "kimi";
    acc.type = AccountType::Kimi;
    acc.cred_rev = 7;
    host.cfg.accounts.push_back(acc);
    host.snap.provider_id = kProviderKimi;
    host.snap.account_id = "kimi";
    host.snap.credential_revision = 6;
    host.snap.status = FetchStatus::Fetching; // 已配置 → 查询中
    Scheduler s;
    s.Init(&host, nullptr, nullptr);
    s.OnSettingsChanged(); // 同步账户调度表（此后 changed 复位再断言）
    host.changed = false;
    HttpResponse old;
    old.request_id = 1;
    old.provider_id = kProviderKimi;
    old.account_id = "kimi";
    old.credential_revision = 5; // 旧版本 → 必须丢弃
    old.http_status = 200;
    old.body = KimiBody100_38();
    s.OnResponse(std::move(old));
    t.check(!host.changed && host.snap.status == FetchStatus::Fetching,
            "sched: 旧凭证版本响应被丢弃（不串账户）");

    // 401 → AuthError + 停止自动重试；详情文本反映
    HttpResponse bad;
    bad.request_id = 2;
    bad.provider_id = kProviderKimi;
    bad.account_id = "kimi";
    bad.credential_revision = 7;
    bad.http_status = 401;
    s.OnResponse(std::move(bad));
    t.check(host.snap.status == FetchStatus::AuthError &&
                host.snap.error_text.find("凭证") != std::string::npos,
            "sched: 401 → 凭证错误");
    // 429（无 Retry-After）→ 退避，next_allowed 写回快照
    host.snap = ProviderSnapshot{};
    host.snap.provider_id = kProviderKimi;
    host.snap.account_id = "kimi";
    host.snap.credential_revision = 7;
    HttpResponse lim;
    lim.request_id = 3;
    lim.provider_id = kProviderKimi;
    lim.account_id = "kimi";
    lim.credential_revision = 7;
    lim.http_status = 429;
    s.OnResponse(std::move(lim));
    t.check(host.snap.status == FetchStatus::QueryRateLimited &&
                host.snap.next_allowed_mono > NowMono(),
            "sched: 429 → 受限且详情可显示下次允许时间");
    // 账户删除后的迟到响应：直接丢弃（不崩溃、不复活调度）
    host.snap = ProviderSnapshot{};
    host.snap.account_id = "kimi";
    host.cfg.accounts.clear();
    HttpResponse ghost;
    ghost.request_id = 4;
    ghost.provider_id = kProviderKimi;
    ghost.account_id = "kimi";
    ghost.credential_revision = 7;
    ghost.http_status = 200;
    ghost.body = KimiBody100_38();
    s.OnResponse(std::move(ghost));
    t.check(host.snap.account_id == "kimi" && host.snap.status == FetchStatus::Unconfigured,
            "sched: 账户已删 → 迟到响应丢弃");
}

// ---------- 任务栏内嵌槽纯逻辑（display.mode=Embed；不建窗口） ----------
// 假测量：宽度 = 字符数 × 10
float FakeSlotWidth(const std::wstring& s, void*) {
    return (float)(s.size() * 10);
}

void TestSlotGeom(T& t) {
    // 底部水平任务栏（主屏 3840×2160 物理，任务栏高 96）：托盘在右端
    const RECT host{0, 2064, 3840, 2160};
    const RECT tray{3400, 2064, 3840, 2160};
    SlotLayout lay = ComputeSlotLayout(host, tray, 200, 20, 6, 40);
    t.check(lay.placement == SlotPlacement::LeftOfTray && lay.rc.left == 3194 &&
                lay.rc.right == 3394 && lay.rc.top == 2102 && lay.rc.bottom == 2122 &&
                lay.avail_w == 3394,
            "slot: 常规左贴托盘（右缘=tray.left-6，垂直居中）");
    // 托盘贴左缘：左侧无空间 → 翻到托盘右侧
    const RECT tray_left{0, 2064, 40, 2160};
    lay = ComputeSlotLayout(host, tray_left, 200, 20, 6, 40);
    t.check(lay.placement == SlotPlacement::RightOfTray && lay.rc.left == 46 &&
                lay.rc.right == 246 && lay.avail_w == 3794,
            "slot: 托盘贴左缘 → 翻到右侧");
    // 两侧都放不下整宽：选可用宽较大侧落位，宽=可用宽
    const RECT host_narrow{0, 2064, 400, 2160};
    const RECT tray_mid{100, 2064, 300, 2160};
    lay = ComputeSlotLayout(host_narrow, tray_mid, 500, 20, 6, 40);
    t.check(lay.placement == SlotPlacement::LeftOfTray && lay.avail_w == 94 &&
                (lay.rc.right - lay.rc.left) == 94,
            "slot: 放不下整宽 → 可用宽侧落位（调用方裁切后复算）");
    // 两侧可用宽都低于最小可用宽 → NoSpace
    lay = ComputeSlotLayout(host_narrow, tray_mid, 30, 20, 6, 200);
    t.check(lay.placement == SlotPlacement::NoSpace, "slot: 两侧均低于最小可用宽 → NoSpace");
    // 垂直任务栏 / 空宿主 / 内容过高 → NoSpace
    const RECT vertical{0, 0, 96, 3840};
    lay = ComputeSlotLayout(vertical, RECT{0, 0, 96, 40}, 200, 20, 6, 40);
    t.check(lay.placement == SlotPlacement::NoSpace, "slot: 垂直任务栏 → NoSpace");
    lay = ComputeSlotLayout(RECT{}, tray, 200, 20, 6, 40);
    t.check(lay.placement == SlotPlacement::NoSpace, "slot: 空宿主 → NoSpace");
    lay = ComputeSlotLayout(host, tray, 200, 200, 6, 40);
    t.check(lay.placement == SlotPlacement::NoSpace, "slot: 内容高于宿主 → NoSpace");
    // 内容贴齐宿主左缘（无溢出）
    lay = ComputeSlotLayout(host, tray, 3394, 20, 6, 40);
    t.check(lay.placement == SlotPlacement::LeftOfTray && lay.rc.left == 0,
            "slot: 整宽贴齐宿主左缘");
    // 可见性：≥ 一半面积在监视器内才可见
    const RECT mon{0, 0, 3840, 2160};
    t.check(HostVisible(RECT{0, 2064, 3840, 2160}, mon), "slot: 宿主完整可见");
    t.check(!HostVisible(RECT{0, 2130, 3840, 2220}, mon), "slot: 宿主大部移出屏外 → 不可见");
    t.check(!HostVisible(RECT{0, 3840, 3840, 4000}, mon), "slot: 宿主完全离屏 → 不可见");
}

void TestSlotText(T& t) {
    const std::wstring ell = L"…";
    std::vector<DisplaySegment> in;
    auto mk = [&](const wchar_t* s) { in.push_back(DisplaySegment{s, RGB(255, 255, 255)}); };
    mk(L"abcd"); // 40
    mk(L"12345678"); // 80
    mk(L"wxyz"); // 40
    auto out = TruncateSlotSegs(in, 1000, 0.0f, ell, FakeSlotWidth, nullptr);
    t.check(out.size() == 3 && out[2].text == L"wxyz", "slottext: 放得下 → 原样返回");
    // 需要丢尾段并对次段截短加省略号
    out = TruncateSlotSegs(in, 100, 0.0f, ell, FakeSlotWidth, nullptr);
    t.check(out.size() == 2 && out[0].text == L"abcd" && out[1].text == L"12345…",
            "slottext: 超宽 → 前缀整段 + 次段截短加省略号");
    // 段间有间隙时截短宽度预算先扣间隙
    out = TruncateSlotSegs(in, 100, 5.0f, ell, FakeSlotWidth, nullptr);
    t.check(out.size() == 2 && out[1].text == L"1234…", "slottext: 间隙计入预算");
    // 首段过宽：只截首段
    std::vector<DisplaySegment> one;
    one.push_back(DisplaySegment{L"abcdefgh", RGB(255, 255, 255)});
    out = TruncateSlotSegs(one, 30, 0.0f, ell, FakeSlotWidth, nullptr);
    t.check(out.size() == 1 && out[0].text == L"ab…", "slottext: 首段超宽截短");
    // 连省略号都放不下 → 空
    out = TruncateSlotSegs(one, 8, 0.0f, ell, FakeSlotWidth, nullptr);
    t.check(out.empty(), "slottext: 放不下省略号 → 空");
    // 空输入
    out = TruncateSlotSegs({}, 100, 0.0f, ell, FakeSlotWidth, nullptr);
    t.check(out.empty(), "slottext: 空输入 → 空");
}

void TestSlotPalette(T& t) {
    t.check(ResolveSlotDarkText(0, true) && !ResolveSlotDarkText(0, false),
            "slotpal: 自动 → 浅色主题黑字/深色主题白字（与浮窗相反）");
    t.check(!ResolveSlotDarkText(1, true) && !ResolveSlotDarkText(1, false),
            "slotpal: 显式白字恒定");
    t.check(ResolveSlotDarkText(2, true) && ResolveSlotDarkText(2, false),
            "slotpal: 显式黑字恒定");
    // 字号约束
    t.check(ClampSlotFontPx(24, 96, 6, 10) == 24, "slotpal: 常规字号不裁剪");
    t.check(ClampSlotFontPx(90, 96, 6, 10) == 76, "slotpal: 超宿主高 → 收到上限");
    t.check(ClampSlotFontPx(24, 20, 6, 10) == 0, "slotpal: 宿主装不下最小字号 → 0");
}

// ---------- 圆环条目（Rings 模式） ----------
void TestRingItems(T& t) {
    int64_t now = NowUnix(), mono = NowMono();
    DisplayOptions opt;
    ProviderSnapshot k = MakeKimiTestSnapshot(now);
    ProviderSnapshot d = MakeDeepSeekTestSnapshot(now);
    ProviderSnapshot c = MakeCodexTestSnapshot(now);
    ProviderSnapshot g = MakeGlmTestSnapshot(now);
    auto accs = FixtureAccounts();
    ProviderSnapshot snaps0[4] = {k, c, d, g};
    auto rings = BuildRingItems(ViewsOf(accs, snaps0), opt, now, mono, 300);
    t.check(rings.size() == 4, "rings: 四账户四环");
    t.check(rings[0].badge == L'K' && rings[1].badge == L'C' && rings[2].badge == L'D' &&
                rings[3].badge == L'G',
            "rings: 徽章字母 K/C/D/G（卡序=视图序）");
    // 百分比环:取 5h 窗口(夹具 81%)环心带 %
    t.check(rings[0].arc_percent == 81 && rings[0].center == L"81%" && !rings[0].full_ring,
            "rings: Kimi 环=5h 剩余 81%");
    // 金额环:满环 + 金额文本(≥10 取整数,环内空间有限;精确值看详情)
    t.check(rings[2].full_ring && rings[2].arc_percent == -1 &&
                rings[2].center == L"¥23",
            "rings: DeepSeek 满环 + 金额");
    t.check(rings[1].arc_percent == 70 && rings[3].arc_percent == 70,
            "rings: Codex/GLM 环 70%");
    // 未配置 → 灰环占位
    ProviderSnapshot uncs[4] = {UnconfSnap(AccountType::Kimi), UnconfSnap(AccountType::Codex),
                                UnconfSnap(AccountType::Deepseek),
                                UnconfSnap(AccountType::Glm)};
    auto r2 = BuildRingItems(ViewsOf(accs, uncs), opt, now, mono, 300);
    t.check(r2.size() == 4 && r2[0].unconfigured && r2[0].center == L"—",
            "rings: 未配置 → 灰环");
    // 低额:余额 ≤ 阈值(5) → 琥珀("¥2.07" 5 字符保留两位小数)
    ProviderSnapshot dlow = d;
    dlow.balances[0].total_raw = "2.07";
    ProviderSnapshot snaps_low[4] = {k, c, dlow, g};
    auto r4 = BuildRingItems(ViewsOf(accs, snaps_low), opt, now, mono, 300);
    t.check(r4[2].low && r4[2].value_color == RGB(255, 176, 32),
            "rings: 余额低于阈值 → 琥珀");
    t.check(r4[2].center == L"¥2.07", "rings: <¥10 保留两位小数");
    // 旧数据 → stale 标记(overlay 据此半透明)
    k.from_cache = true;
    ProviderSnapshot snaps_st[4] = {k, c, d, g};
    auto r3 = BuildRingItems(ViewsOf(accs, snaps_st), opt, now, mono, 300);
    t.check(r3[0].stale, "rings: 缓存/离线 → stale");
}

// 字号阶梯约束：档位单调、辅助文字下限 ≥12（视觉规范「文字大小不一」的护栏；
// 主窗口已用 TypeSize 枚举参数编译期禁绝裸字号，这里守 token 本身的取值）
void TestTypeRamp(T& t) {
    const float hero = theme::TypeSizeDip(theme::TypeSize::Hero);
    const float num = theme::TypeSizeDip(theme::TypeSize::Num);
    const float head = theme::TypeSizeDip(theme::TypeSize::Head);
    const float body = theme::TypeSizeDip(theme::TypeSize::Body);
    const float cap = theme::TypeSizeDip(theme::TypeSize::Cap);
    t.check(hero > num && num > head && head > body,
            "ramp: Hero > Num > Head > Body 单调递减");
    t.check(body >= 12.0f && cap >= 12.0f, "ramp: 辅助文字下限 ≥ 12 dip");
    t.check(theme::TypeSizeDip(static_cast<theme::TypeSize>(99)) == body,
            "ramp: 越界枚举回退 Body");
}

// ---------- 用量历史：采样/去重/清理（A 迭代） ----------
void TestHistorySampling(T& t) {
    AccountConfig acc;
    acc.id = "acc1";
    acc.type = AccountType::Kimi;
    ProviderSnapshot s = MakeKimiTestSnapshot(NowUnix());
    s.status = FetchStatus::Ok;
    AccountHistory h;
    RecordSnapshotSamples(h, acc, s, 1000);
    RecordSnapshotSamples(h, acc, s, 1000 + 60); // 同值 10 分钟内 → 去重
    bool deduped = true;
    for (const auto& w : h.windows)
        for (const auto& smp : w.samples)
            if (smp.t_utc == 1000 + 60) deduped = false;
    t.check(!h.windows.empty() && deduped, "hist: 同值 10 分钟内采样去重");
    t.check(h.account_id == "acc1", "hist: 采样归属账户");

    // 值变化 → 记录新样本
    ProviderSnapshot s2 = s;
    for (auto& w : s2.windows)
        if (w.remaining_percent > 0) w.remaining_percent = (int32_t)(w.remaining_percent / 2);
    RecordSnapshotSamples(h, acc, s2, 1000 + 3600);
    int total = 0;
    for (const auto& w : h.windows) total += (int)w.samples.size();
    t.check(total >= 2, "hist: 值变化记录新样本");
    // Purge：保留新鲜样本（含 30 天保留期截断；样本时间 ≈1000s 与 4600s 均属"新鲜"）
    UsageHistory store;
    store.accounts.push_back(h);
    store.Purge(1000 + 3600);
    total = 0;
    for (const auto& w : store.accounts[0].windows) total += (int)w.samples.size();
    t.check(total >= 2, "hist: Purge 保留新鲜样本");
}

void TestDepletionEstimate(T& t) {
    const int64_t now = NowUnix();
    // 线性消耗：80→50 用 1.5h（-20%/h），预计 50/20 = 2.5h = 9000s
    std::vector<UsageSample> drain = {
        {now - 5400, 80}, {now - 3600, 70}, {now - 1800, 60}, {now, 50}};
    auto e = EstimateDepletion(drain, now);
    t.check(e.valid && e.pct_per_hour < -19.0 && e.pct_per_hour > -21.0,
            "eta: 线性消耗速率 ≈ -20%/h");
    t.check(e.eta_seconds > 8900 && e.eta_seconds < 9100, "eta: 预计 2.5 小时用尽");
    // 平稳 → 不估
    std::vector<UsageSample> flat = {{now - 3600, 50}, {now - 1800, 50}, {now, 50}};
    t.check(!EstimateDepletion(flat, now).valid, "eta: 平稳序列不估用尽");
    // 净恢复 → 不估
    std::vector<UsageSample> rise = {{now - 3600, 40}, {now - 1800, 45}, {now, 50}};
    t.check(!EstimateDepletion(rise, now).valid, "eta: 净恢复不估用尽");
    // 样本不足 / 跨度过短 → 不估
    std::vector<UsageSample> few = {{now - 600, 60}, {now, 50}};
    t.check(!EstimateDepletion(few, now).valid, "eta: 样本不足不估");
    std::vector<UsageSample> short_span = {
        {now - 300, 60}, {now - 200, 55}, {now, 50}};
    t.check(!EstimateDepletion(short_span, now).valid, "eta: 跨度 <20 分钟不估");
}

void TestTodaySpend(T& t) {
    // 用「今日本地 0 点」合成时间戳，任何时刻跑测试都成立
    time_t tt = (time_t)NowUnix();
    struct tm ltm{};
    localtime_s(&ltm, &tt);
    ltm.tm_hour = 0;
    ltm.tm_min = 0;
    ltm.tm_sec = 0;
    const int64_t mid = (int64_t)mktime(&ltm);
    BalanceHistory b;
    b.currency = "CNY";
    b.samples = {{mid + 60, "100.00"}, {mid + 3600, "95.50"}, {mid + 7200, "88.00"}};
    std::string spend;
    t.check(TodaySpend(b, mid + 7200, spend) && spend == "12.00",
            "spend: 今日消耗 100.00→88.00 = 12.00");
    // 净增加（充值）→ 不显示
    BalanceHistory b2;
    b2.currency = "CNY";
    b2.samples = {{mid + 60, "50.00"}, {mid + 3600, "80.00"}};
    t.check(!TodaySpend(b2, mid + 3600, spend), "spend: 净增加（充值）不显示");
    // 样本不足 → 不显示
    BalanceHistory b3;
    b3.samples = {{mid + 60, "50.00"}};
    t.check(!TodaySpend(b3, mid + 3600, spend), "spend: 样本不足不显示");
}

void TestHistoryJsonRoundtrip(T& t) {
    UsageHistory h;
    AccountHistory a;
    a.account_id = "acc9";
    a.windows.push_back({"kimi_rolling_18000s", {{1000, 62}, {5000, 50}}});
    a.balance.currency = "CNY";
    a.balance.samples = {{1000, "23.45"}, {5000, "20.10"}};
    h.accounts.push_back(a);
    AccountHistory cx; // codex 段：身份锚随盘
    cx.account_id = "codex";
    cx.identity_key = "0123456789abcdef";
    cx.windows.push_back({"codex_5h", {{1000, 80}, {5000, 71}}});
    h.accounts.push_back(cx);
    std::string text = HistoryJson::Dump(h, 9999);
    UsageHistory out;
    int64_t saved = 0;
    t.check(HistoryJson::Parse(text, out, saved), "histjson: Dump→Parse 成功");
    t.check(saved == 9999 && out.accounts.size() == 2 &&
                out.accounts[0].account_id == "acc9",
            "histjson: 账户与保存时刻一致");
    t.check(out.accounts[0].windows.size() == 1 &&
                out.accounts[0].windows[0].stable_key == "kimi_rolling_18000s" &&
                out.accounts[0].windows[0].samples.size() == 2 &&
                out.accounts[0].windows[0].samples[1].pct == 50,
            "histjson: 窗口样本 roundtrip 一致");
    t.check(out.accounts[0].balance.currency == "CNY" &&
                out.accounts[0].balance.samples[1].total_raw == "20.10",
            "histjson: 余额样本 roundtrip 一致");
    t.check(out.accounts[1].identity_key == "0123456789abcdef" &&
                out.accounts[1].windows[0].stable_key == "codex_5h" &&
                out.accounts[1].windows[0].samples[1].pct == 71,
            "histjson: codex 身份锚与样本 roundtrip 一致");
    // 旧版文件（无 ik 字段）→ identity_key 为空，可正常加载
    std::string legacy =
        "{\"version\":1,\"saved_at_utc\":1,\"accounts\":[{\"id\":\"codex\","
        "\"windows\":[{\"key\":\"codex_5h\",\"samples\":[[1,90]]}]}]}";
    UsageHistory out2;
    t.check(HistoryJson::Parse(legacy, out2, saved) &&
                out2.accounts.size() == 1 && out2.accounts[0].identity_key.empty() &&
                out2.accounts[0].windows[0].samples[0].pct == 90,
            "histjson: 旧版无 ik 字段 → 空键兼容加载");
}

// ---------- codex 历史身份锚：键建立/一致/换账户清段 ----------
void TestIdentityKeyIsolation(T& t) {
    // Fnv1a64Hex：确定性、可区分、定长 16 hex、空输入 → 空
    t.check(Fnv1a64Hex("acct-a") == Fnv1a64Hex("acct-a") &&
                Fnv1a64Hex("acct-a") != Fnv1a64Hex("acct-b") &&
                Fnv1a64Hex("acct-a").size() == 16 && Fnv1a64Hex("").empty(),
            "idk: 哈希确定性/可区分/定长/空入空出");
    AccountHistory h;
    h.account_id = "codex";
    h.identity_key = Fnv1a64Hex("acct-a");
    h.windows.push_back({"codex_5h", {{1000, 60}, {2000, 55}}});
    h.balance.currency = "CNY"; // codex 不应留余额序列，清段后须为空
    h.balance.samples.push_back({1000, "1.00"});
    t.check(!ApplyIdentityKey(h, h.identity_key), "idk: 键一致不清段");
    t.check(h.windows.size() == 1 && h.windows[0].samples.size() == 2,
            "idk: 键一致数据保留");
    t.check(ApplyIdentityKey(h, "ffffffffffffffff"), "idk: 键变化触发清段");
    t.check(h.identity_key == "ffffffffffffffff" && h.windows.empty() &&
                h.balance.samples.empty() && h.balance.currency.empty(),
            "idk: 换账户后序列清空、新键生效");
    // 空键无操作（调用方在锚不上时不采样，双保险）
    t.check(!ApplyIdentityKey(h, "") && h.identity_key == "ffffffffffffffff",
            "idk: 空 key 无操作");
    // 旧版磁盘段（无键有数据）：首次补锚、数据保留、不算清段
    AccountHistory legacy;
    legacy.account_id = "codex";
    legacy.windows.push_back({"codex_5h", {{1000, 60}}});
    t.check(!ApplyIdentityKey(legacy, "aaaaaaaaaaaaaaaa") &&
                legacy.identity_key == "aaaaaaaaaaaaaaaa" &&
                legacy.windows.size() == 1,
            "idk: 旧段无键 → 补锚且数据保留");
}

// ---------- 卡片拖拽排序（F3）：可见元素移动、隐藏元素原位 ----------
void TestMoveVisible(T& t) {
    auto mk = [](const char* id, bool vis) {
        AccountConfig a;
        a.id = id;
        a.card_visible = vis;
        return a;
    };
    std::vector<AccountConfig> v = {mk("a", true), mk("b", false), mk("c", true),
                                    mk("d", true)};
    // 可见序 [a,c,d]：把第 0 个可见（a）移到可见位 2 → 可见序 [c,d,a]，b 原位
    MoveVisibleInVector(v, 0, 2, [](const AccountConfig& a) { return a.card_visible; });
    t.check(v.size() == 4 && v[0].id == "c" && v[1].id == "b" && v[2].id == "d" &&
                v[3].id == "a",
            "move: 可见位 0→2，隐藏元素原位保留");
    // 相邻移动：[c,b,d,a] 可见序 [c,d,a] 中把 d（位 1）移到 0 → 可见序 [d,c,a]
    MoveVisibleInVector(v, 1, 0, [](const AccountConfig& a) { return a.card_visible; });
    t.check(v[0].id == "d" && v[1].id == "b" && v[2].id == "c" && v[3].id == "a",
            "move: 相邻前移生效");
    // 目标越界钳制到末位：可见序 [d,c,a] 把 d（位 0）移到 99→末位 → 可见序 [c,a,d]
    MoveVisibleInVector(v, 0, 99, [](const AccountConfig& a) { return a.card_visible; });
    t.check(v[0].id == "c" && v[2].id == "a" && v[3].id == "d",
            "move: 目标越界钳制到末位");
}

// ---------- 双主题调色板（F1）：两套 token 可切换且关键色不重合 ----------
void TestThemePalettes(T& t) {
    theme::SetUiTheme(true);
    const COLORREF d_bg = theme::kSbg, d_text = theme::kSOnSurf, d_prim = theme::kSPrimary;
    t.check(theme::UiIsDark(), "palette: 默认深色");
    theme::SetUiTheme(false);
    t.check(!theme::UiIsDark(), "palette: 可切浅色");
    t.check(theme::kSbg != d_bg && theme::kSOnSurf != d_text &&
                theme::kSPrimary != d_prim,
            "palette: 浅色关键 token 与深色不重合");
    // 恢复深色（进程其余部分假定深色基线）
    theme::SetUiTheme(true);
    t.check(theme::UiIsDark() && theme::kSbg == d_bg, "palette: 恢复深色一致");
}

// ---------- 手动刷新回显汇总（第十一轮：刷新被拒不再静默） ----------
void TestRefreshFeedback(T& t) {
    int kind = -1;
    std::wstring text;
    // 全部启动：成功口径；多张附拒绝尾注
    ManualKickSummary s = SummarizeManualKicks(
        {ManualKick::Started, ManualKick::Started, ManualKick::Started});
    ManualKickEchoText(s, &kind, &text);
    t.check(kind == 1 && text == L"已开始刷新 3 张卡片", "kick: 全部启动→成功文案");
    // 启动 + 节流混合：成功为主，尾注提示被跳过的张数
    s = SummarizeManualKicks({ManualKick::Started, ManualKick::Throttled});
    ManualKickEchoText(s, &kind, &text);
    t.check(kind == 1 && text == L"已开始刷新 1 张卡片，1 张刚刷新过",
            "kick: 混合节流→成功+尾注");
    // 全部节流（60s 手动间隔）
    s = SummarizeManualKicks({ManualKick::Throttled, ManualKick::Throttled});
    ManualKickEchoText(s, &kind, &text);
    t.check(kind == 0 && text == L"刚刚已刷新过，请 1 分钟后再试", "kick: 全节流→稍后再试");
    // 限频：Retry-After 剩余秒数取整为分钟
    s = SummarizeManualKicks({ManualKick::RateLimited, ManualKick::Started});
    t.check(s.started == 1 && s.rate_limited == 1 && s.total == 2, "kick: 混合限频计数");
    s.rate_limited = 1;
    s.started = 0;
    s.retry_after_sec = 130;
    ManualKickEchoText(s, &kind, &text);
    t.check(kind == 0 && text == L"平台限频中，约 3 分钟后可再刷新", "kick: 限频→分钟口径");
    s.retry_after_sec = 30;
    ManualKickEchoText(s, &kind, &text);
    t.check(text == L"平台限频中，请稍后再试", "kick: 限频<60s→无分钟");
    // 未配置 Key 优先于「没有可刷新」
    s = SummarizeManualKicks({ManualKick::NotConfigured, ManualKick::Disabled});
    ManualKickEchoText(s, &kind, &text);
    t.check(kind == 0 && text == L"请先在卡片设置中配置 API Key", "kick: 未配置→指引文案");
    // 在途请求：合并触发语义
    s = SummarizeManualKicks({ManualKick::InFlight});
    ManualKickEchoText(s, &kind, &text);
    t.check(kind == 0 && text == L"已在查询中，请稍候", "kick: 在途→等待提示");
    // 空账户/全停用
    s = SummarizeManualKicks({});
    ManualKickEchoText(s, &kind, &text);
    t.check(kind == 0 && text == L"没有可刷新的卡片", "kick: 空账户→无可刷新");
}

} // namespace

int RunSelfTests() {
    T t;
    SelftestPrintW(L"QuotaBar --selftest 合成测试（不访问网络）\n");
    TestComputePercent(t);
    TestKimiParse(t);
    TestDeepSeekParse(t);
    TestCodexParse(t);
    TestGlmParse(t);
    TestAntigravity(t);
    TestUserAgent(t);
    TestJsonBounds(t);
    TestMoney(t);
    TestDisplayModel(t);
    TestConfigAccounts(t);
    TestUiLayout(t);
    TestQuotaLayout(t);
    TestDispLayout(t);
    TestProviderOps(t);
    TestRegressions(t);
    TestEmbedRows(t);
    TestBarColorsAndModes(t);
    TestRingItems(t);
    TestSnapshotRoundtrip(t);
    TestSchedulerBasics(t);
    TestSlotGeom(t);
    TestSlotText(t);
    TestSlotPalette(t);
    TestTypeRamp(t);
    TestHistorySampling(t);
    TestDepletionEstimate(t);
    TestTodaySpend(t);
    TestHistoryJsonRoundtrip(t);
    TestIdentityKeyIsolation(t);
    TestMoveVisible(t);
    TestThemePalettes(t);
    TestRefreshFeedback(t);
    char buf[160];
    sprintf_s(buf, "---- 共 %d 项：通过 %d，失败 %d ----\n", t.pass + t.fail, t.pass,
              t.fail);
    SelftestPrint(buf);
    return t.fail ? 1 : 0;
}

} // namespace qb
