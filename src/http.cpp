#include "http.hpp"
#include "providers.hpp"
#include "util.hpp"
#include "log.hpp"
#include <winhttp.h>
#include <atomic>
#include <map>
#include <mutex>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace qb {

#ifndef APP_VERSION
#define APP_VERSION L"1.0.0"
#endif
static std::wstring UserAgentW() {
    return L"QuotaBar/" + std::wstring(APP_VERSION);
}

// 响应正文上限（解压后）
constexpr size_t kMaxBody = 64 * 1024;

// UI 消息体：携带事件与 ctx 的 shared_ptr（跨线程保活）
struct HttpEventMsg {
    std::shared_ptr<void> ctx;   // 保持存活
    DWORD event = 0;
    DWORD_PTR value = 0;         // REQUEST_ERROR: dwError；DATA_AVAILABLE: 可用字节数；READ_COMPLETE: 读取字节数
};

struct HttpClient::Impl {
    HWND wnd = nullptr;
    UINT msg = 0;
    DoneFn on_done;
    HINTERNET session = nullptr;
    bool dead = false;

    std::atomic<int64_t> next_id{1};
    std::atomic<LONG> timer_seq{1000};

    struct Ctx {
        int64_t id = 0;
        std::string provider;
        std::string account;
        int64_t cred_rev = 0;
        HINTERNET connect = nullptr;
        HINTERNET request = nullptr;
        std::atomic<LONG> finished{0};     // 已生成最终响应
        bool cancelled = false;
        bool too_large = false;
        DWORD err = 0;
        int status = 0;
        std::string retry_after;
        std::string body;
        size_t read_offset = 0;            // 本次 ReadData 写入位置
        int64_t start_mono = 0;
        UINT_PTR timer = 0;
        Impl* owner = nullptr;
    };

    // raw 指针 → shared_ptr 映射（回调线程查找）；HANDLE_CLOSING 时移除
    std::mutex mu;
    std::map<Ctx*, std::shared_ptr<Ctx>> raw_map;

    static void CALLBACK StatusCallback(HINTERNET, DWORD_PTR ctx_ptr, DWORD status,
                                        LPVOID info, DWORD info_len) {
        auto* raw = (Ctx*)ctx_ptr;
        Impl* self = nullptr;
        std::shared_ptr<Ctx> ctx;
        HWND wnd = nullptr;
        UINT msg = 0;
        {
            // 有界工作：查表保活，其余投递到 UI 线程。
            // wnd/msg 必须在锁内快照：锁外再读 self-> 是与 Shutdown→Finalize
            // (delete impl_) 之间的竞态窗口
            self = raw ? raw->owner : nullptr;
            if (!self || self->dead) return;
            std::lock_guard<std::mutex> lk(self->mu);
            auto it = self->raw_map.find(raw);
            if (it == self->raw_map.end()) return; // 句柄已关闭
            ctx = it->second;
            wnd = self->wnd;
            msg = self->msg;
        }
        auto* m = new HttpEventMsg();
        m->ctx = ctx;
        m->event = status;
        if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR && info) {
            WINHTTP_ASYNC_RESULT* res = (WINHTTP_ASYNC_RESULT*)info;
            m->value = res ? (DWORD_PTR)res->dwError : 0;
        } else if (status == WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE && info && info_len >= 4) {
            m->value = (DWORD_PTR) * (DWORD*)info;
        } else if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) {
            m->value = (DWORD_PTR)info_len;
        }
        if (!PostMessage(wnd, msg, 0, (LPARAM)m)) {
            delete m; // 队列满/窗口已销毁：Pump 不会再接手，必须就地释放
        }
    }

    void ClearTimer(Ctx* ctx) {
        if (ctx->timer) {
            KillTimer(wnd, ctx->timer);
            ctx->timer = 0;
        }
    }
    void CloseHandles(Ctx* ctx) {
        if (ctx->request) {
            HINTERNET h = ctx->request;
            ctx->request = nullptr;
            WinHttpCloseHandle(h);
        }
        if (ctx->connect) {
            HINTERNET h = ctx->connect;
            ctx->connect = nullptr;
            WinHttpCloseHandle(h);
        }
    }
    void Finish(std::shared_ptr<Ctx> ctx, bool network_error) {
        if (ctx->finished.exchange(1)) return;
        ClearTimer(ctx.get());
        CloseHandles(ctx.get());
        HttpResponse r;
        r.request_id = ctx->id;
        r.provider_id = ctx->provider;
        r.account_id = ctx->account;
        r.credential_revision = ctx->cred_rev;
        r.error_code = ctx->err;
        r.http_status = ctx->status;
        r.retry_after = ctx->retry_after;
        r.body = std::move(ctx->body);
        r.too_large = ctx->too_large;
        r.cancelled = ctx->cancelled;
        r.elapsed_ms = NowMono() - ctx->start_mono;
        r.network_error = network_error;
        if (on_done) on_done(std::move(r));
        // raw_map 移除发生在 HANDLE_CLOSING（保证回调仍可保活）
    }
    void DropRaw(Ctx* raw) {
        std::lock_guard<std::mutex> lk(mu);
        raw_map.erase(raw);
    }
};

