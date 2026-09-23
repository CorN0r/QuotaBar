#include "log.hpp"
#include "util.hpp"
#include <mutex>
#include <cstdio>

namespace qb {

namespace {

constexpr size_t kMaxFileBytes = 512 * 1024;
constexpr int kMaxFiles = 2;

std::mutex g_mu;
std::wstring g_dir;
std::string g_last_key;      // 合并同类：category+msg
int g_last_count = 0;
std::string g_last_line;
bool g_inited = false;

std::wstring LogPath(int idx) {
    wchar_t buf[8];
    swprintf_s(buf, L"%d", idx);
    return g_dir + L"\\quota_" + buf + L".log";
}

size_t FileSizeOrZero(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return 0;
    return ((size_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
}

void RawAppend(const std::wstring& path, const std::string& line) {
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, line.data(), (DWORD)line.size(), &written, nullptr);
    CloseHandle(h);
}

std::string LevelName(LogLevel lvl) {
    switch (lvl) {
        case LogLevel::Info: return "I";
        case LogLevel::Warn: return "W";
        default: return "E";
    }
}

} // namespace

void Log::Init(const std::wstring& data_dir) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_dir = data_dir + L"\\logs";
    CreateDirectoryW(g_dir.c_str(), nullptr);
    g_inited = true;
    // 轮转：把 1→2，1 清空，保证最新写在 1
    std::wstring p1 = LogPath(1), p2 = LogPath(2);
    if (FileSizeOrZero(p1) > kMaxFileBytes) {
        DeleteFileW(p2.c_str());
        MoveFileW(p1.c_str(), p2.c_str());
    }
}

void Log::Shutdown() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_last_count > 0 && !g_last_line.empty()) {
        RawAppend(LogPath(1), g_last_line + " (x" + std::to_string(g_last_count) + ")\n");
    }
    g_last_count = 0;
    g_last_line.clear();
    g_inited = false;
}

void Log::Write(LogLevel lvl, const std::string& category, const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_inited) return;

    std::string key = category + "|" + msg;
    std::string line = FormatLocalShort(NowUnix()) + " [" + LevelName(lvl) + "][" + category + "] " + msg;
    if (key == g_last_key) {
        // 同类合并：不立即写盘；Shutdown 或换行时补写计数
        ++g_last_count;
        g_last_line = line;
        return;
    }
    if (g_last_count > 0 && !g_last_line.empty()) {
        // 被合并的旧行发生在新行之前：先落盘旧行(带计数)，再写新行，保持文件时间序
        line = g_last_line + " (x" + std::to_string(g_last_count) + ")\n" + line;
    }
    g_last_key = key;
    g_last_count = 0;
    g_last_line.clear();
    line += "\n";

    std::wstring p1 = LogPath(1), p2 = LogPath(2);
    if (FileSizeOrZero(p1) + line.size() > kMaxFileBytes) {
        // 双文件上限：写满 → 1 移到 2（覆盖旧 2），从空 1 开始。合计 ≤ 2×512KiB。
        DeleteFileW(p2.c_str());
        MoveFileW(p1.c_str(), p2.c_str());
    }
    RawAppend(p1, line);
}

// WriteF 模板实现移至 log.hpp（保证所有调用点可实例化，不再依赖显式实例化清单）

} // namespace qb
