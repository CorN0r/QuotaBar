// http.hpp — WinHTTP 异步 HTTPS 客户端。
// 设计（§4/§9.2）：异步不阻塞 UI；单请求总生命周期 10s（内部定时器强制）；
// 响应解压后 ≤64KiB；禁用自动重定向；证书校验保持开启；代理遵循系统配置。
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "providers.hpp" // ProviderRequestSpec（ProviderRequest 按值持有）
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace qb {

struct ProviderRequest {
    ProviderRequestSpec spec;
    std::string auth_header;   // "Bearer <key>"（不得写入日志）
    std::string provider_id;   // 平台类型 id（"kimi"...；决定解析器）
    std::string account_id;    // 账户路由键（回程分派到对应账户快照）
    int64_t request_id = 0;
    int64_t credential_revision = 0;
    // 额外请求头（Codex 的 ChatGPT-Account-Id 等）；值不得写入日志
    std::vector<std::pair<std::string, std::string>> extra_headers;
};

struct HttpResponse {
    int64_t request_id = 0;
    std::string provider_id;   // 平台类型 id（透传自请求）
    std::string account_id;    // 账户路由键（透传自请求）
    int64_t credential_revision = 0;
    bool network_error = false;   // DNS/连接/TLS/超时/取消
    bool cancelled = false;
    bool too_large = false;       // >64KiB
    DWORD error_code = 0;
    int http_status = 0;          // 0 = 无响应
    std::string retry_after;      // 原始头（秒或 HTTP-date）
    std::string body;             // ≤64KiB
    int64_t elapsed_ms = 0;
};

class HttpClient {
public:
    using DoneFn = std::function<void(HttpResponse&&)>;  // UI 线程回调
    bool Init(HWND notify_wnd, UINT msg, DoneFn on_done);
    // 退出：先 Shutdown()（取消全部），继续抽消息直到 Pending()==false 或超时，再 Finalize()
    void Shutdown();
    bool Pending() const;
    void Finalize();

    int64_t Start(ProviderRequest&& req);   // 0 = 启动失败
    void Cancel(int64_t request_id);
    size_t InFlightCount() const;
    // app 的 WM_TIMER 转发（10s 总期限）
    void OnTimer(UINT_PTR timer_id);
    // UI 线程：处理 WinHTTP 通知消息（wParam 携带堆上消息体指针）
    void Pump(void* event_msg);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

// error_code 带 0x80000000 标记位（SECURE_FAILURE 回调）时解码 TLS 校验失败原因；
// 返回 nullptr 表示不是安全失败码。buf 由调用方提供（≥64 字节）。
const char* SecureFailureText(DWORD error_code, char* buf, size_t cap);

} // namespace qb