bool HttpClient::Init(HWND notify_wnd, UINT msg, DoneFn on_done) {
    impl_ = new Impl();
    impl_->wnd = notify_wnd;
    impl_->msg = msg;
    impl_->on_done = std::move(on_done);

    impl_->session = WinHttpOpen(UserAgentW().c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                                 WINHTTP_FLAG_ASYNC);
    if (!impl_->session) {
        impl_->session = WinHttpOpen(UserAgentW().c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                                     WINHTTP_FLAG_ASYNC);
    }
    if (!impl_->session) {
        Log::Write(LogLevel::Error, "http", "WinHttpOpen 失败");
        return false;
    }
    // 阶段超时（总期限另由 10s 定时器强制）
    WinHttpSetTimeouts(impl_->session, 8000, 8000, 8000, 8000);
    WinHttpSetStatusCallback(impl_->session, Impl::StatusCallback,
                             WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS |
                                 WINHTTP_CALLBACK_STATUS_SECURE_FAILURE |
                                 WINHTTP_CALLBACK_STATUS_REDIRECT,
                             0);
    return true;
}

void HttpClient::Shutdown() {
    if (!impl_) return;
    impl_->dead = true;               // 新事件不再入队
    // 取消全部在途请求
    std::vector<std::shared_ptr<Impl::Ctx>> all;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        for (auto& kv : impl_->raw_map) {
            if (kv.second->finished.load() == 0) {
                kv.second->cancelled = true;
                all.push_back(kv.second);
            }
        }
    }
    for (auto& c : all) {
        if (c->request) WinHttpCloseHandle(c->request);
    }
    // 进程退出路径：不再等待 HANDLE_CLOSING 回调，直接清空（queued 消息由 Pump 安全处理）
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->raw_map.clear();
    }
}

bool HttpClient::Pending() const {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lk(impl_->mu);
    return !impl_->raw_map.empty();
}

void HttpClient::Finalize() {
    if (!impl_) return;
    if (impl_->session) {
        WinHttpCloseHandle(impl_->session);
        impl_->session = nullptr;
    }
    delete impl_;
    impl_ = nullptr;
}

int64_t HttpClient::Start(ProviderRequest&& req) {
    if (!impl_ || !impl_->session || impl_->dead) return 0;
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[1024] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 1023;
    std::wstring url = Utf8ToWide(req.spec.url);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.dwHostNameLength == 0) {
        Log::Write(LogLevel::Error, "http", "URL 解析失败");
        return 0;
    }

    auto ctx = std::make_shared<Impl::Ctx>();
    ctx->id = impl_->next_id++;
    ctx->provider = req.provider_id;
    ctx->account = req.account_id;
    ctx->cred_rev = req.credential_revision;
    ctx->owner = impl_;
    ctx->start_mono = NowMono();

    ctx->connect = WinHttpConnect(impl_->session, host, uc.nPort, 0);
    if (!ctx->connect) {
        Log::WriteF(LogLevel::Error, "http", "连接失败 provider=%s", req.provider_id.c_str());
        return 0;
    }
    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    ctx->request = WinHttpOpenRequest(ctx->connect, L"GET", path, nullptr,
                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!ctx->request) {
        Log::WriteF(LogLevel::Error, "http", "OpenRequest 失败 provider=%s",
                    req.provider_id.c_str());
        HINTERNET h = ctx->connect;
        ctx->connect = nullptr;
        WinHttpCloseHandle(h);
        return 0;
    }
    // 禁用自动重定向：携带密钥的请求不跟随跨域（§9.2）
    DWORD disable = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(ctx->request, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
    WinHttpSetStatusCallback(ctx->request, Impl::StatusCallback,
                             WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS |
                                 WINHTTP_CALLBACK_STATUS_SECURE_FAILURE |
                                 WINHTTP_CALLBACK_STATUS_REDIRECT, 0);

    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->raw_map[ctx.get()] = ctx;
    }
    std::wstring headers =
        L"Accept: application/json\r\nAccept-Encoding: identity\r\nAuthorization: " +
        Utf8ToWide(req.auth_header) + L"\r\n";
    for (const auto& kv : req.extra_headers) {
        headers += Utf8ToWide(kv.first) + L": " + Utf8ToWide(kv.second) + L"\r\n";
    }
    if (!WinHttpSendRequest(ctx->request, headers.c_str(), (DWORD)headers.size(),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, (DWORD_PTR)ctx.get())) {
        DWORD gle = GetLastError();
        Log::WriteF(LogLevel::Error, "http", "发送失败 provider=%s", req.provider_id.c_str());
        {
            std::lock_guard<std::mutex> lk(impl_->mu);
            impl_->raw_map.erase(ctx.get());
        }
        HINTERNET r = ctx->request; ctx->request = nullptr;
        HINTERNET c = ctx->connect; ctx->connect = nullptr;
        WinHttpCloseHandle(r);
        WinHttpCloseHandle(c);
        (void)gle;
        return 0;
    }
    // 10s 总期限
    UINT_PTR timer = (UINT_PTR)impl_->timer_seq++;
    ctx->timer = timer;
    SetTimer(impl_->wnd, timer, 10000, nullptr);
    return ctx->id;
}

