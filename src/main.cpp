// main.cpp — 入口：识别 --selftest（合成测试，不访问网络/不写真实数据目录），
// 否则装配 App 并运行消息循环。
#include "app.hpp"
#include "util.hpp"
#include <windows.h>
#include <cwchar>

namespace qb {
int RunSelfTests(); // tests.cpp
}

// selftest 输出控制：
//  - 结果总是追加写 exe 同目录 selftest.txt（UTF-8），保证任何环境可回读；
//  - 仅当存在“真实交互控制台”（stdout 是字符设备且可用 GetConsoleMode）时才同步打印；
//    父进程把 stdout 接成管道时（如 Claude Code / IDE 执行器）不打印，避免输出
//    混进宿主界面。GUI 正常启动（不带 --selftest）不产生任何 stdout 输出。
static void WriteOut(const wchar_t* s) {
    std::string u8 = qb::WideToUtf8(s);

    // 1) 落盘（始终）
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH)) {
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"selftest.txt");
        HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            WriteFile(f, u8.data(), (DWORD)u8.size(), &w, nullptr);
            CloseHandle(f);
        }
    }

    // 2) 真实交互控制台才打印：尝试一次附加父控制台；不再自建（避免弹黑窗）。
    static bool tried_attach = false;
    if (!tried_attach) {
        tried_attach = true;
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
            // 失败即无可用父控制台；后续按 stdout 句柄性质判断
        }
    }
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) return;
    if (GetFileType(h) != FILE_TYPE_CHAR) return;   // 管道/重定向 → 不输出
    DWORD mode = 0;
    if (!GetConsoleMode(h, &mode)) return;          // 非控制台字符设备 → 不输出
    DWORD n = 0;
    WriteConsoleW(h, s, (DWORD)wcslen(s), &n, nullptr);
}

// selftest 打印入口（tests.cpp 调用）
void SelftestPrint(const char* utf8_line) {
    std::wstring w = qb::Utf8ToWide(utf8_line);
    WriteOut(w.c_str());
}
void SelftestPrintW(const wchar_t* line) { WriteOut(line); }

int APIENTRY wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR cmd_line, int) {
    // 测试开关必须显式给出，绝不经配置开启
    if (cmd_line && wcsstr(cmd_line, L"--selftest") != nullptr) {
        return qb::RunSelfTests();
    }
    // --settings：启动后直接打开显示与查询设置（快捷入口；单实例已运行时不生效）
    bool open_settings = cmd_line && wcsstr(cmd_line, L"--settings") != nullptr;
    qb::App app;
    if (!app.Init(hinst)) return 0; // 单实例已在运行等，不弹窗
    if (open_settings) app.CmdOpenDisplay();
    return app.Run();
}
