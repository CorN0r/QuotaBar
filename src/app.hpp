// app.hpp — 生命周期与装配（§5 app/placement 的职责在此）：
// 单实例、隐藏消息中枢窗口、ISchedulerHost 实现、系统事件（锁屏/电源/全屏/
// 网络/Explorer 重启/主题/DPI）、缓存合并落盘、低额度阈值通知、退出清理。
// 2026-09-14 起账户为动态列表（每 coding plan 账户一张卡片）：
// 密钥随账户存内存（DPAPI 落盘经 storage），codex 令牌仍只读 ~/.codex/auth.json。
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "storage.hpp" // AppConfig/ProviderSnapshot/CacheData 完整定义
#include "scheduler.hpp"
#include <memory>
#include <string>
#include <vector>

namespace qb {

class HttpClient;
class Overlay;
class TaskbarSlot;
class TrayUi;
class NetWatch;   // Network List Manager 事件（app.cpp 内部实现）

// 主窗口自定义消息号
inline constexpr UINT kMsgHttpDone   = WM_APP + 1; // lParam = HttpEventMsg*（http.cpp 内部结构）
inline constexpr UINT kMsgTrayNotify = WM_APP + 2; // 托盘回调（lParam = 鼠标/图标事件）
inline constexpr UINT kMsgFullscreen = WM_APP + 3; // WinEvent → 全屏复查
inline constexpr UINT kMsgNetChange  = WM_APP + 4; // 网络状态变化
inline constexpr UINT kMsgShowDetail = WM_APP + 5; // 第二实例请求展示
inline constexpr UINT kTimerSched    = 1;          // 调度器合并定时器
inline constexpr UINT kTimerFsCheck  = 2;          // F11 无前台切换的一次性复查
inline constexpr UINT kTimerSlotPoll = 3;          // 任务栏内嵌槽 1s 轮询（仅 Embed 期）
inline constexpr UINT kExitDrainTimer = 100;       // 退出时等待在途请求收尾（一次性短轮询）

// 运行态账户：配置 + 密钥 + 快照（key 为空 = 未配置；codex/antigravity 恒为空，令牌即时读本机凭证源）
struct AccountState {
    AccountConfig cfg;
    std::string key;
    ProviderSnapshot snap;
};

// 阈值通知跟踪（逐账户；B 迭代 2026-09-15 起含重置恢复检测）
struct NotifyTrack {
    std::string id;
    bool below_fired = false;   // 阈值滞回：已提醒、未回升
    int32_t last_pct = -1;      // 上次已知最小剩余%（-1=未知）
    int64_t last_reset = 0;     // 上次主窗口 reset_at_utc（变化 + 上期低额 → 已重置提醒）
};

class App : public ISchedulerHost {
public:
    // 构造/析构在 app.cpp 定义：App 的成员含 unique_ptr<PImpl 类型>，
    // 避免在头文件内联展开对不完整类型（NetWatch 等）的析构。
    App();
    ~App();
    bool Init(HINSTANCE hinst); // false=已有实例在运行等
    int Run();                  // 消息循环；返回退出码

    void RequestExit() { PostMessage(hwnd_, WM_CLOSE, 0, 0); }
    HWND hwnd() const { return hwnd_; }

    // ---- ISchedulerHost ----
    AppConfig& Cfg() override { return cfg_; }
    size_t AccountCount() override { return accounts_.size(); }
    const AccountConfig& AccountAt(size_t i) override { return accounts_[i].cfg; }
    ProviderSnapshot* FindSnap(const std::string& account_id) override;
    std::string GetKey(const AccountConfig& acc) override;
    std::string GetAccountId(const AccountConfig& acc) override; // Codex: ChatGPT-Account-Id
    std::string GetEndpointBase(const AccountConfig& acc) override; // Antigravity: 本机 hub 基址
    void OnSnapshotsChanged(bool cache_dirty) override;
    void OnNotifyThreshold(const AccountConfig& acc) override;
    bool IsShuttingDown() const override { return shutting_down_; }

