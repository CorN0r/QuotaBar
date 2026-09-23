#include "scheduler.hpp"
#include "config.hpp"
#include "http.hpp"
#include "log.hpp"
#include "providers.hpp"
#include "state.hpp"
#include "util.hpp"
#include <algorithm>
#include <cstdlib>

namespace qb {

// 5, 10, 20, 30 分钟；之后封顶 30 分钟
int64_t BackoffSeconds(int consecutive_failures) {
    if (consecutive_failures <= 0) return 0;
    static const int64_t kTable[] = {300, 600, 1200, 1800};
    int idx = consecutive_failures - 1;
    if (idx >= 4) return 1800;
    return kTable[idx];
}

namespace {
constexpr int64_t kMs = 1000;
} // namespace

void Scheduler::Init(ISchedulerHost* host, HttpClient* http, HWND wnd) {
    host_ = host;
    http_ = http;
    wnd_ = wnd;
}

int64_t Scheduler::EffectiveIntervalSec() const {
    return host_->Cfg().EffectiveIntervalSec();
}

// 账户列表变化（新增/删除）后对齐调度表：新增账户拿干净调度态，删除账户直接丢弃。
// 各入口（Start/OnTimer/OnSettingsChanged/...）先进此处，保证与 cfg.accounts 一致。
void Scheduler::SyncAccounts() {
    for (size_t i = 0; i < host_->AccountCount(); ++i) {
        const AccountConfig& a = host_->AccountAt(i);
        bool found = false;
        for (const auto& s : sched_) found = found || s.account_id == a.id;
        if (!found) {
            ProviderSched ns;
            ns.account_id = a.id;
            sched_.push_back(std::move(ns));
        }
    }
    sched_.erase(std::remove_if(sched_.begin(), sched_.end(),
                                [&](const ProviderSched& s) {
                                    return host_->Cfg().FindAccount(s.account_id) == nullptr;
                                }),
                 sched_.end());
}

Scheduler::ProviderSched* Scheduler::FindSched(const std::string& account_id) {
    for (auto& s : sched_)
        if (s.account_id == account_id) return &s;
    return nullptr;
}

void Scheduler::ScheduleProvider(ProviderSched& s, int64_t at_mono) {
    s.next_fetch_mono = at_mono;
}

bool Scheduler::CanAutoFetch(ProviderSched& s, int64_t now_mono) const {
    (void)this;
    if (!s.next_fetch_mono) return false;
    if (s.in_flight) return false;
    if (s.stopped) return false;
    if (now_mono < s.rate_limited_until_mono) return false;
    return true;
}

void Scheduler::Start() {
    started_ = true;
    SyncAccounts();
    int64_t now = NowMono();
    for (size_t i = 0; i < host_->AccountCount(); ++i) {
        const AccountConfig& a = host_->AccountAt(i);
        if (!a.enabled) continue;
        ProviderSnapshot* snap = host_->FindSnap(a.id);
        if (!snap || !snap->IsConfigured()) continue;
        ProviderSched* s = FindSched(a.id);
        if (!s) continue;
        // 启动：缓存可能已显示；首批查询合并安排（同批次，小随机延迟）
        ScheduleProvider(*s, now + RandRange(1, 4) * kMs);
    }
    RescheduleTimer();
}

void Scheduler::OnTimer() {
    int64_t now = NowMono();
    RunDue(now);
    RescheduleTimer();
}

void Scheduler::RunDue(int64_t now_mono) {
    if (!started_) return;
    SyncAccounts();
    const AppConfig& cfg = host_->Cfg();
    for (size_t i = 0; i < host_->AccountCount(); ++i) {
        const AccountConfig& a = host_->AccountAt(i);
        if (!a.enabled) continue;                      // 账户停用：不调度
        ProviderSnapshot* snap = host_->FindSnap(a.id);
        if (!snap || !snap->IsConfigured()) continue;
        if (cfg.query.pause) continue;                 // 用户暂停：取消自动调度
        if (locked_or_suspended_) continue;            // 锁屏/休眠
        ProviderSched* s = FindSched(a.id);
        if (!s || !CanAutoFetch(*s, now_mono)) continue;
        if (now_mono >= s->next_fetch_mono) StartFetch(*s, a, now_mono, false);
    }
}

void Scheduler::StartFetch(ProviderSched& s, const AccountConfig& acc, int64_t now_mono,
                           bool manual) {
    const ProviderOps& ops = ProviderOpsOf(acc.type); // 组请求/平台 id/解析器全查分派表
    ProviderRequest req;
    req.spec = ops.build_request();
    std::string key = host_->GetKey(acc);
    if (key.empty()) {
        Log::Write(LogLevel::Info, "fetch", acc.id + " 无密钥，跳过");
        // 密钥消失（如 codex 登出后 auth.json 不在）也必须推进 next_fetch：
        // 直接 return 会让 deadline 停留在过去，RescheduleTimer 把定时器钉在
        // 100ms 下限，RunDue 每 tick 重进本分支——每秒 10 次文件读+日志，
        // 退避/停机语义全部被绕过（实测定时器风暴）。密钥恢复后
        // OnAccountChanged 会清零失败计数并立即重查，退避残留无影响。
        s.consecutive_failures++;
        int64_t delay = BackoffSeconds(s.consecutive_failures);
        ScheduleProvider(s, now_mono + (delay + JitterSeconds(delay, 10)) * kMs);
        return;
    }
    // 认证方案随 provider：GLM 监控端点要求裸 Key（无 Bearer 前缀），其余 "Bearer <key>"
    req.auth_header = req.spec.auth_bearer ? "Bearer " + key : key;
    req.provider_id = ops.id;
    req.account_id = acc.id;
    req.credential_revision = acc.cred_rev;
    Log::WriteF(LogLevel::Info, "fetch", "%s(%s) 请求开始 rev=%lld", acc.id.c_str(),
                req.provider_id.c_str(), (long long)req.credential_revision);
    std::string account = host_->GetAccountId(acc);
    if (!account.empty()) req.extra_headers.push_back({"ChatGPT-Account-Id", account});

    int64_t id = http_->Start(std::move(req));
    if (!id) {
        Log::Write(LogLevel::Warn, "fetch", acc.id + " HTTP 启动失败");
        // 启动失败按一次网络失败退避
        s.consecutive_failures++;
        int64_t delay = BackoffSeconds(s.consecutive_failures);
        ScheduleProvider(s, now_mono + (delay + JitterSeconds(delay, 10)) * kMs);
        RescheduleTimer();
        return;
    }
    s.in_flight = true;
    if (manual) s.last_manual_mono = now_mono;
    ProviderSnapshot* snap = host_->FindSnap(acc.id);
    if (snap) {
        snap->status = FetchStatus::Fetching;
        snap->last_attempt_at_utc = NowUnix();
        snap->last_attempt_mono = now_mono;
        if (snap->last_success_at_utc == 0) {
            snap->error_text.clear(); // 无旧值时显示"查询中"
        }
    }
    host_->OnSnapshotsChanged(false);
    RescheduleTimer();
}

ManualKick Scheduler::RequestManual(const std::string& account_id, int* retry_after_sec) {
    if (retry_after_sec) *retry_after_sec = 0;
    SyncAccounts();
    const AccountConfig* acc = host_->Cfg().FindAccount(account_id);
    if (!acc) return ManualKick::NoAccount;
    if (!acc->enabled) return ManualKick::Disabled;
    ProviderSched* s = FindSched(account_id);
    if (!s) return ManualKick::NoAccount;
    int64_t now = NowMono();
    ProviderSnapshot* snap = host_->FindSnap(account_id);
    if (!snap || !snap->IsConfigured()) return ManualKick::NotConfigured;
    if (s->in_flight) return ManualKick::InFlight; // 合并触发：保持单个在途
    if (now - s->last_manual_mono < 60 * kMs) return ManualKick::Throttled; // 每账户至少间隔 60s
    if (now < s->rate_limited_until_mono) {
        // 不绕过 Retry-After；剩余秒数向上取整交给 UI 提示
        if (retry_after_sec)
            *retry_after_sec = (int)((s->rate_limited_until_mono - now + kMs - 1) / kMs);
        return ManualKick::RateLimited;
    }
    // 暂停时手动刷新：允许一次，但保持暂停调度（不安排 next_fetch）
    StartFetch(*s, *acc, now, true);
    return ManualKick::Started;
}

// ---- 手动刷新汇总（纯函数，selftest 覆盖） ----
ManualKickSummary SummarizeManualKicks(const std::vector<ManualKick>& kicks) {
    ManualKickSummary s;
    s.total = (int)kicks.size();
    for (ManualKick k : kicks) {
        switch (k) {
            case ManualKick::Started: ++s.started; break;
            case ManualKick::InFlight: ++s.in_flight; break;
            case ManualKick::Throttled: ++s.throttled; break;
            case ManualKick::RateLimited: ++s.rate_limited; break;
            case ManualKick::NotConfigured: ++s.not_configured; break;
            case ManualKick::Disabled: ++s.inactive; break;
            case ManualKick::NoAccount: ++s.no_account; break;
        }
    }
    return s;
}
// 文案优先级：有启动→成功口径（附拒绝尾注）；全被拒→按可解释性排序取首要原因
void ManualKickEchoText(const ManualKickSummary& s, int* kind, std::wstring* text) {
    *kind = 1;
    if (s.started > 0) {
        *text = s.started == 1 ? std::wstring(L"已开始刷新 1 张卡片")
                               : L"已开始刷新 " + std::to_wstring(s.started) + L" 张卡片";
        if (s.not_configured > 0)
            *text += L"，" + std::to_wstring(s.not_configured) + L" 张未配置 Key";
        else if (s.throttled > 0)
            *text += L"，" + std::to_wstring(s.throttled) + L" 张刚刷新过";
        else if (s.inactive + s.no_account > 0)
            *text += L"，" + std::to_wstring(s.inactive + s.no_account) + L" 张已停用";
        return;
    }
    *kind = 0;
    if (s.in_flight > 0) {
        *text = L"已在查询中，请稍候";
    } else if (s.rate_limited > 0) {
        if (s.retry_after_sec >= 60)
            *text = L"平台限频中，约 " + std::to_wstring((s.retry_after_sec + 59) / 60) +
                    L" 分钟后可再刷新";
        else
            *text = L"平台限频中，请稍后再试";
    } else if (s.throttled > 0) {
        *text = L"刚刚已刷新过，请 1 分钟后再试";
    } else if (s.not_configured > 0) {
        *text = L"请先在卡片设置中配置 API Key";
    } else {
        *text = L"没有可刷新的卡片";
    }
}

void Scheduler::OnResponse(HttpResponse&& r) {
    ProviderSched* sp = FindSched(r.account_id);
    if (!sp) return; // 账户已被删除：丢弃
    sp->in_flight = false;
    if (host_->IsShuttingDown()) return;

    const AccountConfig* acc = host_->Cfg().FindAccount(r.account_id);
    ProviderSnapshot* snap = host_->FindSnap(r.account_id);
    if (!acc || !snap) return;
    AppConfig& cfg = host_->Cfg();
    (void)cfg;
    int64_t my_rev = acc->cred_rev;
    if (r.credential_revision != my_rev) {
        Log::Write(LogLevel::Info, "fetch", acc->id + " 丢弃旧凭证版本的响应");
        return; // 旧结果不覆盖新账户状态
    }

    int64_t now = NowMono();
    int64_t now_utc = NowUnix();
    ProviderSched& s = *sp;

    if (r.network_error) {
        if (r.cancelled && host_->IsShuttingDown()) return;
        s.consecutive_failures++;
        snap->status = FetchStatus::Offline;
        snap->http_status = 0;
        snap->error_text = r.cancelled ? "请求超时（10s 上限）" : "网络异常";
        snap->next_allowed_mono = 0;
        int64_t delay = BackoffSeconds(s.consecutive_failures);
        delay += JitterSeconds(delay, 10); // 0–10% 正向抖动
        if (!cfg.query.pause) ScheduleProvider(s, now + delay * kMs);
        host_->OnSnapshotsChanged(false);
        char tls[80];
        const char* tls_text = SecureFailureText(r.error_code, tls, sizeof(tls));
        if (tls_text) {
            Log::WriteF(LogLevel::Warn, "fetch", "%s %s code=%lx 退避=%llds",
                        acc->id.c_str(), tls_text, (unsigned long)(r.error_code & 0x7FFFFFFFu),
                        (long long)delay);
        } else {
            Log::WriteF(LogLevel::Warn, "fetch", "%s 网络失败 code=%lu 退避=%llds",
                        acc->id.c_str(), (unsigned long)r.error_code, (long long)delay);
        }
        RescheduleTimer();
        return;
    }

    snap->http_status = r.http_status;
    switch (r.http_status) {
        case 200: {
            // 解析器与契约版本查分派表（provider_id 路由）；未知 id → pr 保持默认
            // （ok=false）走下方 ProtocolError 停机，与旧 if-chain 未命中分支同语义
            const ProviderOps* ops = ProviderOpsById(r.provider_id);
            ParseResult pr;
            if (ops) pr = ops->parse(r.body, now_utc, now);
            if (!pr.ok) {
                s.consecutive_failures = 0;
                snap->status = FetchStatus::ProtocolError;
                snap->error_text = pr.error;
                s.stopped = true; // 结构不兼容：暂停自动查询，避免重复错误请求
                // 尾部附响应体头 8 字节 hex：区分 gzip(1f8b)/BOM(efbbbf)/错误页,不泄露内容
                char hex[17]{};
                if (!r.body.empty()) {
                    unsigned char* p = (unsigned char*)r.body.data();
                    int n = (int)std::min<size_t>(8, r.body.size());
                    for (int k = 0; k < n; ++k) sprintf_s(hex + k * 2, 3, "%02x", p[k]);
                }
                Log::WriteF(LogLevel::Warn, "fetch", "%s %s (http=%d %zuB hdr=%s)",
                            acc->id.c_str(), pr.error.c_str(), r.http_status, r.body.size(),
                            hex);
                break;
            }
            // 成功：发布新快照（保留原 cred_rev/last_attempt）
            int64_t attempt = snap->last_attempt_at_utc;
            int64_t rev = snap->credential_revision;
            *snap = ProviderSnapshot{};
            snap->provider_id = r.provider_id;
            snap->account_id = r.account_id;
            snap->credential_revision = rev;
            snap->status = FetchStatus::Ok;
            snap->last_attempt_at_utc = attempt;
            snap->last_attempt_mono = now;
            snap->last_success_at_utc = now_utc;
            snap->last_success_mono = now;
            snap->http_status = r.http_status; // 重建快照时保留实际 HTTP 码（此前被清零）
            if (ops) snap->contract_revision = ops->contract; // 成功必然查得到（pr.ok ⟺ 已解析）
            snap->completeness = pr.completeness;
            snap->windows = std::move(pr.windows);
            snap->balances = std::move(pr.balances);
            snap->from_cache = false;
            s.consecutive_failures = 0;
            s.stopped = false;
            s.rate_limited_until_mono = 0;
            if (!cfg.query.pause) ScheduleProvider(s, now + EffectiveIntervalSec() * kMs);
            host_->OnNotifyThreshold(*acc);
            host_->OnSnapshotsChanged(true);
            Log::Write(LogLevel::Info, "fetch", acc->id + " 查询成功");
            RescheduleTimer();
            return;
        }
        case 401:
        case 403:
            s.stopped = true; // 停止该账户自动重试，等待用户修改设置
            s.consecutive_failures = 0;
            snap->status = FetchStatus::AuthError;
            snap->error_text = "凭证无效或权限不足";
            break;
        case 429: {
            snap->status = FetchStatus::QueryRateLimited;
            snap->error_text = "查询频率受限";
            int64_t delay = 0;
            std::string ra = r.retry_after;
            if (!ra.empty()) {
                if (ra.find_first_not_of("0123456789 ") == std::string::npos) {
                    delay = strtoll(ra.c_str(), nullptr, 10);
                } else {
                    auto t = ParseHttpDate(ra);
                    if (t) delay = *t - now_utc;
                }
            }
            if (delay <= 0) {
                s.consecutive_failures++;
                delay = BackoffSeconds(s.consecutive_failures);
            }
            delay += JitterSeconds(delay, 10);
            if (delay < 60) delay = 60;
            if (delay > 24 * 3600) delay = 24 * 3600;
            s.rate_limited_until_mono = now + delay * kMs;
            s.next_fetch_mono = s.rate_limited_until_mono;
            snap->next_allowed_mono = s.rate_limited_until_mono; // 详情显示"下次允许查询"
            Log::WriteF(LogLevel::Warn, "fetch", "%s 429 受限 %llds", acc->id.c_str(),
                        (long long)delay);
            RescheduleTimer();
            host_->OnSnapshotsChanged(false);
            return;
        }
        case 404:
        case 410:
            s.stopped = true;
            snap->status = FetchStatus::ProtocolError;
            snap->error_text = "接口不支持或已变化";
            break;
        default:
            if (r.http_status >= 500) {
                s.consecutive_failures++;
                snap->status = FetchStatus::Offline;
                snap->error_text = "服务端错误";
                int64_t delay = BackoffSeconds(s.consecutive_failures);
                delay += JitterSeconds(delay, 10);
                if (!cfg.query.pause) ScheduleProvider(s, now + delay * kMs);
                Log::WriteF(LogLevel::Warn, "fetch", "%s 5xx=%d 退避=%llds",
                            acc->id.c_str(), r.http_status, (long long)delay);
                RescheduleTimer();
                host_->OnSnapshotsChanged(false);
                return;
            }
            s.stopped = true;
            snap->status = FetchStatus::RequestError;
            char buf[32];
            sprintf_s(buf, "HTTP %d", r.http_status);
            snap->error_text = buf;
            break;
    }
    host_->OnSnapshotsChanged(false);
    RescheduleTimer();
}

void Scheduler::OnSettingsChanged() {
    SyncAccounts();
    int64_t now = NowMono();
    const AppConfig& cfg = host_->Cfg();
    for (size_t i = 0; i < host_->AccountCount(); ++i) {
        const AccountConfig& a = host_->AccountAt(i);
        ProviderSnapshot* snap = host_->FindSnap(a.id);
        ProviderSched* s = FindSched(a.id);
        if (!s) continue;
        if (!a.enabled || !snap || !snap->IsConfigured()) {
            s->next_fetch_mono = 0;
            continue;
        }
        if (cfg.query.pause) {
            snap->status = FetchStatus::Paused;
            s->next_fetch_mono = 0;
            continue;
        }
        if (snap->status == FetchStatus::Paused) snap->status = FetchStatus::Ok;
        if (s->stopped) continue;   // auth/protocol 停机需用户干预
        // 数据到期则立即补查；否则按周期
        int64_t due = s->next_fetch_mono;
        if (!due || now >= due) ScheduleProvider(*s, now + RandRange(1, 3) * kMs);
    }
    host_->OnSnapshotsChanged(false);
    RescheduleTimer();
}

void Scheduler::OnAccountChanged(const std::string& account_id) {
    SyncAccounts();
    ProviderSched* s = FindSched(account_id);
    if (!s) return;
    // 清理旧账户数据与错误（跨账户隔离）。
    // 凭证版本由 App 递增后本函数才被调用；是否"已配置"以实际密钥为准，
    // 删除密钥 → 快照版本归 0 → 显示"待配置"，不沿用旧账户版本。
    s->consecutive_failures = 0;
    s->stopped = false;
    s->rate_limited_until_mono = 0;
    s->next_fetch_mono = 0;
    const AccountConfig* acc = host_->Cfg().FindAccount(account_id);
    ProviderSnapshot* snap = host_->FindSnap(account_id);
    if (!acc || !snap) return;
    int64_t rev = host_->GetKey(*acc).empty() ? 0 : acc->cred_rev;
    *snap = ProviderSnapshot{};
    snap->provider_id = ProviderIdOf(acc->type);
    snap->account_id = acc->id;
    snap->credential_revision = rev;
    snap->status = rev > 0 ? FetchStatus::Fetching : FetchStatus::Unconfigured;
    if (rev > 0 && !host_->Cfg().query.pause) {
        // 立即安排一次查询验证新密钥（无需等待下个周期）
        ScheduleProvider(*s, NowMono() + RandRange(1, 3) * kMs);
    }
    host_->OnSnapshotsChanged(true);
    RescheduleTimer();
}

void Scheduler::OnSessionLock() {
    locked_or_suspended_ = true;
    Log::Write(LogLevel::Info, "sched", "锁屏：暂停自动查询");
}
void Scheduler::OnSuspend() {
    locked_or_suspended_ = true;
}
void Scheduler::OnSessionUnlock() {
    locked_or_suspended_ = false;
    AfterResume();
}
void Scheduler::OnResume() {
    locked_or_suspended_ = false;
    AfterResume();
}
void Scheduler::AfterResume() {
    // 数据到期 → 5–15s 随机延迟后各补查一次；合并重复系统事件
    SyncAccounts();
    int64_t now = NowMono();
    int64_t interval = EffectiveIntervalSec();
    for (size_t i = 0; i < host_->AccountCount(); ++i) {
        const AccountConfig& a = host_->AccountAt(i);
        if (!a.enabled) continue;
        ProviderSnapshot* snap = host_->FindSnap(a.id);
        ProviderSched* s = FindSched(a.id);
        if (!snap || !snap->IsConfigured() || !s) continue;
        if (host_->Cfg().query.pause) continue;
        if (s->stopped || s->in_flight) continue;
        bool expired = snap->last_success_mono == 0 ||
                       (now - snap->last_success_mono) > (int64_t)interval * kMs;
        if (expired && now >= s->next_fetch_mono) {
            ScheduleProvider(*s, now + RandRange(5, 15) * kMs);
        }
    }
    RescheduleTimer();
}

void Scheduler::OnNetworkChange() {
    int64_t now = NowMono();
    if (now - last_net_event_mono_ < 10 * kMs) return; // 事件风暴合并
    last_net_event_mono_ = now;
    SyncAccounts();
    for (size_t i = 0; i < host_->AccountCount(); ++i) {
        const AccountConfig& a = host_->AccountAt(i);
        if (!a.enabled) continue;
        ProviderSnapshot* snap = host_->FindSnap(a.id);
        ProviderSched* s = FindSched(a.id);
        if (!snap || !snap->IsConfigured() || !s) continue;
        if (host_->Cfg().query.pause) continue;
        if (s->stopped || s->in_flight) continue;
        // 网络恢复：允许提前重试一次（仅当正处于退避）
        if (snap->status == FetchStatus::Offline &&
            s->next_fetch_mono > now + 30 * kMs) {
            ScheduleProvider(*s, now + RandRange(5, 15) * kMs);
        }
    }
    RescheduleTimer();
}

void Scheduler::SetNetworkOnline(bool) {
    // 保留接口：实际通过 OnNetworkChange 驱动
}

void Scheduler::RescheduleTimer() {
    int64_t now = NowMono();
    int64_t best = 0;
    for (const auto& s : sched_) {
        int64_t t = s.next_fetch_mono;
        if (t && (!best || t < best)) best = t;
    }
    // 缓存合并落盘时间点（15 分钟）也纳入同一截止时间集合
    int64_t due_ms = best ? (best - now) : 0;
    if (best == 0) {
        KillTimer(wnd_, 1);
        return;
    }
    UINT delay = (UINT)std::max((int64_t)100, std::min(due_ms, (int64_t)3600 * 1000));
    SetTimer(wnd_, 1, delay, nullptr);
}

int64_t Scheduler::NextDeadlineMs() const {
    int64_t now = NowMono();
    int64_t best = 0;
    for (const auto& s : sched_) {
        int64_t t = s.next_fetch_mono;
        if (t && (!best || t < best)) best = t;
    }
    return best ? best - now : -1;
}

} // namespace qb
