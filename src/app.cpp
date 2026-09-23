// app.cpp — 装配与生命周期（§5/§9/§10.3）。实现见 app.hpp。
#include "app.hpp"
#include "http.hpp"
#include "json.hpp"
#include "log.hpp"
#include "notify.hpp"
#include "overlay.hpp"
#include "provider_icons.hpp"
#include "providers.hpp"
#include "taskbar_slot.hpp"
#include "state.hpp"
#include "storage.hpp"
#include "theme.hpp"
#include "tray_ui.hpp"
#include "tray_ui_internal.hpp" // SyncDraftDisplay（浮条拖放落位后同步页面草稿）
#include "util.hpp"
#include <netlistmgr.h>
#include <wtsapi32.h>
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <iterator>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "wtsapi32.lib")

namespace qb {

namespace {
// Codex CLI 登录凭证：%USERPROFILE%\.codex\auth.json。QuotaBar 只读不写（令牌由 Codex CLI
// 自己刷新维护）；仅支持 auth_mode=chatgpt（订阅账户）；读取失败 → 空（"未配置"语义）。
// 令牌绝不写入日志/配置/缓存/详情文本。
bool ReadCodexAuth(std::string& token, std::string& account_id) {
    token.clear();
    account_id.clear();
    wchar_t prof[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    std::ifstream f(std::wstring(prof) + L"\\.codex\\auth.json", std::ios::binary);
    if (!f) return false;
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto root = JsonParse(text);
    if (!root || !root->IsObject()) return false;
    if (root->Find("auth_mode").AsString() != "chatgpt") return false; // 仅订阅账户模式
    const JsonValue& t = root->Find("tokens");
    const JsonValue& at = t.Find("access_token");
    if (!at.IsString() || at.AsString().empty()) return false;
    if (t.Find("account_id").IsString()) account_id = t.Find("account_id").AsString();
    token = at.AsString();
    return true;
}
} // namespace

// 本文件内部实现（内部链接）：单实例常量、网络监视、WinEvent 钩子、主题读取。
constexpr wchar_t kMainClassName[] = L"QuotaBarHiddenWnd";
constexpr wchar_t kSingleInstName[] = L"QuotaBar_SingleInstance";

// ---------- 网络状态监视（Network List Manager，轻量且仅接收变化事件） ----------
// NetWatch/NetNotifySink 具名于 qb 作用域：与 app.hpp 前向声明保持一致。
class NetNotifySink : public INetworkListManagerEvents {
public:
    NetNotifySink(HWND wnd) : wnd_(wnd) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_INetworkListManagerEvents) {
            *ppv = static_cast<INetworkListManagerEvents*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }
    // 可能来自 COM 工作线程：只投递到 UI 线程（风暴由 Scheduler 合并）
    STDMETHODIMP ConnectivityChanged(NLM_CONNECTIVITY) override {
        PostMessage(wnd_, kMsgNetChange, 0, 0);
        return S_OK;
    }
private:
    HWND wnd_ = nullptr;
    LONG ref_ = 1;
};

class NetWatch {
public:
    bool Start(HWND wnd) {
        INetworkListManager* nlm = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&nlm));
        if (FAILED(hr)) return false;
        IConnectionPointContainer* cpc = nullptr;
        hr = nlm->QueryInterface(IID_PPV_ARGS(&cpc));
        nlm->Release();
        if (FAILED(hr)) return false;
        hr = cpc->FindConnectionPoint(IID_INetworkListManagerEvents, &cp_);
        cpc->Release();
        if (FAILED(hr)) return false;
        sink_ = new NetNotifySink(wnd);
        hr = cp_->Advise(sink_, &cookie_);
        if (FAILED(hr)) {
            sink_->Release();
            sink_ = nullptr;
            cp_->Release();
            cp_ = nullptr;
            return false;
        }
        return true;
    }
    ~NetWatch() {
        if (cp_) {
            if (sink_) cp_->Unadvise(cookie_);
            cp_->Release();
        }
        if (sink_) sink_->Release();
    }
private:
    IConnectionPoint* cp_ = nullptr;
    INetworkListManagerEvents* sink_ = nullptr;
    DWORD cookie_ = 0;
};

// ---------- WinEvent 全屏钩子（§10.3：有限范围、OUTOFCONTEXT、只筛选前台切换） ----------
static HWND g_win_event_wnd = nullptr; // 投递目标（InitHiddenWindow 设置）

static void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD,
                                DWORD) {
    if (!hwnd) return;
    // 只投递 UI 线程；App 负责复查（含定时复查覆盖 F11 无前台切换的场景）
    static HWND s_last{};
    if (s_last == hwnd) return;
    s_last = hwnd;
    if (g_win_event_wnd) PostMessage(g_win_event_wnd, kMsgFullscreen, 0, 0);
}

// 主题（浅/深）读取：文字颜色"自动"模式依据
static bool AppsUseLightTheme() {
    HKEY hk;
    DWORD v = 1, sz = sizeof(v), type = REG_DWORD;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &hk) == ERROR_SUCCESS) {
        RegQueryValueExW(hk, L"AppsUseLightTheme", nullptr, &type, (BYTE*)&v, &sz);
        RegCloseKey(hk);
    }
    return v != 0;
}

App::App() = default;

App::~App() {
    if (single_mutex_) CloseHandle(single_mutex_);
    if (com_inited_) CoUninitialize();
}