    // ---- 命令（托盘菜单/主窗口转发） ----
    void CmdRefreshAll();
    void CmdToggleDetail();      // 详情面板开关
    void CmdOpenQuota();         // 主窗口·额度详情（卡片=账户设置）
    void CmdOpenDisplay();       // 显示设置
    void CmdTogglePause();
    void CmdToggleTrayOnly();
    void CmdClearLocalData();    // 弹出确认后清空本机数据
    void CmdExit();

    // ---- 账户管理（UI 线程调用；变更即落盘并刷新显示） ----
    const std::vector<AccountState>& Accounts() const { return accounts_; }
    AccountState* FindAccount(const std::string& id);
    std::string GetAccountKey(const std::string& id) const;   // UI 明文回填
    // codex 免密直连检测（~/.codex/auth.json 是否就绪）
    bool CodexAuthReady() const;
    // antigravity 本机直连检测（是否有存活的 hub 描述文件；UI 凭证行文案用）
    bool AntigravityHubReady() const;
    enum class AccountCommit { NoChange, Ok, IoError };
    // 新增账户（无 Key 类忽略 key；别名空则用平台默认名）；成功即查询。
    // 无 Key 类（codex/antigravity）凭证源全机唯一,已存在同类型卡片时返回 false
    bool AddAccount(AccountType type, const std::wstring& alias, const std::string& key);
    bool RemoveAccount(const std::string& id);
    // 修改别名/Key（无 Key 类只改别名）；Key 变化递增 cred_rev、清旧数据、立即验证
    AccountCommit UpdateAccount(const std::string& id, const std::wstring& alias,
                                const std::string& key);
    // 即落盘开关三件套：返回 config.json 落盘结果（false=写入失败，UI 回显；无改动返回 true）
    bool SetAccountEnabled(const std::string& id, bool on);   // 启用（查询+常驻条）
    bool SetAccountVisible(const std::string& id, bool on);   // 卡片显隐
    // 低额预警阈值（百分比账户）：0=继承全局 1=关闭 5..95=自定义；即落盘
    bool SetAccountAlertPct(const std::string& id, int pct);
    // 卡片拖拽排序（F3）：把账户移到可见序列第 vis_to 位；即落盘并刷新
    // 返回 config+secrets 双落盘结果（secrets 失败意味着密钥对位未重写，UI 必须回显）
    bool MoveAccountOrder(const std::string& id, int vis_to);

    // ---- 整体配置提交（自启动注册表差异 / 暂停 / 间隔变化分别处理） ----
    // 返回 config.json 落盘结果；自启动注册表写入失败时回拍真实开关态（不假成功）
    bool CommitConfig(const AppConfig& next);

