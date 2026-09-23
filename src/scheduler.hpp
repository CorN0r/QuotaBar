// scheduler.hpp — 统一调度器：所有网络请求的唯一入口（§5/§9）。
// 单个可合并定时器；退避 5/10/20/30 分钟后封顶 30 分钟 + 0–10% 抖动；
// 429 遵守 Retry-After；401/403 停止自动重试；锁屏/休眠暂停；并发上限 2（每账户 1 个在途）。
// 2026-09-14 起按账户（config.accounts）调度，不再按固定 4 平台槽位。
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace qb {

struct AppConfig;
struct AccountConfig;
struct ProviderSnapshot;
struct HttpResponse;
class HttpClient;
class Log;

// 宿主接口（App 实现）
struct ISchedulerHost {
    virtual AppConfig& Cfg() = 0;
    virtual size_t AccountCount() = 0;
    virtual const AccountConfig& AccountAt(size_t i) = 0;
    // 账户不存在（刚被删除等）返回 nullptr；调度器跳过
    virtual ProviderSnapshot* FindSnap(const std::string& account_id) = 0;
    // API Key：API Key 型账户取存储密钥；codex 返回 auth.json 令牌
    virtual std::string GetKey(const AccountConfig& acc) = 0;
    // 附加认证标识：Codex 的 ChatGPT-Account-Id（其余返回空）
    virtual std::string GetAccountId(const AccountConfig& acc) = 0;
    virtual void OnSnapshotsChanged(bool cache_dirty) = 0; // UI 刷新 + 缓存合并
    virtual void OnNotifyThreshold(const AccountConfig& acc) = 0;
    virtual bool IsShuttingDown() const = 0;
    virtual ~ISchedulerHost() = default;
};

// 纯函数（selftest 复用）：连续失败次数 → 退避秒数（不含抖动）
int64_t BackoffSeconds(int consecutive_failures);

// 手动刷新单次触发结果（2026-09-19 第十一轮：刷新被拒不再静默，UI 汇总后回显）。
// RequestManual 旧版所有拒绝路径 early-return void，点击「立即刷新」无反应时用户
// 无从得知原因（节流/限频/未配 Key）。
enum class ManualKick {
    Started,       // 已发起查询
    InFlight,      // 已有在途请求（合并触发；卡片 chip 已显示「查询中」）
    Throttled,     // 距上次手动刷新不足 60s
    RateLimited,   // 429 退避中（不绕过 Retry-After）
    NotConfigured, // 快照未就绪（未配置 Key / codex 未登录）
    Disabled,      // 账户已停用
    NoAccount,     // 账户不存在或调度表缺失
};
// 汇总（纯函数，selftest 复用）：CmdRefreshAll 把各账户结果归并后取回显文案
struct ManualKickSummary {
    int started = 0, in_flight = 0, throttled = 0, rate_limited = 0;
    int not_configured = 0, inactive = 0, no_account = 0;
    int total = 0;
    int retry_after_sec = 0; // 限频剩余秒数（调用方取各账户最大值填充；0=未知）
};
ManualKickSummary SummarizeManualKicks(const std::vector<ManualKick>& kicks);
// 汇总 → 回显文案（纯函数，selftest 复用）：kind 1=成功(绿) 0=提示(蓝)；text 恒非空
void ManualKickEchoText(const ManualKickSummary& s, int* kind, std::wstring* text);

class Scheduler {
public:
    void Init(ISchedulerHost* host, HttpClient* http, HWND wnd);
    void Start();                 // 窗口创建后调用：安排首批查询（带随机小延迟合并）
    void OnTimer();               // WM_TIMER(id=1)：执行到期工作并重排
    void OnSettingsChanged();     // 间隔/暂停/账户启用变化后重算
    // 手动刷新：返回触发结果供 UI 回显；RateLimited 时经 out 参给出剩余秒数
    ManualKick RequestManual(const std::string& account_id,
                             int* retry_after_sec = nullptr);
    void OnResponse(HttpResponse&& r);
    // 账户凭证/结构变化（新增/删除/换 Key）：重置该账户调度态；已由 App 递增 cred_rev
    void OnAccountChanged(const std::string& account_id);
    void OnSessionLock();
    void OnSessionUnlock();
    void OnSuspend();
    void OnResume();
    void OnNetworkChange();       // 系统网络变化：允许提前重试一次
    void SetNetworkOnline(bool online); // 简化：与 OnNetworkChange 二选一使用
    int64_t NextDeadlineMs() const;      // 诊断用

private:
    struct ProviderSched {
        std::string account_id;
        int consecutive_failures = 0;
        int64_t next_fetch_mono = 0;   // 0=未安排
        bool in_flight = false;
        int64_t last_manual_mono = 0;
        bool stopped = false;          // auth/protocol 错误停机（需用户干预）
        int64_t rate_limited_until_mono = 0;
    };
    std::vector<ProviderSched> sched_; // 与 cfg.accounts 惰性同步（SyncAccounts）
    ISchedulerHost* host_ = nullptr;
    HttpClient* http_ = nullptr;
    HWND wnd_ = nullptr;
    bool locked_or_suspended_ = false;
    bool started_ = false;
    int64_t last_net_event_mono_ = 0;

    ProviderSched* FindSched(const std::string& account_id);
    void SyncAccounts();           // 账户增删后对齐 sched_（新增继承干净状态；删除丢弃）
    bool CanAutoFetch(ProviderSched& s, int64_t now_mono) const;
    void ScheduleProvider(ProviderSched& s, int64_t at_mono);
    void RunDue(int64_t now_mono);
    void StartFetch(ProviderSched& s, const AccountConfig& acc, int64_t now_mono, bool manual);
    void AfterResume();           // 解锁/恢复：到期补查（5–15s 随机延迟，合并事件）
    void RescheduleTimer();
    int64_t EffectiveIntervalSec() const;
};

} // namespace qb