void HttpClient::Cancel(int64_t request_id) {
    if (!impl_) return;
    std::shared_ptr<Impl::Ctx> ctx;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        for (auto& kv : impl_->raw_map) {
            if (kv.second->id == request_id) { ctx = kv.second; break; }
        }
    }
    if (!ctx || ctx->finished.load()) return;
    ctx->cancelled = true;
    if (ctx->request) WinHttpCloseHandle(ctx->request); // → HANDLE_CLOSING → DropRaw + Finish
}

size_t HttpClient::InFlightCount() const {
    if (!impl_) return 0;
    std::lock_guard<std::mutex> lk(impl_->mu);
    size_t n = 0;
    for (auto& kv : impl_->raw_map) {
        if (kv.second->finished.load() == 0) ++n;
    }
    return n;
}

void HttpClient::OnTimer(UINT_PTR timer_id) {
    if (!impl_) return;
    // timer_id 即 ctx->timer；找到并取消
    std::shared_ptr<Impl::Ctx> ctx;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        for (auto& kv : impl_->raw_map) {
            if (kv.second->timer == timer_id) { ctx = kv.second; break; }
        }
    }
    if (ctx && ctx->finished.load() == 0) {
        ctx->cancelled = true;   // 超时 → 取消
        if (ctx->request) WinHttpCloseHandle(ctx->request);
    }
}

