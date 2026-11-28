// http.hpp — WinHTTP 异步 HTTPS 客户端。
// 设计（§4/§9.2）：异步不阻塞 UI；单请求总生命周期 10s（内部定时器强制）；
// 响应解压后 ≤64KiB；禁用自动重定向；证书校验保持开启；代理遵循系统配置。
// 2026-11-27 起支持 POST+JSON 体与自定义认证头名（Antigravity 本机 hub 的 Connect RPC）；
// 回环地址（127.0.0.1）WinHTTP 天生不经代理——会话级指向假代理的对照实验已证实
// （experiments/ag_probe/README.md），无需按请求覆盖代理。
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
    std::string auth_header;   // 认证头值："Bearer <key>" / 裸 Key / CSRF 令牌（不得写入日志）
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

// WinHTTP 会话的 User-Agent："QuotaBar/<exe 版本>"。版本读 exe 的版本资源（util::ExeVersion,
// 与侧栏页脚同源——版本号的唯一来源是 assets/quota_bar.rc）;无版本资源时退回 QuotaBar/0.0.0。
std::wstring HttpUserAgent();

} // namespace qb
