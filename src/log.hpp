// log.hpp — 有上限的诊断日志（≤2 文件 × 512KiB，同类错误合并，内容脱敏）。
#pragma once
#include <string>

namespace qb {

enum class LogLevel { Info, Warn, Error };

class Log {
public:
    // data_dir: %LOCALAPPDATA%\QuotaBar；logs 子目录自动创建
    static void Init(const std::wstring& data_dir);
    static void Shutdown();
    // 约定：msg 不得包含 Key/Token/Authorization/完整响应/账户标识。
    static void Write(LogLevel lvl, const std::string& category, const std::string& msg);
    // 便捷：格式化。定义放在头文件，保证所有调用点可实例化。
    template <typename... A>
    static void WriteF(LogLevel lvl, const std::string& category, const char* fmt, A... args) {
        char buf[512];
        sprintf_s(buf, fmt, args...);
        Write(lvl, category, buf);
    }
};

} // namespace qb