void HttpClient::Pump(void* msg_ptr) {
    if (!impl_) {
        delete (HttpEventMsg*)msg_ptr;
        return;
    }
    std::unique_ptr<HttpEventMsg> m((HttpEventMsg*)msg_ptr);
    auto ctx = std::static_pointer_cast<Impl::Ctx>(m->ctx);
    if (!ctx) return;
    // 完成后仅接受 HANDLE_CLOSING（用于清理 raw_map）
    if (ctx->finished.load() && m->event != WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) return;

    DWORD event = m->event;
    switch (event) {
        case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
            if (!WinHttpReceiveResponse(ctx->request, nullptr)) {
                ctx->err = GetLastError();
                impl_->Finish(ctx, true);
            }
            break;

        case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE: {
            DWORD status = 0, size = sizeof(status);
            if (WinHttpQueryHeaders(ctx->request,
                                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                    WINHTTP_NO_HEADER_INDEX)) {
                ctx->status = (int)status;
            }
            // 诊断:内容长度/编码(不落内容)
            DWORD cl = 0, clsz = sizeof(cl);
            wchar_t ce[32] = {};
            DWORD cesz = sizeof(ce);
            bool has_cl = WinHttpQueryHeaders(
                ctx->request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &cl, &clsz, WINHTTP_NO_HEADER_INDEX);
            bool has_ce = WinHttpQueryHeaders(ctx->request, WINHTTP_QUERY_CONTENT_ENCODING,
                                              WINHTTP_HEADER_NAME_BY_INDEX, ce, &cesz,
                                              WINHTTP_NO_HEADER_INDEX);
            Log::WriteF(LogLevel::Info, "http", "%s headers status=%d cl=%s%zu enc=%s",
                        ctx->provider.c_str(), ctx->status,
                        has_cl ? "" : "?", has_cl ? (size_t)cl : 0,
                        has_ce ? WideToUtf8(ce).c_str() : "-");
            wchar_t ra[64] = {};
            DWORD rasize = sizeof(ra);
            if (WinHttpQueryHeaders(ctx->request, WINHTTP_QUERY_RETRY_AFTER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, ra, &rasize,
                                    WINHTTP_NO_HEADER_INDEX)) {
                ctx->retry_after = WideToUtf8(ra);
            }
            // 标准异步模式：HEADERS 后必须 QueryDataAvailable 一次以推动首个 DATA_AVAILABLE
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(ctx->request, &avail)) {
                ctx->err = GetLastError();
                impl_->Finish(ctx, true);
            }
            break;
        }

        case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE: {
            DWORD avail = (DWORD)m->value;
            if (avail == 0) {
                impl_->Finish(ctx, false); // 结束帧（仅此单点判流结束）
                break;
            }
            if (ctx->body.size() + avail > kMaxBody) {
                ctx->too_large = true;
                impl_->Finish(ctx, false);
                break;
            }
            ctx->read_offset = ctx->body.size();
            ctx->body.resize(ctx->read_offset + avail);
            // 只读一次：按 DATA_AVAILABLE 帧尺寸
            if (!WinHttpReadData(ctx->request, ctx->body.data() + ctx->read_offset, avail, 0)) {
                ctx->body.resize(ctx->read_offset);
                ctx->err = GetLastError();
                impl_->Finish(ctx, true);
            }
            break;
        }

        case WINHTTP_CALLBACK_STATUS_READ_COMPLETE: {
            size_t read = (size_t)m->value;
            if (ctx->body.size() > ctx->read_offset + read) {
                ctx->body.resize(ctx->read_offset + read); // 实际读取量修正
            }
            // 推动下一块：QueryDataAvailable（异步回调触发 DATA_AVAILABLE）→ 在事件中读
            DWORD next = 0;
            if (!WinHttpQueryDataAvailable(ctx->request, &next)) {
                ctx->err = GetLastError();
                impl_->Finish(ctx, true);
            }
            break;
        }

        case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
            // 取消与真实错误都按 network_error 收尾（cancelled 标志在 Finish 里
            // 决定调度器是否计入退避），两路终点一致
            ctx->err = (DWORD)m->value;
            impl_->Finish(ctx, true);
            break;

        case WINHTTP_CALLBACK_STATUS_SECURE_FAILURE: {
            // lpvStatusInformation 指向 WINHTTP_CALLBACK_SECURE_FLAG_* 标志位 DWORD
            // （不是错误码本身）；置 0x80000000 标记位与 Win32 错误码区分，
            // SecureFailureText() 据此解码人类可读原因
            const DWORD bits = m->value ? *(const DWORD*)(uintptr_t)m->value : 0;
            ctx->err = bits ? (0x80000000u | bits) : (DWORD)ERROR_WINHTTP_SECURE_FAILURE;
            impl_->Finish(ctx, true);
            break;
        }

        case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
            impl_->DropRaw(ctx.get());
            if (ctx->finished.load() == 0) {
                // 从未完成（Shutdown/超时取消）→ 以取消结束，调度器据此回退
                ctx->cancelled = true;
                impl_->Finish(ctx, true);
            }
            break;

        default:
            break;
    }
}

const char* SecureFailureText(DWORD error_code, char* buf, size_t cap) {
    if (!(error_code & 0x80000000u)) return nullptr;
    const DWORD bits = error_code & 0x7FFFFFFFu;
    // WINHTTP_CALLBACK_SECURE_FLAG_*（winhttp.h 值固定，显式列出避免依赖头版本）
    struct { DWORD bit; const char* text; } tab[] = {
        {0x01, "CN不符"}, {0x02, "证书过期"}, {0x04, "未知CA"},
        {0x08, "CN不符(忽略)"}, {0x10, "日期无效(忽略)"}, {0x20, "用途错误"},
        {0x40, "吊销检查失败"}, {0x80, "证书已吊销"},
    };
    std::string out = "TLS校验失败";
    bool any = false;
    for (const auto& e : tab) {
        if (bits & e.bit) {
            out += any ? "|" : ":";
            out += e.text;
            any = true;
        }
    }
    if (!any) {
        char hex[16];
        snprintf(hex, sizeof(hex), "%lx", (unsigned long)bits);
        out += std::string(":未知位0x") + hex;
    }
    snprintf(buf, cap, "%s", out.c_str());
    return buf;
}

} // namespace qb