bool App::Init(HINSTANCE hinst) {
    inst_ = hinst;
    SeedRng();
    Log::Init(GetDataDir());
    Log::Write(LogLevel::Info, "app", "QuotaBar 启动");

    // ---- 单实例：命名互斥 + 隐藏窗查找（唤醒已有实例） ----
    HANDLE hmut = CreateMutexW(nullptr, FALSE, kSingleInstName);
    bool already = hmut && GetLastError() == ERROR_ALREADY_EXISTS;
    if (already) {
        // FindWindow 对隐藏顶层窗口不可靠，改用 EnumWindows 按类名匹配；
        // 老实例可能正处于窗口创建前，短时重试覆盖该竞态窗口。
        HWND peer = nullptr;
        for (int attempt = 0; attempt < 10 && !peer; ++attempt) {
            EnumWindows([](HWND h, LPARAM lp) -> BOOL {
                wchar_t cls[64]{};
                GetClassNameW(h, cls, 64);
                if (wcscmp(cls, kMainClassName) == 0) {
                    *(HWND*)lp = h;
                    return FALSE; // 找到即停止
                }
                return TRUE;
            }, (LPARAM)&peer);
            if (!peer) Sleep(100);
        }
        if (peer) PostMessageW(peer, kMsgShowDetail, 0, 0);
        if (hmut) CloseHandle(hmut);
        return false; // main 直接退出，不弹窗
    }
    single_mutex_ = hmut;

    // ---- 配置 / 凭证 / 缓存 ----
    LoadConfig(cfg_);
    // ---- 主题（F1 双主题；ui_theme 支持 0=跟随系统 1=深色 2=浅色 手动覆盖）----
    ApplySystemTheme_();
    last_text_mode_ = cfg_.display.mode != DisplayMode::TrayOnly
                          ? cfg_.display.mode
                          : DisplayMode::Free;
    {
        // secrets 迁移/对齐到账户列表（v1 三段自动映射到同类型账户）
        std::vector<std::string> keys;
        LoadSecrets(cfg_.accounts, keys);
        accounts_.clear();
        for (size_t i = 0; i < cfg_.accounts.size(); ++i) {
            AccountState st;
            st.cfg = cfg_.accounts[i];
            st.key = std::move(keys[i]);
            accounts_.push_back(std::move(st));
        }
    }
    if (GetAutostart() != cfg_.query.autostart) {
        // 注册表与配置漂移：以注册表为准并回写（卸载/被杀都可能留下差异）
        cfg_.query.autostart = GetAutostart();
        config_dirty_ = true;
    }

    // ---- COM（Network List Manager 事件） ----
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) com_inited_ = true;

    ProviderIconsInit(); // 原厂图标解码（COM/GDI+ 就绪后；幂等，嵌入条与徽章共用）

    if (!InitHiddenWindow(hinst)) return false;
    LoadStartupState();

    http_ = std::make_unique<HttpClient>();
    if (!http_->Init(hwnd_, kMsgHttpDone, [this](HttpResponse&& r) { sched_->OnResponse(std::move(r)); })) {
        Log::Write(LogLevel::Error, "app", "HTTP 初始化失败");
    }
    sched_ = std::make_unique<Scheduler>();
    sched_->Init(this, http_.get(), hwnd_);

    overlay_ = std::make_unique<Overlay>();
    if (!overlay_->Create(hinst, hwnd_)) {
        Log::Write(LogLevel::Error, "app", "覆盖层窗口创建失败");
    }
    overlay_->SetAdjustEndCallback([this](bool save, const std::optional<DisplayConfig>& next) {
        if (save && next) {
            cfg_.display = *next;
            config_dirty_ = true;
            SaveConfigIfDirty_();
            SyncDraftDisplay(*next); // 显示页草稿同步：防「保存设置」用旧快照拽回位置
            RefreshDisplay();
        }
        tray_->RefreshMenuState();
    });

    // 任务栏内嵌槽(display.mode=Embed 时启用;窗口/设备懒建,失败不阻塞其他模式)
    slot_ = std::make_unique<TaskbarSlot>();
    if (!slot_->Create(hinst)) {
        Log::Write(LogLevel::Warn, "app", "任务栏内嵌槽注册失败(嵌入模式不可用)");
    }
    slot_->SetStatusCallback([this](bool degraded, const char* why) {
        if (!tray_) return;
        if (degraded) {
            std::string m = std::string("任务栏内嵌不可用（") + why + "），恢复后自动显示";
            tray_->ShowBubble(L"QuotaBar", Utf8ToWide(m));
        }
        tray_->RefreshMenuState();
    });

    tray_ = std::make_unique<TrayUi>();
    tray_->Init(hinst, hwnd_, this);
    tray_->UpdateTip(TipText_());

    // 网络监视失败静默降级：退避机制兜底（§9.1"没有通知时按退避调度"）
    net_ = std::make_unique<NetWatch>();
    if (!net_->Start(hwnd_)) {
        net_.reset();
        Log::Write(LogLevel::Info, "app", "网络监视不可用，依赖退避调度");
    }

    fs_hook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                               OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    if (!fs_hook_) Log::Write(LogLevel::Warn, "app", "WinEvent 前台钩子注册失败");

    RefreshDisplay();
    sched_->Start();

    // 首次运行：自动打开主窗口额度页并展开「添加卡片」（账户设置并入卡片）
    if (!cfg_.first_run_done) {
        cfg_.first_run_done = true;
        config_dirty_ = true;
        SaveConfigIfDirty_();
        tray_->ShowQuotaAddPanel();
    } else {
        SaveConfigIfDirty_(); // autostart 漂移等任何未落盘变更
    }
    Log::Write(LogLevel::Info, "app", "初始化完成");
    return true;
}