    // ---- 展示辅助（tray_ui 用） ----
    // 详情卡片（visible_only=true 仅返回 card_visible 账户；false 含隐藏卡）
    std::vector<DetailCard> DetailCards(bool visible_only) const;
    // 账户视图（供显示构建器：常驻条/圆环/提示传 enabled；详情页传 visible）
    std::vector<AccountView> AccountViews(bool enabled_only, bool visible_only) const;
    std::wstring TipText() const { return TipText_(); }
    // 用量历史（A 迭代；详情卡片趋势带数据源；只读）
    const UsageHistory& History() const { return history_; }
    bool AccountHasTrendFor(const std::string& id) const;
    // 嵌入条水平偏移实时预览(不落盘;对话框保存走 CommitConfig)
    void SetSlotOffsetDip(int dip) {
        cfg_.display.slot_offset_x_dip = dip;
        if (cfg_.display.mode == DisplayMode::Embed) RefreshDisplay();
    }
    // 嵌入条水平偏移落盘（位置微调 ±/滑杆抬起路径；替代旧的 CfgRef 直写——
    // UI 层不得再持 App 配置的可变引用，改配置一律走显式命令）。
    // 返回 config.json 落盘结果（false=写入失败，UI 回显）
    bool CommitSlotOffsetDip(int dip);
    // 浮条自由位置实时预览（不落盘；滑杆拖动路径，仅 Free 模式生效）
    void SetFreePosLive(int x_dip, int y_dip) {
        cfg_.display.free_pos_x_dip = x_dip;
        cfg_.display.free_pos_y_dip = y_dip;
        if (cfg_.display.mode == DisplayMode::Free) RefreshDisplay();
    }
    // 浮条自由位置落盘（滑杆抬起；即拖即存同语义）。返回落盘结果
    bool CommitFreePos(int x_dip, int y_dip);
    // 吸附开关/吸附水平偏移实时预览（Free 吸附态与 Rings 共用 bar_snap_x_dip）
    void SetSnapLive(bool snap, int snap_x_dip) {
        cfg_.display.bar_snap = snap;
        cfg_.display.bar_snap_x_dip = snap_x_dip;
        if (cfg_.display.mode == DisplayMode::Free || cfg_.display.mode == DisplayMode::Rings)
            RefreshDisplay();
    }
    // 吸附开关/偏移落盘（滑杆抬起/开关切换）。返回落盘结果
    bool CommitSnap(bool snap, int snap_x_dip);
    // 显示设置即调即存（2026-09-15 用户反馈：模式/样式切换即时看到效果，不等保存）。
    // 返回 config.json 落盘结果（false=已应用但写入失败，UI 回显）
    bool ApplyDisplayLive(const DisplayConfig& disp);
    // 主窗口主题手动切换：0=跟随系统 1=深色 2=浅色（即选即生效并落盘）
    bool SetUiThemeMode(int mode);

private:
    bool InitHiddenWindow(HINSTANCE hinst);
    LRESULT HandleMessage(UINT m, WPARAM w, LPARAM l);
    void RefreshDisplay();       // model → overlay/tip/详情窗
    void LoadStartupState();     // secrets/cache/history → 快照与历史（凭证版本不匹配则丢弃）
    void SaveCacheNow(bool force = false); // 落盘（≥15 分钟合并；退出/换 Key 时 force）
    void SaveHistoryNow(bool force = false); // 历史落盘（≥5 分钟合并；退出 force）
    void RecordHistoryNewData(); // 成功快照 → 追加采样（含去重）+ 合并落盘
    void ApplySystemTheme_();    // 读系统深浅色 → SetUiTheme + 菜单/DWM 刷新
    void Notify_(const wchar_t* title, const std::wstring& body); // Toast 优先，气球兜底
    NotifyTrack* FindTrack_(const std::string& id); // 惰性建（账户删除时清理）
    bool SaveSecretsNow();       // 按账户列表写 secrets.dat
    void SyncCfgAccounts_();     // accounts_ → cfg_.accounts（提交/落盘前同步）
    void FullscreenEvent();
    void BeginShutdown();
    std::wstring TipText_() const;          // 托盘悬停摘要
    bool SaveConfigIfDirty_();              // 需要时落盘 config.json

    // 成员
    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    AppConfig cfg_;
    std::vector<AccountState> accounts_;
    UsageHistory history_;       // 用量历史（A 迭代；codex 以身份锚入盘，换 ChatGPT 账户整段重记）
    bool history_dirty_ = false;
    int64_t last_history_save_mono_ = 0;
    std::unique_ptr<HttpClient> http_;
    std::unique_ptr<Scheduler> sched_;
    std::unique_ptr<Overlay> overlay_;
    std::unique_ptr<TaskbarSlot> slot_;
    DisplayMode last_text_mode_ = DisplayMode::Free; // 「仅托盘」往返记忆
    std::unique_ptr<TrayUi> tray_;
    std::unique_ptr<NetWatch> net_;
    HWINEVENTHOOK fs_hook_ = nullptr;
    bool shutting_down_ = false;
    bool config_dirty_ = false;
    bool cache_dirty_ = false;
    std::vector<NotifyTrack> notify_;  // 阈值滞回 + 重置检测（逐账户）
    int64_t last_cache_save_mono_ = 0;
    int64_t exit_deadline_mono_ = 0;
    UINT taskbar_created_msg_ = 0;
    bool com_inited_ = false;
    HANDLE single_mutex_ = nullptr; // 单实例互斥（进程结束自动释放）
};

} // namespace qb
