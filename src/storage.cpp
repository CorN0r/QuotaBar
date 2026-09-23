#include "storage.hpp"
#include "providers.hpp"
#include "util.hpp"
#include "json.hpp"
#include "log.hpp"
#include <wincrypt.h>
#include <shlobj.h>
#include <cstdio>
#include <algorithm>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "crypt32.lib")   // DPAPI（CryptProtectData）
#pragma comment(lib, "shell32.lib")

namespace qb {

std::wstring GetDataDir() {
    static std::wstring dir = [] {
        wchar_t* p = nullptr;
        SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p);
        std::wstring d = p ? p : L"C:\\ProgramData";
        CoTaskMemFree(p);
        d += L"\\QuotaBar";
        CreateDirectoryW(d.c_str(), nullptr);
        return d;
    }();
    return dir;
}
std::wstring GetLogDir() { return GetDataDir() + L"\\logs"; }
std::wstring GetConfigPath() { return GetDataDir() + L"\\config.json"; }
std::wstring GetSecretsPath() { return GetDataDir() + L"\\secrets.dat"; }
std::wstring GetCachePath() { return GetDataDir() + L"\\cache.json"; }
std::wstring GetHistoryPath() { return GetDataDir() + L"\\history.json"; }

namespace {

// 临时文件 + 原子替换
bool AtomicWrite(const std::wstring& path, const void* data, size_t size) {
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data, (DWORD)size, &written, nullptr);
    CloseHandle(h);
    if (!ok || written != size) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool ReadAll(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 4 * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    out.resize((size_t)sz.QuadPart);
    DWORD read = 0;
    BOOL ok = ReadFile(h, out.data(), (DWORD)out.size(), &read, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    out.resize(read);
    return true;
}

} // namespace

// ---------- config ----------
std::vector<AccountConfig> AppConfig::Defaults() {
    // 顺序 = 历史卡片序 K/C/D/G；codex 凭证版本恒 1（auth.json 免密直连）
    std::vector<AccountConfig> out;
    auto add = [&](AccountType t, const char* id) {
        AccountConfig a;
        a.id = id;
        a.type = t;
        a.alias = ProviderDefaultAlias(t);
        a.enabled = true;
        a.card_visible = true;
        a.cred_rev = t == AccountType::Codex ? 1 : 0;
        out.push_back(std::move(a));
    };
    add(AccountType::Kimi, "kimi");
    add(AccountType::Codex, "codex");
    add(AccountType::Deepseek, "deepseek");
    add(AccountType::Glm, "glm");
    return out;
}

bool LoadConfig(AppConfig& c) {
    std::string text;
    if (!ReadAll(GetConfigPath(), text)) return false;
    if (!ConfigIo::Load(text, c)) {
        Log::Write(LogLevel::Warn, "config", "config.json 损坏，使用默认配置");
        c = AppConfig{};
        c.accounts = AppConfig::Defaults();
        return false;
    }
    c.query.interval_sec = std::min(std::max(c.query.interval_sec, 60), 3600);
    if (c.accounts.empty()) c.accounts = AppConfig::Defaults();
    return true;
}
bool SaveConfig(const AppConfig& c) {
    std::string text = ConfigIo::Dump(c);
    return AtomicWrite(GetConfigPath(), text.data(), text.size());
}

// ConfigIo 实现：手写紧凑 JSON（字段固定，无需通用 dump）
namespace {
std::string JStr(const std::string& s) {
    // 转义必须走 json.cpp 的完整实现：局部版漏掉 \t/\r 等控制字符会写出
    // 非法 JSON，下次启动解析失败即整体回退默认配置（账户全部丢失，实测链路）。
    return JsonQuote(s);
}
} // namespace

std::string ConfigIo::Dump(const AppConfig& c) {
    char buf[256];
    std::string s = "{";
    s += "\"config_version\":2,";
    s += "\"display\":{";
    s += "\"mode\":" + std::to_string((int)c.display.mode) + ",";
    s += "\"free_pos_x\":" + std::to_string(c.display.free_pos_x_dip) + ",";
    s += "\"free_pos_y\":" + std::to_string(c.display.free_pos_y_dip) + ",";
    s += std::string("\"bar_snap\":") + (c.display.bar_snap ? "true" : "false") + ",";
    s += "\"bar_snap_x\":" + std::to_string(c.display.bar_snap_x_dip) + ",";
    s += "\"bar_snap_y\":" + std::to_string(c.display.bar_snap_y_dip) + ",";
    s += "\"font_size_pt\":" + std::to_string(c.display.font_size_pt) + ",";
    s += "\"text_color_mode\":" + std::to_string(c.display.text_color_mode) + ",";
    s += "\"slot_offset_x_dip\":" + std::to_string(c.display.slot_offset_x_dip) + ",";
    s += std::string("\"two_lines\":") + (c.display.two_lines ? "true" : "false") + ",";
    s += std::string("\"low_quota_color\":") + (c.display.low_quota_color ? "true" : "false") + ",";
    s += std::string("\"brand_color_names\":") + (c.display.brand_color_names ? "true" : "false") + ",";
    s += std::string("\"embed_show_names\":") + (c.display.embed_show_names ? "true" : "false") + ",";
    s += std::string("\"embed_show_windows\":") + (c.display.embed_show_windows ? "true" : "false") + ",";
    s += std::string("\"embed_brand_icons\":") + (c.display.embed_brand_icons ? "true" : "false");
    s += "},";
    s += "\"query\":{";
    s += "\"interval_sec\":" + std::to_string(c.query.interval_sec) + ",";
    s += std::string("\"power_save\":") + (c.query.power_save ? "true" : "false") + ",";
    s += std::string("\"pause\":") + (c.query.pause ? "true" : "false") + ",";
    s += std::string("\"autostart\":") + (c.query.autostart ? "true" : "false");
    s += "},";
    s += "\"notify\":{";
    s += std::string("\"enabled\":") + (c.notify.enabled ? "true" : "false") + ",";
    s += std::string("\"reset_reminder\":") + (c.notify.reset_reminder ? "true" : "false") + ",";
    s += "\"kimi_threshold_pct\":" + std::to_string(c.notify.kimi_threshold_pct) + ",";
    sprintf_s(buf, "\"deepseek_threshold_cny\":%.2f", c.notify.deepseek_threshold_cny);
    s += buf;
    s += "},";
    s += std::string("\"first_run_done\":") + (c.first_run_done ? "true" : "false") + ",";
    s += "\"ui_theme\":" + std::to_string(c.ui_theme) + ",";
    s += "\"account_seq\":" + std::to_string(c.account_seq) + ",";
    s += "\"accounts\":[";
    for (size_t i = 0; i < c.accounts.size(); ++i) {
        const AccountConfig& a = c.accounts[i];
        if (i) s += ",";
        s += "{";
        s += "\"id\":" + JStr(a.id) + ",";
        s += "\"type\":" + std::to_string((int)a.type) + ",";
        s += "\"alias\":" + JStr(a.alias) + ",";
        s += std::string("\"enabled\":") + (a.enabled ? "true" : "false") + ",";
        s += std::string("\"card_visible\":") + (a.card_visible ? "true" : "false") + ",";
        s += "\"cred_rev\":" + std::to_string(a.cred_rev) + ",";
        s += "\"alert_pct\":" + std::to_string(a.alert_pct);
        s += "}";
    }
    s += "]";
    s += "}";
    return s;
}
bool ConfigIo::Load(const std::string& text, AppConfig& c) {
    auto root = JsonParse(text);
    if (!root || !root->IsObject()) return false;
    const JsonValue& d = root->Find("display");
    int64_t mode = 0;
    const bool have_mode = d.Find("mode").AsInt(mode);
    if (have_mode) {
        c.display.mode = (DisplayMode)(int)std::min<int64_t>(std::max<int64_t>(mode, 0), 4);
        // 旧「贴靠任务栏外侧」(0) 已并入浮条：迁移为 Free + 吸附任务栏（位置语义不变）
        if (c.display.mode == DisplayMode::Docked) c.display.mode = DisplayMode::Free;
    }
    int64_t t = 0;
    if (d.Find("free_pos_x").AsInt(t)) c.display.free_pos_x_dip = (int)t;
    if (d.Find("free_pos_y").AsInt(t)) c.display.free_pos_y_dip = (int)t;
    c.display.bar_snap = d.Find("bar_snap").AsBool(false);
    if (d.Find("bar_snap_x").AsInt(t)) {
        c.display.bar_snap_x_dip = (int)std::min<int64_t>(std::max<int64_t>(t, -200), 200);
    } else if (d.Find("dock_offset_x").AsInt(t)) {
        // v2 旧键迁移：dock_offset_x（贴靠右缘偏移）→ bar_snap_x，钳制与滑杆同域
        c.display.bar_snap_x_dip = (int)std::min<int64_t>(std::max<int64_t>(t, -200), 200);
    }
    if (d.Find("bar_snap_y").AsInt(t)) {
        c.display.bar_snap_y_dip = (int)std::min<int64_t>(std::max<int64_t>(t, 0), 200);
    } else if (d.Find("dock_offset_y").AsInt(t)) {
        c.display.bar_snap_y_dip = (int)std::min<int64_t>(std::max<int64_t>(t, 0), 200);
    }
    // 旧配置 mode=Docked 且未写 bar_snap 键：贴靠语义 = 吸附开启
    if (have_mode && mode == 0 && !d.Find("bar_snap").IsBool()) c.display.bar_snap = true;
    int64_t fs = 9;
    if (d.Find("font_size_pt").AsInt(fs)) {
        c.display.font_size_pt = (int)std::min<int64_t>(std::max<int64_t>(fs, 7), 18);
    }
    if (d.Find("text_color_mode").AsInt(t)) c.display.text_color_mode = (int)t;
    if (d.Find("slot_offset_x_dip").AsInt(t))
        c.display.slot_offset_x_dip = (int)std::min<int64_t>(std::max<int64_t>(t, -200), 200);
    c.display.two_lines = d.Find("two_lines").AsBool(true);
    c.display.low_quota_color = d.Find("low_quota_color").AsBool(true);
    c.display.brand_color_names = d.Find("brand_color_names").AsBool(false);
    c.display.embed_show_names = d.Find("embed_show_names").AsBool(true);
    c.display.embed_show_windows = d.Find("embed_show_windows").AsBool(true);
    c.display.embed_brand_icons = d.Find("embed_brand_icons").AsBool(false);

    const JsonValue& q = root->Find("query");
    if (q.Find("interval_sec").AsInt(t)) c.query.interval_sec = (int)t;
    c.query.power_save = q.Find("power_save").AsBool(false);
    c.query.pause = q.Find("pause").AsBool(false);
    c.query.autostart = q.Find("autostart").AsBool(false);

    const JsonValue& n = root->Find("notify");
    c.notify.enabled = n.Find("enabled").AsBool(false);
    c.notify.reset_reminder = n.Find("reset_reminder").AsBool(true);
    int64_t thr = 20;
    if (n.Find("kimi_threshold_pct").AsInt(thr)) {
        c.notify.kimi_threshold_pct = (int)std::min<int64_t>(std::max<int64_t>(thr, 1), 99);
    }
    c.notify.deepseek_threshold_cny = n.Find("deepseek_threshold_cny").AsDouble(5.0);

    c.first_run_done = root->Find("first_run_done").AsBool(false);
    int64_t ut = 0;
    if (root->Find("ui_theme").AsInt(ut)) {
        c.ui_theme = (int)std::min<int64_t>(std::max<int64_t>(ut, 0), 2);
    }
    root->Find("account_seq").AsInt(c.account_seq);

    // ---- 账户列表（v2）；缺 accounts 键 = v1 → 按旧平铺字段迁移 ----
    const JsonValue& accs = root->Find("accounts");
    if (accs.IsArray()) {
        for (const auto& ja : accs.AsArray()) {
            AccountConfig a;
            a.id = ja.Find("id").AsString();
            int64_t ty = 0;
            ja.Find("type").AsInt(ty);
            if (ty < 0 || ty > 3) continue; // 未知类型：丢弃该条目（防御）
            a.type = (AccountType)(int)ty;
            a.alias = ja.Find("alias").AsString();
            // 别名钳制到与 UI 输入上限（EM_SETLIMITTEXT 64 宽字符）一致：外部手改
            // config.json 写入的超长别名会把通知文案的 swprintf_s(buf[96]) 撑爆
            // （secure CRT 触发 invalid parameter handler，release 直接崩溃）
            if (a.alias.size() > 64) {
                std::wstring aw = Utf8ToWide(a.alias);
                if (aw.size() > 64) {
                    aw.resize(64);
                    a.alias = WideToUtf8(aw);
                }
            }
            a.enabled = ja.Find("enabled").AsBool(true);
            a.card_visible = ja.Find("card_visible").AsBool(true);
            ja.Find("cred_rev").AsInt(a.cred_rev);
            int64_t ap = 0;
            if (ja.Find("alert_pct").AsInt(ap)) {
                a.alert_pct = (ap == 1) ? 1
                                        : (int)std::min<int64_t>(std::max<int64_t>(ap, 0), 95);
                if (a.alert_pct != 0 && a.alert_pct != 1 && a.alert_pct < 5) a.alert_pct = 0;
            }
            if (a.id.empty()) continue;
            if (a.type == AccountType::Codex) a.cred_rev = 1; // codex 无换 Key 语义
            c.accounts.push_back(std::move(a));
        }
    } else {
        // v1 迁移：show_* → card_visible（旧"常驻条开关" closest 语义）、cred_rev_* → 各账户
        struct Leg { AccountType type; const char *show_key, *rev_key; const char* id; };
        const Leg legs[4] = {
            {AccountType::Kimi, "show_kimi", "cred_rev_kimi", "kimi"},
            {AccountType::Codex, "show_codex", nullptr, "codex"},
            {AccountType::Deepseek, "show_deepseek", "cred_rev_deepseek", "deepseek"},
            {AccountType::Glm, "show_glm", "cred_rev_glm", "glm"},
        };
        for (const auto& lg : legs) {
            AccountConfig a;
            a.id = lg.id;
            a.type = lg.type;
            a.alias = ProviderDefaultAlias(lg.type);
            a.enabled = true;
            a.card_visible = d.Find(lg.show_key).AsBool(true);
            if (lg.rev_key) root->Find(lg.rev_key).AsInt(a.cred_rev);
            if (lg.type == AccountType::Codex) a.cred_rev = 1;
            c.accounts.push_back(std::move(a));
        }
        Log::Write(LogLevel::Info, "config", "v1 配置已迁移到账户模型");
    }
    return true;
}

// ---------- secrets（DPAPI 当前用户范围） ----------
namespace {
// v1 = 固定三段（kimi/deepseek/glm）；v2 = 按 config.accounts 顺序逐段。
// 两版格式相同（magic + 若干 [4 字节长度 + DPAPI blob]），仅 magic 与段语义不同；
// 新增平台无需再追加格式版本，账户增删全部整体重写。
constexpr char kMagicV1[8] = {'Q', 'B', 'A', 'R', 'S', 'E', 'C', '1'};
constexpr char kMagicV2[8] = {'Q', 'B', 'A', 'R', 'S', 'E', 'C', '2'};

bool DpapiProtect(const std::string& plain, std::string& blob) {
    DATA_BLOB in{}, out{};
    in.pbData = (BYTE*)plain.data();
    in.cbData = (DWORD)plain.size();
    // 描述串不含任何敏感信息
    if (!CryptProtectData(&in, L"QuotaBar credentials", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return false;
    }
    blob.assign((const char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}
bool DpapiUnprotect(const std::string& blob, std::string& plain) {
    DATA_BLOB in{}, out{};
    in.pbData = (BYTE*)blob.data();
    in.cbData = (DWORD)blob.size();
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return false;
    }
    plain.assign((const char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}
} // namespace

bool LoadSecrets(const std::vector<AccountConfig>& accounts, std::vector<std::string>& keys) {
    keys.assign(accounts.size(), std::string{});
    std::string data;
    if (!ReadAll(GetSecretsPath(), data)) return false; // 不存在 = 未配置
    if (data.size() < 8 ||
        (memcmp(data.data(), kMagicV1, 8) != 0 && memcmp(data.data(), kMagicV2, 8) != 0)) {
        Log::Write(LogLevel::Warn, "secrets", "secrets.dat 无法识别，已忽略");
        return false;
    }
    const bool v1 = memcmp(data.data(), kMagicV1, 8) == 0;
    size_t p = 8;
    std::vector<std::string> blobs;
    while (p + 4 <= data.size()) {
        uint32_t len;
        memcpy(&len, data.data() + p, 4);
        p += 4;
        if (len > 64 * 1024 || p + len > data.size()) break; // 缺尾视为空（旧文件不判损坏）
        blobs.emplace_back(data, p, len);
        p += len;
    }
    if (v1) {
        // v1 固定 [kimi, deepseek, glm] → 映射到账户列表中同类型首个账户
        const AccountType leg[3] = {AccountType::Kimi, AccountType::Deepseek,
                                    AccountType::Glm};
        for (size_t i = 0; i < 3 && i < blobs.size(); ++i) {
            std::string plain;
            if (!DpapiUnprotect(blobs[i], plain) || plain.empty()) continue;
            for (size_t k = 0; k < accounts.size(); ++k) {
                if (accounts[k].type == leg[i]) {
                    keys[k] = std::move(plain);
                    break;
                }
            }
        }
        Log::Write(LogLevel::Info, "secrets", "v1 secrets 已按账户模型迁移");
    } else {
        for (size_t i = 0; i < blobs.size() && i < keys.size(); ++i) {
            std::string plain;
            if (!blobs[i].empty()) DpapiUnprotect(blobs[i], plain); // 失败 → 空（未配置）
            keys[i] = std::move(plain);
        }
    }
    return true;
}
bool SaveSecrets(const std::vector<AccountConfig>& accounts,
                 const std::vector<std::string>& keys) {
    if (accounts.size() != keys.size()) return false;
    std::string data(kMagicV2, 8);
    auto write_blob = [&](const std::string& plain) {
        std::string blob;
        if (!DpapiProtect(plain, blob)) {
            Log::Write(LogLevel::Error, "secrets", "DPAPI 加密失败");
            return false;
        }
        uint32_t len = (uint32_t)blob.size();
        data.append((const char*)&len, 4);
        data.append(blob);
        return true;
    };
    for (size_t i = 0; i < accounts.size(); ++i) {
        // codex 不落盘（占位空段保持与账户列表对齐）
        if (!write_blob(accounts[i].type == AccountType::Codex ? std::string() : keys[i]))
            return false;
    }
    return AtomicWrite(GetSecretsPath(), data.data(), data.size());
}

// ---------- cache ----------
bool LoadCache(CacheData& c) {
    std::string text;
    if (!ReadAll(GetCachePath(), text)) return false;
    auto root = JsonParse(text);
    if (!root || !root->IsObject()) return false;
    root->Find("saved_at_utc").AsInt(c.saved_at_utc);
    const JsonValue& accs = root->Find("accounts");
    if (!accs.IsArray()) return false; // v1 旧缓存格式：直接废弃（重新查询即可）
    bool any = false;
    for (const auto& ja : accs.AsArray()) {
        ProviderSnapshot s;
        if (!SnapshotJson::Parse(ja.Find("snapshot").AsString(), s)) continue;
        s.from_cache = true;
        // 先取布尔再 move：push_back(move) 后 s 处于 moved-from 状态，读其字段值未指定
        bool has_ts = s.last_success_at_utc > 0;
        c.items.push_back(std::move(s));
        any = any || has_ts;
    }
    return any;
}
bool SaveCache(const CacheData& c) {
    std::string s = "{\"saved_at_utc\":" + std::to_string(c.saved_at_utc) + ",";
    s += "\"accounts\":[";
    bool first = true;
    for (const auto& it : c.items) {
        if (it.account_id.empty()) continue;
        if (!first) s += ",";
        first = false;
        s += "{\"account_id\":" + JStr(it.account_id) + ",";
        s += "\"snapshot\":" + JStr(SnapshotJson::Dump(it)) + "}";
    }
    s += "]}";
    return AtomicWrite(GetCachePath(), s.data(), s.size());
}

// ---------- history（用量历史滚动采样） ----------
// 紧凑格式：百分比样本 [t,pct]、余额样本 [t,"raw"]；无凭证字段。
// ik=账户身份锚（codex 专用，Fnv1a64Hex 哈希非原始 Id；可选字段，旧文件无 ik 照常解析）。
std::string HistoryJson::Dump(const UsageHistory& h, int64_t saved_at_utc) {
    std::string s = "{\"version\":1,";
    s += "\"saved_at_utc\":" + std::to_string(saved_at_utc) + ",";
    s += "\"accounts\":[";
    bool first_acc = true;
    for (const auto& a : h.accounts) {
        bool empty_acc = a.windows.empty() && a.balance.samples.empty();
        if (empty_acc) continue;
        if (!first_acc) s += ",";
        first_acc = false;
        s += "{\"id\":" + JStr(a.account_id);
        if (!a.identity_key.empty()) s += ",\"ik\":" + JStr(a.identity_key);
        s += ",\"windows\":[";
        bool first_w = true;
        for (const auto& w : a.windows) {
            if (w.samples.empty()) continue;
            if (!first_w) s += ",";
            first_w = false;
            s += "{\"key\":" + JStr(w.stable_key) + ",\"samples\":[";
            for (size_t i = 0; i < w.samples.size(); ++i) {
                if (i) s += ",";
                s += "[" + std::to_string(w.samples[i].t_utc) + "," +
                     std::to_string(w.samples[i].pct) + "]";
            }
            s += "]}";
        }
        s += "],\"balance\":{";
        if (a.balance.samples.size() >= 2) {
            s += "\"currency\":" + JStr(a.balance.currency) + ",\"samples\":[";
            for (size_t i = 0; i < a.balance.samples.size(); ++i) {
                if (i) s += ",";
                s += "[" + std::to_string(a.balance.samples[i].t_utc) + "," +
                     JStr(a.balance.samples[i].total_raw) + "]";
            }
            s += "]";
        }
        s += "}}";
    }
    s += "]}";
    return s;
}
bool HistoryJson::Parse(const std::string& json_text, UsageHistory& out,
                        int64_t& saved_at_utc) {
    auto root = JsonParse(json_text);
    if (!root || !root->IsObject()) return false;
    int64_t ver = 0;
    if (!root->Find("version").AsInt(ver) || ver != 1) return false;
    root->Find("saved_at_utc").AsInt(saved_at_utc);
    for (const auto& ja : root->Find("accounts").AsArray()) {
        AccountHistory a;
        a.account_id = ja.Find("id").AsString();
        if (a.account_id.empty()) continue;
        a.identity_key = ja.Find("ik").AsString();
        for (const auto& jw : ja.Find("windows").AsArray()) {
            WindowHistory w;
            w.stable_key = jw.Find("key").AsString();
            if (w.stable_key.empty()) continue;
            for (const auto& js : jw.Find("samples").AsArray()) {
                const JsonValue& arr = js;
                if (!arr.IsArray() || arr.AsArray().size() < 2) continue;
                UsageSample s;
                arr.AsArray()[0].AsInt(s.t_utc);
                int64_t pct = -1;
                arr.AsArray()[1].AsInt(pct);
                s.pct = (int32_t)pct;
                w.samples.push_back(s);
            }
            if (!w.samples.empty()) a.windows.push_back(std::move(w));
        }
        const JsonValue& bal = ja.Find("balance");
        for (const auto& js : bal.Find("samples").AsArray()) {
            const JsonValue& arr = js;
            if (!arr.IsArray() || arr.AsArray().size() < 2) continue;
            BalanceSample s;
            arr.AsArray()[0].AsInt(s.t_utc);
            s.total_raw = arr.AsArray()[1].AsString();
            a.balance.samples.push_back(std::move(s));
        }
        if (!a.balance.samples.empty())
            a.balance.currency = bal.Find("currency").AsString();
        if (!a.windows.empty() || !a.balance.samples.empty())
            out.accounts.push_back(std::move(a));
    }
    return true;
}
bool LoadHistory(UsageHistory& h) {
    std::string text;
    if (!ReadAll(GetHistoryPath(), text)) return false;
    int64_t saved = 0;
    return HistoryJson::Parse(text, h, saved);
}
bool SaveHistory(const UsageHistory& h) {
    std::string text = HistoryJson::Dump(h, NowUnix());
    return AtomicWrite(GetHistoryPath(), text.data(), text.size());
}

// ---------- 自启动 ----------
static const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t kRunValue[] = L"QuotaBar";

bool GetAutostart() {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &hk) != ERROR_SUCCESS) return false;
    wchar_t buf[MAX_PATH + 8]{};
    DWORD sz = sizeof(buf), type = 0;
    bool ok = RegQueryValueExW(hk, kRunValue, nullptr, &type, (BYTE*)buf, &sz) == ERROR_SUCCESS &&
              type == REG_SZ;
    RegCloseKey(hk);
    return ok;
}
bool SetAutostart(bool enable) {
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &hk) != ERROR_SUCCESS) return false;
    bool ok = true;
    if (enable) {
        wchar_t path[MAX_PATH + 8]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(path) + L"\"";
        ok = RegSetValueExW(hk, kRunValue, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                            (DWORD)((cmd.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    } else {
        RegDeleteValueW(hk, kRunValue);
    }
    RegCloseKey(hk);
    return ok;
}

bool WipeLocalData() {
    DeleteFileW(GetConfigPath().c_str());
    DeleteFileW(GetSecretsPath().c_str());
    DeleteFileW(GetCachePath().c_str());
    DeleteFileW(GetHistoryPath().c_str());
    for (int i = 1; i <= 2; ++i) {
        wchar_t buf[8];
        swprintf_s(buf, L"%d", i);
        DeleteFileW((GetLogDir() + L"\\quota_" + buf + L".log").c_str());
    }
    return true;
}

} // namespace qb