int App::Run() {
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { // -1=错误：读未初始化 wParam 无意义，直接退出
        // 主窗口打开时优先走其对话框导航（Tab/Esc/方向键）
        if (tray_ && tray_->PreTranslateMessage(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

// ---------- 隐藏消息中枢窗口 ----------
bool App::InitHiddenWindow(HINSTANCE hinst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
        App* self = (App*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (self) return self->HandleMessage(m, w, l);
        return DefWindowProcW(h, m, w, l);
    };
    wc.hInstance = hinst;
    wc.lpszClassName = kMainClassName;
    if (!RegisterClassExW(&wc)) return false;
    hwnd_ = CreateWindowExW(0, kMainClassName, L"QuotaBar", WS_OVERLAPPED,
                            CW_USEDEFAULT, CW_USEDEFAULT, 0, 0,
                            nullptr, nullptr, hinst, nullptr);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);
    g_win_event_wnd = hwnd_;
    taskbar_created_msg_ = RegisterWindowMessageW(L"TaskbarCreated");
    WTSRegisterSessionNotification(hwnd_, NOTIFY_FOR_THIS_SESSION);
    return true;
}

// ---------- 消息处理 ----------
LRESULT App::HandleMessage(UINT m, WPARAM w, LPARAM l) {
    if (m == taskbar_created_msg_) {
        // Explorer 重启：重建托盘图标并重新定位（不重置查询退避，§10.3）
        if (tray_) tray_->RecreateIcon();
        if (overlay_) overlay_->OnTaskbarCreated();
        if (slot_) slot_->OnTaskbarCreated(); // 嵌入槽整窗重建(detach→rebuild→attach)
        return 0;
    }
    switch (m) {
        case WM_TIMER: {
            UINT_PTR id = w;
            if (id == kTimerSched) {
                if (sched_) sched_->OnTimer();
            } else if (id == kTimerFsCheck) {
                KillTimer(hwnd_, id);
                if (overlay_) overlay_->OnFullscreenCheck();
            } else if (id == kTimerSlotPoll) {
                if (slot_) slot_->Poll();
            } else if (id == kExitDrainTimer) {
                // 退出收尾：等待在途 HTTP 取消回调清空（Pump 负责释放事件体）
                if (http_ && http_->Pending() && NowMono() < exit_deadline_mono_) {
                    // 继续等待
                } else {
                    KillTimer(hwnd_, kExitDrainTimer);
                    if (http_) http_->Finalize(); // 对象保留到进程退出，残余消息由 Pump 安全处理
                    DestroyWindow(hwnd_);
                }
            } else if (id >= 1000 && http_) {
                http_->OnTimer(id);
            }
            return 0;
        }
        case kMsgHttpDone:
            if (http_) http_->Pump((void*)l); // Pump 负责释放（http_ 保持存活至进程退出）
            return 0;
        case kMsgTrayNotify:
            if (tray_) tray_->OnNotify((WORD)w, l);
            return 0;
        case kMsgFullscreen:
            FullscreenEvent();
            return 0;
        case kMsgNetChange:
            if (sched_) sched_->OnNetworkChange();
            return 0;
        case kMsgShowDetail:
            // 第二实例请求：显示详情并置前
            if (tray_) {
                tray_->ShowDetail();
                HWND dh = tray_->detail_hwnd();
                if (dh) {
                    ShowWindow(dh, SW_SHOW);
                    SetForegroundWindow(dh);
                }
            }
            return 0;

        case WM_WTSSESSION_CHANGE:
            if (w == WTS_SESSION_LOCK) {
                if (sched_) sched_->OnSessionLock();
            } else if (w == WTS_SESSION_UNLOCK) {
                if (sched_) sched_->OnSessionUnlock();
                RefreshDisplay();
            }
            return 0;
        case WM_POWERBROADCAST:
            if (w == PBT_APMSUSPEND) {
                if (sched_) sched_->OnSuspend();
                return TRUE;
            }
            if (w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMECRITICAL) {
                if (sched_) sched_->OnResume();
                RefreshDisplay();
                return TRUE;
            }
            return TRUE;
        case WM_DISPLAYCHANGE:
            if (overlay_) overlay_->OnDisplayChange();
            if (overlay_) overlay_->OnFullscreenCheck();
            if (slot_) slot_->OnDisplayChange();
            return 0;
        case WM_THEMECHANGED:
            ApplySystemTheme_(); // 颜色自动模式随主题重建
            RefreshDisplay();
            return 0;
        case WM_SETTINGCHANGE:
            // 主题色/个性化变化（Appearance 变化也可能带 ImmersiveColorSet）
            if (l && wcscmp((const wchar_t*)l, L"ImmersiveColorSet") == 0) {
                ApplySystemTheme_();
                RefreshDisplay();
            }
            return 0;

        case WM_CLOSE:
            if (!shutting_down_) BeginShutdown();
            return 0;
        case WM_ENDSESSION:
            if (w) BeginShutdown();
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd_, m, w, l);
    }
}

// ---------- 生命周期收尾 ----------
void App::BeginShutdown() {
    shutting_down_ = true;
    Log::Write(LogLevel::Info, "app", "退出中");
    KillTimer(hwnd_, kTimerSched);
    KillTimer(hwnd_, kTimerFsCheck);
    KillTimer(hwnd_, kTimerSlotPoll);
    if (fs_hook_) { UnhookWinEvent(fs_hook_); fs_hook_ = nullptr; }
    WTSUnRegisterSessionNotification(hwnd_);
    if (net_) { net_.reset(); }
    if (slot_) { slot_->Destroy(); slot_.reset(); }
    if (tray_) tray_->Shutdown();
    if (overlay_) overlay_->Destroy();
    SaveCacheNow(true);
    SaveHistoryNow(true);
    if (config_dirty_) {
        SaveConfig(cfg_);
        config_dirty_ = false;
    }
    Log::Shutdown();
    if (http_) {
        http_->Shutdown();          // 取消在途请求；回调仍会入队
        exit_deadline_mono_ = NowMono() + 2000;
        SetTimer(hwnd_, kExitDrainTimer, 30, nullptr); // 短轮询收尾（一次性）
    } else {
        DestroyWindow(hwnd_);
    }
}

// ---------- 快照与启动加载 ----------
ProviderSnapshot* App::FindSnap(const std::string& account_id) {
    for (auto& a : accounts_)
        if (a.cfg.id == account_id) return &a.snap;
    return nullptr;
}
std::string App::GetKey(const AccountConfig& acc) {
    if (acc.type == AccountType::Codex) {
        // codex 免密直连：令牌只读 ~/.codex/auth.json（Codex CLI 维护刷新）
        std::string tok, acc_id;
        return ReadCodexAuth(tok, acc_id) ? tok : "";
    }
    for (const auto& a : accounts_)
        if (a.cfg.id == acc.id) return a.key;
    return {};
}
std::string App::GetAccountId(const AccountConfig& acc) {
    if (acc.type != AccountType::Codex) return {};
    std::string tok, acc_id;
    ReadCodexAuth(tok, acc_id);
    return acc_id;
}
bool App::CodexAuthReady() const {
    std::string tok, acc_id;
    return ReadCodexAuth(tok, acc_id) && !tok.empty();
}

void App::LoadStartupState() {
    // 快照初始化：配置了 Key → 查询中；未配置 → 待配置
    auto seed = [&](AccountState& st) {
        std::string key = st.cfg.type == AccountType::Codex
                              ? GetKey(st.cfg)
                              : st.key;
        st.snap.provider_id = ProviderIdOf(st.cfg.type);
        st.snap.account_id = st.cfg.id;
        // codex 凭证版本固定 1（auth.json 已配置即有效，无换 Key 语义）
        st.snap.credential_revision = key.empty() ? 0 : st.cfg.cred_rev;
        st.snap.status = key.empty() ? FetchStatus::Unconfigured : FetchStatus::Fetching;
        st.snap.error_text.clear();
    };
    for (auto& st : accounts_) seed(st);

    // 用量历史（A 迭代）：磁盘加载 + 按保留期截断（账户隔离沿用 account_id 键）
    UsageHistory hist;
    if (LoadHistory(hist)) {
        hist.Purge(NowUnix());
        history_ = std::move(hist);
    }
    // codex 历史身份校验：进程关闭期间切换过 ChatGPT 账户 → 旧段立即丢弃，
    // 不等首次采样才切段（否则旧账户曲线会在 UI 里多停留一个查询周期）
    for (const auto& st : accounts_) {
        if (st.cfg.type != AccountType::Codex) continue;
        std::string idk = Fnv1a64Hex(GetAccountId(st.cfg));
        if (idk.empty()) continue;
        auto it = std::find_if(history_.accounts.begin(), history_.accounts.end(),
                               [&](const AccountHistory& h) {
                                   return h.account_id == st.cfg.id &&
                                          !h.identity_key.empty() &&
                                          h.identity_key != idk;
                               });
        if (it != history_.accounts.end()) {
            history_.accounts.erase(it);
            history_dirty_ = true;
            Log::Write(LogLevel::Info, "history",
                       "codex 历史身份与 auth.json 不一致，丢弃旧段");
        }
    }

    CacheData cache;
    if (!LoadCache(cache)) return;
    // 凭证版本匹配才采用缓存（跨账户隔离：换 Key 后旧数据不得出现）
    for (auto& cs : cache.items) {
        if (cs.account_id.empty()) continue;
        for (auto& st : accounts_) {
            if (st.cfg.id != cs.account_id) continue;
            if (st.snap.credential_revision == 0) break; // 未配置：不进旧缓存
            if (cs.credential_revision != st.cfg.cred_rev) break;
            if (cs.provider_id != ProviderIdOf(st.cfg.type)) break;
            st.snap = cs; // from_cache=true → 显示 †（§9.3）
            break;
        }
    }
}

// ---------- 账户管理 ----------
AccountState* App::FindAccount(const std::string& id) {
    for (auto& a : accounts_)
        if (a.cfg.id == id) return &a;
    return nullptr;
}
std::string App::GetAccountKey(const std::string& id) const {
    for (const auto& a : accounts_)
        if (a.cfg.id == id) return a.key;
    return {};
}
bool App::AddAccount(AccountType type, const std::wstring& alias_w, const std::string& key) {
    if (type == AccountType::Codex && cfg_.HasType(AccountType::Codex)) return false;
    AccountState st;
    st.cfg.id = "acc" + std::to_string(++cfg_.account_seq);
    st.cfg.type = type;
    st.cfg.alias = WideToUtf8(TrimW(alias_w));
    if (st.cfg.alias.empty()) st.cfg.alias = ProviderDefaultAlias(type);
    st.cfg.enabled = true;
    st.cfg.card_visible = true;
    st.cfg.cred_rev = 1; // 新建账户 = v1 凭证
    if (type != AccountType::Codex) st.key = Trim(key);
    st.snap.provider_id = ProviderIdOf(type);
    st.snap.account_id = st.cfg.id;
    accounts_.push_back(std::move(st));
    SyncCfgAccounts_();
    config_dirty_ = true;
    if (!SaveConfigIfDirty_()) config_dirty_ = true;
    SaveSecretsNow();
    sched_->OnAccountChanged(accounts_.back().cfg.id);
    SaveCacheNow(true); // 清掉可能残留的旧缓存语义
    RefreshDisplay();
    Log::Write(LogLevel::Info, "accounts", "新增账户 " + accounts_.back().cfg.id);
    return true;
}
bool App::RemoveAccount(const std::string& id) {
    auto it = std::find_if(accounts_.begin(), accounts_.end(),
                           [&](const AccountState& a) { return a.cfg.id == id; });
    if (it == accounts_.end()) return false;
    accounts_.erase(it);
    // 通知跟踪与历史一并丢弃（id 永不复用，历史无回归价值）
    notify_.erase(std::remove_if(notify_.begin(), notify_.end(),
                                 [&](const NotifyTrack& t) { return t.id == id; }),
                  notify_.end());
    history_.accounts.erase(std::remove_if(history_.accounts.begin(),
                                           history_.accounts.end(),
                                           [&](const AccountHistory& h) {
                                               return h.account_id == id;
                                           }),
                            history_.accounts.end());
    history_dirty_ = true;
    cfg_.accounts.clear();
    for (const auto& a : accounts_) cfg_.accounts.push_back(a.cfg);
    config_dirty_ = true;
    SaveConfigIfDirty_();
    SaveSecretsNow();
    SaveCacheNow(true); // 旧账户缓存立即移除
    sched_->OnSettingsChanged();
    RefreshDisplay();
    Log::Write(LogLevel::Info, "accounts", "删除账户 " + id);
    return true;
}
App::AccountCommit App::UpdateAccount(const std::string& id, const std::wstring& alias_w,
                                      const std::string& key) {
    AccountState* st = FindAccount(id);
    if (!st) return AccountCommit::NoChange;
    std::string alias = WideToUtf8(TrimW(alias_w));
    if (st->cfg.type == AccountType::Codex) {
        // codex：只改别名（令牌由 auth.json 提供）
        if (alias == st->cfg.alias) return AccountCommit::NoChange;
        st->cfg.alias = alias;
        config_dirty_ = true;
        if (!SaveConfigIfDirty_()) return AccountCommit::IoError; // 别名未持久化，如实上报
        RefreshDisplay();
        return AccountCommit::Ok;
    }
    bool key_changed = Trim(key) != st->key;
    bool alias_changed = !alias.empty() && alias != st->cfg.alias;
    if (!key_changed && !alias_changed) return AccountCommit::NoChange;
    if (key_changed) ++st->cfg.cred_rev;
    if (!alias.empty()) st->cfg.alias = alias;
    if (key_changed) {
        st->key = Trim(key);
        // 凭证版本隔离：旧 Key 期的历史序列切段（跨账户混算会污染速率/今日消耗）
        history_.accounts.erase(std::remove_if(history_.accounts.begin(),
                                               history_.accounts.end(),
                                               [&](const AccountHistory& h) {
                                                   return h.account_id == id;
                                               }),
                                history_.accounts.end());
        history_dirty_ = true;
    }
    SyncCfgAccounts_();
    config_dirty_ = true;
    bool cfg_ok = SaveConfigIfDirty_();
    if (!SaveSecretsNow() || !cfg_ok) return AccountCommit::IoError;
    if (key_changed) sched_->OnAccountChanged(id);
    SaveCacheNow(true);
    RefreshDisplay();
    return AccountCommit::Ok;
}
bool App::SetAccountEnabled(const std::string& id, bool on) {
    AccountState* st = FindAccount(id);
    if (!st || st->cfg.enabled == on) return true;
    st->cfg.enabled = on;
    SyncCfgAccounts_();
    config_dirty_ = true;
    bool ok = SaveConfigIfDirty_();
    sched_->OnSettingsChanged();
    RefreshDisplay();
    return ok;
}
bool App::SetAccountVisible(const std::string& id, bool on) {
    AccountState* st = FindAccount(id);
    if (!st || st->cfg.card_visible == on) return true;
    st->cfg.card_visible = on;
    SyncCfgAccounts_();
    config_dirty_ = true;
    bool ok = SaveConfigIfDirty_();
    RefreshDisplay();
    return ok;
}
bool App::SetAccountAlertPct(const std::string& id, int pct) {
    AccountState* st = FindAccount(id);
    if (!st) return true;
    if (pct != 0 && pct != 1 && (pct < 5 || pct > 95)) return true; // 口径外值拒绝
    if (st->cfg.alert_pct == pct) return true;
    st->cfg.alert_pct = pct;
    // 换阈值即重置滞回：新阈值下允许立刻按新口径判断
    NotifyTrack* tr = FindTrack_(id);
    if (tr) tr->below_fired = false;
    SyncCfgAccounts_();
    config_dirty_ = true;
    bool ok = SaveConfigIfDirty_();
    Log::WriteF(LogLevel::Info, "notify", "%s 阈值改为 %d", id.c_str(), pct);
    return ok;
}
bool App::MoveAccountOrder(const std::string& id, int vis_to) {
    // 找到该账户在"可见序列"中的当前位置，再按目标位整体重排（隐藏卡原位保留）
    std::vector<size_t> vis;
    size_t from_pos = SIZE_MAX;
    for (size_t i = 0; i < accounts_.size(); ++i) {
        if (!accounts_[i].cfg.card_visible) continue;
        if (accounts_[i].cfg.id == id) from_pos = vis.size();
        vis.push_back(i);
    }
    if (from_pos == SIZE_MAX || vis.size() < 2) return true;
    // secrets.dat v2 按 accounts 序逐段存取——重排后必须立即按"账户↔密钥"原关联
    // 重写，否则下次启动按新顺序映射会张冠李戴（实测排序→重启后密钥错位）
    std::vector<std::pair<std::string, std::string>> key_by_id;
    for (const auto& a : accounts_) key_by_id.push_back({a.cfg.id, a.key});
    MoveVisibleInVector(accounts_, from_pos, vis_to,
                        [](const AccountState& a) { return a.cfg.card_visible; });
    for (auto& a : accounts_)
        for (const auto& kv : key_by_id)
            if (kv.first == a.cfg.id) a.key = kv.second;
    SyncCfgAccounts_();
    config_dirty_ = true;
    bool ok = SaveConfigIfDirty_();
    // 与新账户序对齐重写（密钥按 id 关联不变）；失败时密钥对位未落盘，UI 必须回显
    if (!SaveSecretsNow()) ok = false;
    RefreshDisplay();
    Log::WriteF(LogLevel::Info, "accounts", "%s 排序移至可见位 %d（secrets 已按新序重写）",
                id.c_str(), vis_to);
    return ok;
}
void App::SyncCfgAccounts_() {
    cfg_.accounts.clear();
    for (const auto& a : accounts_) cfg_.accounts.push_back(a.cfg);
}

// ---------- 数据变化 → 界面 ----------
void App::OnSnapshotsChanged(bool cache_dirty) {
    if (cache_dirty) {
        cache_dirty_ = true;
        SaveCacheNow(false); // 内部按 ≥15 分钟合并
        RecordHistoryNewData(); // 成功快照 → 历史采样（内部按 ≥5 分钟合并落盘）
    }
    RefreshDisplay();
    // 每次成功/失败也会更新托盘提示
    if (tray_) tray_->UpdateTip(TipText_());
}

// ---------- 用量历史（A 迭代 2026-09-15；codex 身份锚入盘 2026-09-22） ----------
void App::RecordHistoryNewData() {
    int64_t now_utc = NowUnix();
    bool any = false;
    for (auto& a : accounts_) {
        if (a.snap.status != FetchStatus::Ok || a.snap.last_success_at_utc == 0) continue;
        // codex 身份锚：ChatGPT Account Id 哈希（读取 auth.json，与本次查询同一账户）。
        // 锚不上（auth_mode 非 chatgpt/缺 account_id）→ 不采样，沿用旧的保守口径；
        // 锚上但与已存键不同 → 整段清空重记（cred_rev 隔离的同口径延伸）。
        std::string idk;
        if (a.cfg.type == AccountType::Codex) {
            idk = Fnv1a64Hex(GetAccountId(a.cfg));
            if (idk.empty()) continue;
        }
        AccountHistory* h = history_.Find(a.cfg.id);
        if (!h) {
            AccountHistory nh;
            nh.account_id = a.cfg.id;
            history_.accounts.push_back(std::move(nh));
            h = &history_.accounts.back();
        }
        if (!idk.empty() && ApplyIdentityKey(*h, idk))
            Log::Write(LogLevel::Info, "history",
                       "codex 账户身份变化，历史序列已重置");
        RecordSnapshotSamples(*h, a.cfg, a.snap, now_utc);
        any = true;
    }
    if (any) {
        history_.Purge(now_utc);
        history_dirty_ = true;
        SaveHistoryNow(false); // ≥5 分钟合并
    }
}
void App::SaveHistoryNow(bool force) {
    if (!history_dirty_ && !force) return;
    int64_t now = NowMono();
    if (!force && now - last_history_save_mono_ < 5 * 60 * 1000) return; // ≥5 分钟合并
    if (SaveHistory(history_)) {
        history_dirty_ = false;
        last_history_save_mono_ = now;
    }
}
void App::ApplySystemTheme_() {
    // ui_theme:0=跟随系统 1=深色 2=浅色（手动覆盖时系统切换不再倒戈）
    const bool dark = cfg_.ui_theme == 1 ? true
                      : cfg_.ui_theme == 2 ? false
                                           : !AppsUseLightTheme();
    theme::SetUiTheme(dark);
    theme::EnableDarkSystemChrome();
    if (tray_ && tray_->detail_hwnd()) {
        HWND dh = tray_->detail_hwnd();
        theme::ApplyUiThemeToWindow(dh);
        InvalidateRect(dh, nullptr, FALSE);
        // EDIT 子控件只在自身失效时才重新发 WM_CTLCOLOR* 查询背景刷，
        // 主题切换后必须逐个强制重绘，否则输入框残留旧主题底色（实测）
        EnumChildWindows(dh, [](HWND h, LPARAM) -> BOOL {
            InvalidateRect(h, nullptr, TRUE);
            return TRUE;
        }, 0);
    }
}
bool App::SetUiThemeMode(int mode) {
    if (mode < 0 || mode > 2) return true;
    if (cfg_.ui_theme == mode) return true;
    cfg_.ui_theme = mode;
    config_dirty_ = true;
    bool ok = SaveConfigIfDirty_();
    ApplySystemTheme_();
    Log::WriteF(LogLevel::Info, "app", "ui theme mode -> %d", mode);
    return ok;
}
void App::Notify_(const wchar_t* title, const std::wstring& body) {
    // Toast 优先（Win11 原生通知中心）；AUMID 捷径缺失/系统关闭通知 → 托盘气球兜底
    if (notify::EnsureToastShortcut() && notify::ShowToast(notify::kAumid, title, body)) {
        return;
    }
    if (tray_) tray_->ShowBubble(title, body);
}
NotifyTrack* App::FindTrack_(const std::string& id) {
    for (auto& t : notify_)
        if (t.id == id) return &t;
    NotifyTrack t;
    t.id = id;
    notify_.push_back(std::move(t));
    return &notify_.back();
}

void App::SaveCacheNow(bool force) {
    if (!cache_dirty_ && !force) return;
    int64_t now = NowMono();
    if (!force && now - last_cache_save_mono_ < 15 * 60 * 1000) return; // 合并 ≥15 分钟
    CacheData c;
    c.saved_at_utc = NowUnix();
    // 未配置/无成功数据的快照不落盘（避免把"待配置"当缓存）；codex 不入盘
    for (const auto& a : accounts_) {
        if (a.cfg.type == AccountType::Codex) continue;
        if (a.snap.last_success_at_utc > 0) c.items.push_back(a.snap);
    }
    if (!c.items.empty()) {
        if (SaveCache(c)) {
            cache_dirty_ = false;
            last_cache_save_mono_ = now;
        }
    }
}

bool App::SaveSecretsNow() {
    std::vector<AccountConfig> accs;
    std::vector<std::string> keys;
    for (const auto& a : accounts_) {
        accs.push_back(a.cfg);
        keys.push_back(a.key);
    }
    if (!SaveSecrets(accs, keys)) {
        Log::Write(LogLevel::Error, "secrets", "密钥保存失败");
        return false;
    }
    return true;
}

std::vector<DetailCard> App::DetailCards(bool visible_only) const {
    return BuildDetailCards(AccountViews(false, visible_only), NowUnix(),
                            cfg_.EffectiveIntervalSec(), &history_);
}

std::vector<AccountView> App::AccountViews(bool enabled_only, bool visible_only) const {
    std::vector<AccountView> out;
    for (const auto& a : accounts_) {
        if (enabled_only && !a.cfg.enabled) continue;
        if (visible_only && !a.cfg.card_visible) continue;
        out.push_back({&a.cfg, &a.snap});
    }
    return out;
}

bool App::AccountHasTrendFor(const std::string& id) const {
    const AccountState* st = nullptr;
    for (const auto& a : accounts_)
        if (a.cfg.id == id) { st = &a; break; }
    if (!st) return false;
    return AccountHasTrend(history_, st->cfg, st->snap);
}

// ---------- 显示刷新 ----------
void App::RefreshDisplay() {
    if (!overlay_ && !slot_) return;
    const bool embed = cfg_.display.mode == DisplayMode::Embed;
    DisplayOptions opt;
    // 内嵌条固定单行（运行时强制，不改持久 two_lines 配置）
    opt.two_lines = embed ? false : cfg_.display.two_lines;
    // 自动字色:内嵌文字底色是任务栏主题表面(深色任务栏→白字),与浮窗(壁纸底色)相反
    opt.dark_text = cfg_.display.text_color_mode == 2 ||
                    (cfg_.display.text_color_mode == 0 &&
                     (embed ? AppsUseLightTheme() : !AppsUseLightTheme()));
    opt.low_percent_threshold = cfg_.notify.kimi_threshold_pct;
    opt.low_money_cny = cfg_.notify.deepseek_threshold_cny;
    opt.low_quota_color = cfg_.display.low_quota_color;
    opt.brand_color_names = cfg_.display.brand_color_names;
    opt.embed_show_windows = cfg_.display.embed_show_windows;
    // 常驻条/圆环成员 = 启用的账户（停用即隐藏；卡片显隐只影响详情页）
    std::vector<AccountView> live = AccountViews(true, false);
    DisplayModel model = BuildDisplayModel(live, opt, NowUnix(), NowMono(),
                                           cfg_.EffectiveIntervalSec());
    if (embed) {
        // 嵌入期浮窗整体抑制：TaskbarCreated/DisplayChange/DPI 等事件路径的
        // Relayout 不得再点亮浮窗（每次都 Hide：即使状态失配也按窗口实际可见性纠正）
        if (overlay_) {
            overlay_->SetSuppressed(true);
            overlay_->Hide();
        }
        if (slot_) {
            // 嵌入两行：第一行账户别名，第二行额度（列对齐）
            std::vector<EmbedRowItem> rows =
                BuildEmbedItems(live, opt, NowUnix(), NowMono(),
                                cfg_.EffectiveIntervalSec());
            slot_->Update(rows, cfg_.display);
        }
        SetTimer(hwnd_, kTimerSlotPoll, 1000, nullptr); // 幂等：嵌入期常开
    } else if (cfg_.display.mode == DisplayMode::Rings) {
        if (overlay_) overlay_->SetSuppressed(false);
        KillTimer(hwnd_, kTimerSlotPoll);
        if (slot_) slot_->Deactivate();
        std::vector<RingItem> rings =
            BuildRingItems(live, opt, NowUnix(), NowMono(), cfg_.EffectiveIntervalSec());
        overlay_->UpdateRings(rings, cfg_.display, false);
    } else {
        if (overlay_) overlay_->SetSuppressed(false);
        KillTimer(hwnd_, kTimerSlotPoll);
        if (slot_) slot_->Deactivate();
        if (overlay_) overlay_->Update(model, cfg_.display);
    }
    if (tray_) {
        tray_->UpdateTip(TipText_());
        tray_->UpdateDetailText();
    }
}

// ---------- 托盘提示（单行摘要） ----------
std::wstring App::TipText_() const {
    // 由显示模型文本直接拼接：与常驻一致、不含颜色
    DisplayOptions opt;
    opt.two_lines = false;
    opt.dark_text = false;
    DisplayModel m = BuildDisplayModel(AccountViews(true, false), opt, NowUnix(), NowMono(),
                                       cfg_.EffectiveIntervalSec());
    std::wstring tip;
    for (const auto& line : m.lines) {
        if (!tip.empty()) tip += L" | ";
        for (const auto& seg : line) tip += seg.text;
    }
    if (tip.empty()) tip = L"QuotaBar";
    return tip;
}

// ---------- 全屏事件 ----------
void App::FullscreenEvent() {
    if (overlay_) overlay_->OnFullscreenCheck();
    // F11 等切换不产生前台事件：一次性复查覆盖（§10.3）
    SetTimer(hwnd_, kTimerFsCheck, 2000, nullptr);
}

// ---------- 命令实现 ----------
void App::CmdRefreshAll() {
    // 逐账户触发并汇总拒绝原因：刷新被节流/限频/未配置时不再静默（第十一轮用户
    // 反馈"点了没反应"）。窗口开着走窗内回显条；窗口关着回落托盘气泡。
    std::vector<ManualKick> kicks;
    kicks.reserve(accounts_.size());
    int retry_max = 0;
    for (const auto& a : accounts_) {
        int ra = 0;
        kicks.push_back(sched_->RequestManual(a.cfg.id, &ra));
        retry_max = std::max(retry_max, ra);
    }
    ManualKickSummary sum = SummarizeManualKicks(kicks);
    sum.retry_after_sec = retry_max;
    int kind = 0;
    std::wstring text;
    ManualKickEchoText(sum, &kind, &text);
    if (tray_) tray_->ShowEcho(kind, text);
    if (tray_) tray_->RefreshMenuState();
}
void App::CmdToggleDetail() { if (tray_) tray_->ToggleDetail(); }
void App::CmdOpenQuota() { if (tray_) tray_->ShowMainAt(kPageQuota); }
void App::CmdOpenDisplay() { if (tray_) tray_->ShowMainAt(kPageDisplay); }

void App::CmdTogglePause() {
    cfg_.query.pause = !cfg_.query.pause;
    CommitConfig(cfg_);
}
void App::CmdToggleTrayOnly() {
    // 「仅托盘」往返记忆:回到上次非托盘文字模式(浮条/内嵌/圆环均可)
    if (cfg_.display.mode == DisplayMode::TrayOnly) {
        cfg_.display.mode = last_text_mode_;
    } else {
        last_text_mode_ = cfg_.display.mode;
        cfg_.display.mode = DisplayMode::TrayOnly;
    }
    config_dirty_ = true;
    SaveConfigIfDirty_();
    RefreshDisplay();
    if (tray_) tray_->RefreshMenuState();
}
void App::CmdExit() { RequestExit(); }

void App::CmdClearLocalData() {
    // 调用方已确认；清空数据并复位内存态（§11）
    WipeLocalData();
    cfg_ = AppConfig{};
    cfg_.accounts = AppConfig::Defaults();
    accounts_.clear();
    for (const auto& a : cfg_.accounts) {
        AccountState st;
        st.cfg = a;
        accounts_.push_back(std::move(st));
    }
    cache_dirty_ = false;
    config_dirty_ = false;
    history_ = UsageHistory{}; // 历史随本地数据一并清除
    history_dirty_ = false;
    notify_.clear();           // 通知滞回/重置跟踪复位
    if (tray_) tray_->RefreshMenuState();
    RefreshDisplay();
    Log::Write(LogLevel::Info, "app", "本地数据已清除");
}

// 显示设置即调即存：只替换 display 分组并立即刷新常驻条/圆环/嵌入槽。
// 账户、刷新间隔等仍走完整 CommitConfig（草稿+保存键语义）。返回落盘结果。
bool App::ApplyDisplayLive(const DisplayConfig& disp) {
    cfg_.display = disp;
    config_dirty_ = true;
    bool ok = SaveConfigIfDirty_();
    RefreshDisplay();
    if (tray_) tray_->RefreshMenuState();
    return ok;
}

// 嵌入条偏移落盘（±200 DIP 钳制与「调整位置」对话框一致；滑杆抬起/± 按钮路径）
bool App::CommitSlotOffsetDip(int dip) {
    cfg_.display.slot_offset_x_dip = std::min(std::max(dip, -200), 200);
    config_dirty_ = true;
    bool ok = SaveConfigIfDirty_();
    RefreshDisplay();
    return ok;
}

// 浮条自由位置落盘（滑杆抬起路径；ApplyPosition 落位时还会按工作区可见性二次钳制）
bool App::CommitFreePos(int x_dip, int y_dip) {
    cfg_.display.free_pos_x_dip = std::min(std::max(x_dip, 0), 4000);
    cfg_.display.free_pos_y_dip = std::min(std::max(y_dip, 0), 4000);
    config_dirty_ = true;
    bool ok = SaveConfigIfDirty_();
    RefreshDisplay();
    return ok;
}

// 吸附开关/水平偏移落盘（吸附偏移 Free 吸附态与 Rings 共用）
bool App::CommitSnap(bool snap, int snap_x_dip) {
    cfg_.display.bar_snap = snap;
    cfg_.display.bar_snap_x_dip = std::min(std::max(snap_x_dip, -200), 200);
    config_dirty_ = true;
    bool ok = SaveConfigIfDirty_();
    RefreshDisplay();
    return ok;
}

bool App::CommitConfig(const AppConfig& next) {
    bool autostart_changed = next.query.autostart != cfg_.query.autostart;
    bool pause_changed = next.query.pause != cfg_.query.pause;
    // account_seq/first_run_done 单调字段以运行值为准：next 常来自 RebuildPage 的
    // 草稿快照，若其间发生过 AddAccount，草稿里的旧 seq 会回退——下一次新增账户
    // 生成与现存账户相同的 id，cache/secrets/调度按 id 路由全部错乱
    const int64_t seq = std::max(next.account_seq, cfg_.account_seq);
    const bool first_run = next.first_run_done || cfg_.first_run_done;
    cfg_ = next;
    cfg_.account_seq = seq;
    cfg_.first_run_done = first_run;
    // 显示页草稿不含账户（账户开关即调即存），但防御性保持 accounts_ 为准
    SyncCfgAccounts_();
    if (autostart_changed) {
        // 注册表写入独立设置项；失败时回拍真实开关态（UI 不得显示假成功，
        // 调用方比对 cfg 与提交前的期望值即可发现并回显），重试留在下次保存
        if (!SetAutostart(cfg_.query.autostart)) {
            Log::Write(LogLevel::Error, "config", "自启动写入失败");
            cfg_.query.autostart = !cfg_.query.autostart;
        }
    }
    config_dirty_ = true;
    bool ok = SaveConfigIfDirty_();
    if (sched_) sched_->OnSettingsChanged(); // 暂停/间隔/账户变化后重算
    RefreshDisplay();
    if (tray_) tray_->RefreshMenuState();
    (void)pause_changed;
    return ok;
}

// ---------- 阈值通知与重置提醒（B 迭代 2026-09-15 重构） ----------
// 跟踪恒续（无论通知开关），保证关闭再开启后不误报陈旧状态；提醒按开关发出。
// 百分比账户：每账户阈值（alert_pct：0=继承全局 1=关闭 5..95=自定义）+ 滞回（回升 +5% 重臂）。
// 金额账户：全局 ¥ 阈值（显示设置配置）+ ¥2 滞回。codex 不参与阈值提醒，仅重置检测。
void App::OnNotifyThreshold(const AccountConfig& acc) {
    const ProviderSnapshot* snap = FindSnap(acc.id);
    if (!snap) return;
    NotifyTrack* tr = FindTrack_(acc.id);
    std::wstring name = Utf8ToWide(acc.alias.empty() ? ProviderDefaultAlias(acc.type)
                                                     : acc.alias);

    // ---- 已知窗口的最小剩余% 与主窗口重置时刻（重置检测跨类型通用） ----
    int best = 101;
    int64_t primary_reset = 0;
    for (const auto& w : snap->windows) {
        if (w.availability != Availability::Known || w.remaining_percent < 0) continue;
        best = std::min(best, (int)w.remaining_percent);
        if (w.has_reset && w.reset_at_utc > primary_reset) primary_reset = w.reset_at_utc;
    }

    // ---- 重置恢复提醒：上一周期临近耗尽（≤25%）且重置时刻推进 → 新周期已恢复 ----
    if (tr->last_reset > 0 && primary_reset > tr->last_reset && tr->last_pct >= 0 &&
        tr->last_pct <= 25 && best > tr->last_pct && best <= 100 &&
        cfg_.notify.enabled && cfg_.notify.reset_reminder) {
        wchar_t buf[96];
        swprintf_s(buf, L"%s 额度已重置，当前剩余 %d%%", name.c_str(), best);
        Notify_(L"QuotaBar 额度已恢复", buf);
    }
    if (primary_reset > 0) tr->last_reset = primary_reset;
    if (best != 101) tr->last_pct = best;

    if (!cfg_.notify.enabled) return;

    if (acc.type == AccountType::Kimi || acc.type == AccountType::Glm) {
        if (best == 101) return; // 暂无已知窗口
        int thr = acc.alert_pct == 1 ? 0 // 关闭
                  : acc.alert_pct > 0 ? acc.alert_pct
                                      : std::min(std::max(cfg_.notify.kimi_threshold_pct, 1),
                                                 99);
        if (thr <= 0) return;
        bool below = best <= thr;
        bool rearm = best > thr + 5; // 滞回：回到阈值+5% 以上才可再次提醒
        if (!below) {
            if (rearm) tr->below_fired = false;
            return;
        }
        if (tr->below_fired) return; // 已提醒过，仍低于阈值
        tr->below_fired = true;
        wchar_t buf[96];
        swprintf_s(buf, L"%s 剩余额度 %d%%（低于 %d%%）", name.c_str(), best, thr);
        Notify_(L"QuotaBar 额度提醒", buf);
    } else if (acc.type == AccountType::Deepseek) {
        const MoneyBalance* primary = PrimaryBalance(snap->balances);
        if (!primary || !primary->has_total) return;
        Decimal d = ParseDecimal(primary->total_raw);
        Decimal zero{}; zero.valid = true;
        if (!d.valid) return;
        char tb[32];
        sprintf_s(tb, "%.2f", cfg_.notify.deepseek_threshold_cny);
        Decimal thr = ParseDecimal(tb);
        char rearm_buf[32];
        sprintf_s(rearm_buf, "%.2f", cfg_.notify.deepseek_threshold_cny + 2.0); // ¥2 滞回
        Decimal rearm = ParseDecimal(rearm_buf);
        bool below = CompareDecimal(d, thr) <= 0;
        bool rearmed = CompareDecimal(d, rearm) > 0;
        if (!below) {
            if (rearmed) tr->below_fired = false;
            return;
        }
        if (tr->below_fired) return;
        tr->below_fired = true;
        wchar_t buf[96];
        swprintf_s(buf, L"%s 余额 %hs，低于 %hs", name.c_str(),
                   FormatMoney(d, primary->currency).c_str(),
                   FormatMoney(thr, primary->currency).c_str());
        Notify_(L"QuotaBar 余额提醒", buf);
    }
    // codex：无阈值提醒定义（百分比窗口仅参与重置检测）
}

bool App::SaveConfigIfDirty_() {
    if (!config_dirty_) return true;
    bool ok = SaveConfig(cfg_);
    if (ok) config_dirty_ = false;
    return ok;
}

} // namespace qb
