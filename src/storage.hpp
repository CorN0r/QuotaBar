// storage.hpp — %LOCALAPPDATA%\QuotaBar\ 下的 config.json / secrets.dat / cache.json。
// 写入一律"临时文件 + 原子替换"；secrets.dat 用 DPAPI 当前用户范围加密。
#pragma once
#include "config.hpp"
#include "state.hpp"
#include <string>
#include <vector>

namespace qb {

std::wstring GetDataDir();
std::wstring GetLogDir();
std::wstring GetConfigPath();
std::wstring GetSecretsPath();
std::wstring GetCachePath();
std::wstring GetHistoryPath();

// config.json
bool LoadConfig(AppConfig& c);           // 文件不存在/损坏 → 默认值（不视为错误）
bool SaveConfig(const AppConfig& c);

// secrets.dat：各账户的 API Key（UTF-8；codex 不存——令牌只读 ~/.codex/auth.json）。
// v2 = magic "QBARSEC2" + 按 accounts 顺序逐段 [4 字节长度 + DPAPI blob]；
// 旧 v1（"QBARSEC1" + 固定 kimi/deepseek/glm 三段）读取时自动映射到同类型账户。
// keys 与 accounts 等长（codex 位存空串）。
bool LoadSecrets(const std::vector<AccountConfig>& accounts, std::vector<std::string>& keys);
bool SaveSecrets(const std::vector<AccountConfig>& accounts,
                 const std::vector<std::string>& keys);

// cache.json：最近成功快照（无凭证；codex 不入盘），按账户 id 存
struct CacheData {
    std::vector<ProviderSnapshot> items; // 各账户最近快照（account_id 路由）
    int64_t saved_at_utc = 0;
};
bool LoadCache(CacheData& c);
bool SaveCache(const CacheData& c);      // 失败静默（缓存非关键路径）

// history.json：用量历史滚动采样（百分比/余额，无凭证）。codex 以身份锚入盘：
// ik = ChatGPT Account Id 的 FNV-1a 哈希（非原始 Id），换账户整段重记（见 ApplyIdentityKey）。
// 结构见 state.hpp 的 UsageHistory；Dump/Parse 为纯函数（selftest 可测 roundtrip）。
struct HistoryJson {
    static std::string Dump(const UsageHistory& h, int64_t saved_at_utc);
    static bool Parse(const std::string& json_text, UsageHistory& out, int64_t& saved_at_utc);
};
bool LoadHistory(UsageHistory& h);
bool SaveHistory(const UsageHistory& h); // 失败静默（历史非关键路径）

// 自启动（HKCU Run 键，路径加引号；不使用管理员权限）
bool GetAutostart();
bool SetAutostart(bool enable);

// 清除本地数据（设置入口显式调用）：删 config/secrets/cache/history/logs
bool WipeLocalData();

} // namespace qb
