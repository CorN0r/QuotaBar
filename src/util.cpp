#include "util.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <cstring>
#include <vector>

#pragma comment(lib, "version.lib") // ExeVersion：GetFileVersionInfo / VerQueryValue

namespace qb {

static std::mt19937& Rng() {
    static std::mt19937 rng{std::random_device{}()};
    return rng;
}
void SeedRng() { Rng().seed((uint32_t)(std::random_device{}() ^ (uint64_t)NowMono())); }
int64_t RandRange(int64_t lo, int64_t hi) {
    if (hi <= lo) return lo;
    std::uniform_int_distribution<int64_t> d(lo, hi);
    return d(Rng());
}
int64_t JitterSeconds(int64_t base_sec, int max_pct) {
    if (max_pct <= 0 || base_sec <= 0) return 0;
    int64_t spread = base_sec * max_pct / 100;
    if (spread <= 0) return 0;
    return RandRange(0, spread);
}

int64_t NowUnix() {
    return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
int64_t NowMono() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000 / freq.QuadPart;
}
FILETIME UnixToFileTime(int64_t unix_sec) {
    ULARGE_INTEGER u;
    u.QuadPart = (uint64_t)116444736000000000ULL + (uint64_t)unix_sec * 10000000ULL;
    FILETIME ft;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    return ft;
}
int64_t FileTimeToUnix(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (int64_t)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
}
bool SystemTimeJumped(int64_t prev_unix) {
    if (prev_unix <= 0) return false;
    int64_t now = NowUnix();
    int64_t delta = now - prev_unix;
    return delta < -120 || delta > 6 * 3600; // 倒退超过2分钟或前进超过6小时视为异常跳变
}

static bool ParseDigits(std::string_view s, size_t& i, int digits_min, int digits_max, int& out) {
    int v = 0, n = 0;
    while (i < s.size() && n < digits_max && s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i] - '0'); ++i; ++n;
    }
    if (n < digits_min) return false;
    out = v;
    return true;
}

std::optional<int64_t> ParseIso8601Utc(std::string_view s) {
    std::string owned = Trim(s); // Trim 返回临时，view 不能持有（悬垂）
    s = owned;
    if (s.size() < 10) return std::nullopt;
    size_t i = 0;
    int Y, M, D, h, mi;
    if (!ParseDigits(s, i, 4, 4, Y)) return std::nullopt;
    if (i >= s.size() || s[i++] != '-') return std::nullopt;
    if (!ParseDigits(s, i, 1, 2, M)) return std::nullopt;
    if (i >= s.size() || s[i++] != '-') return std::nullopt;
    if (!ParseDigits(s, i, 1, 2, D)) return std::nullopt;
    if (Y < 1970 || Y > 9999 || M < 1 || M > 12 || D < 1 || D > 31) return std::nullopt;
    int sec = 0;
    h = mi = 0;
    if (i < s.size() && (s[i] == 'T' || s[i] == ' ')) {
        ++i;
        if (!ParseDigits(s, i, 1, 2, h)) return std::nullopt;
        if (i >= s.size() || s[i++] != ':') return std::nullopt;
        if (!ParseDigits(s, i, 1, 2, mi)) return std::nullopt;
        if (i < s.size() && s[i] == ':') {
            ++i;
            if (!ParseDigits(s, i, 1, 2, sec)) return std::nullopt;
        }
        if (h > 24 || mi > 59 || sec > 60) return std::nullopt;
        // 小数秒：允许（含纳秒精度），解析但截断到秒
        if (i < s.size() && s[i] == '.') {
            ++i;
            int frac_dummy;
            if (!ParseDigits(s, i, 1, 9, frac_dummy)) return std::nullopt;
        }
    }
    int off_min = 0;
    if (i < s.size()) {
        if (s[i] == 'Z' || s[i] == 'z') {
            ++i;
        } else if (s[i] == '+' || s[i] == '-') {
            int sign = (s[i] == '-') ? -1 : 1;
            ++i;
            int oh, om;
            if (!ParseDigits(s, i, 2, 2, oh)) return std::nullopt;
            if (i < s.size() && s[i] == ':') ++i;
            if (!ParseDigits(s, i, 2, 2, om)) return std::nullopt;
            if (oh > 23 || om > 59) return std::nullopt;
            off_min = sign * (oh * 60 + om);
        } else {
            return std::nullopt;
        }
        if (i != s.size()) return std::nullopt;
    }
    // 用 timegm 风格：直接通过 SYSTEMTIME→FILETIME（按 UTC）
    SYSTEMTIME st{};
    st.wYear = (WORD)Y; st.wMonth = (WORD)M; st.wDay = (WORD)D;
    st.wHour = (WORD)h; st.wMinute = (WORD)mi; st.wSecond = (WORD)sec;
    FILETIME ft{};
    if (!SystemTimeToFileTime(&st, &ft)) return std::nullopt;
    int64_t unix_sec = FileTimeToUnix(ft) - off_min * 60;
    return unix_sec;
}

std::optional<int64_t> ParseHttpDate(std::string_view s) {
    // 例如 "Wed, 21 Oct 2026 07:28:00 GMT"
    static const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    std::string owned = Trim(s);
    s = owned;
    size_t sp = s.find(' ');
    if (sp == std::string_view::npos) return std::nullopt;
    std::string_view rest = s.substr(sp + 1);
    size_t i = 0;
    int day = 0, year = 0, hour = 0, minute = 0, sec = 0, mon = 0;
    auto skip = [&]() { while (i < rest.size() && rest[i] == ' ') ++i; };
    skip();
    if (!ParseDigits(rest, i, 1, 2, day)) return std::nullopt;
    skip();
    if (i + 3 > rest.size()) return std::nullopt;
    std::string_view mon_s = rest.substr(i, 3); i += 3;
    for (int m = 0; m < 12; ++m) {
        if (mon_s == months[m]) { mon = m + 1; break; }
    }
    if (mon == 0) return std::nullopt;
    skip();
    if (!ParseDigits(rest, i, 4, 4, year)) return std::nullopt;
    skip();
    if (i < rest.size()) {
        if (!ParseDigits(rest, i, 1, 2, hour)) return std::nullopt;
        if (i >= rest.size() || rest[i++] != ':') return std::nullopt;
        if (!ParseDigits(rest, i, 1, 2, minute)) return std::nullopt;
        if (i < rest.size() && rest[i] == ':') { ++i; if (!ParseDigits(rest, i, 1, 2, sec)) return std::nullopt; }
    }
    SYSTEMTIME st{};
    st.wYear = (WORD)year; st.wMonth = (WORD)mon; st.wDay = (WORD)day;
    st.wHour = (WORD)hour; st.wMinute = (WORD)minute; st.wSecond = (WORD)sec;
    FILETIME ft{};
    if (!SystemTimeToFileTime(&st, &ft)) return std::nullopt;
    return FileTimeToUnix(ft);
}

static int64_t MakeUnix(int y, int m, int d, int h, int mi, int s) {
    SYSTEMTIME st{};
    st.wYear = (WORD)y; st.wMonth = (WORD)m; st.wDay = (WORD)d;
    st.wHour = (WORD)h; st.wMinute = (WORD)mi; st.wSecond = (WORD)s;
    FILETIME ft{};
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    return FileTimeToUnix(ft);
}

std::string FormatLocalShort(int64_t unix_sec) {
    FILETIME ft = UnixToFileTime(unix_sec);
    SYSTEMTIME lt{};
    FileTimeToSystemTime(&ft, &lt);
    SYSTEMTIME local{};
    SystemTimeToTzSpecificLocalTime(nullptr, &lt, &local);
    char buf[32];
    sprintf_s(buf, "%04d-%02d-%02d %02d:%02d", local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute);
    return buf;
}
std::string FormatIso8601Utc(int64_t unix_sec) {
    FILETIME ft = UnixToFileTime(unix_sec);
    SYSTEMTIME st{};
    FileTimeToSystemTime(&ft, &st);
    char buf[32];
    sprintf_s(buf, "%04d-%02d-%02dT%02d:%02d:%02dZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}
std::string FormatRemainingZh(int64_t seconds_left, bool minute_precision) {
    if (seconds_left < 0) seconds_left = 0;
    const int64_t days = seconds_left / 86400;
    const int64_t hours = (seconds_left % 86400) / 3600;
    const int64_t mins = (seconds_left % 3600) / 60;
    std::string out;
    if (minute_precision) {
        if (days) out += I64(days) + "天";
        if (days || hours) out += I64(hours) + "小时";
        if (days || hours || mins) out += I64(mins) + "分";
        if (out.empty()) out = "不足1分";
    } else {
        if (days) out += I64(days) + "天";
        if (days || hours) out += I64(hours) + "小时";
        if (out.empty()) out = "不足1小时";
    }
    return out;
}
// 供测试引用（避免 unused 警告）
int64_t MakeUnixForTest(int y, int m, int d, int h, int mi, int s) { return MakeUnix(y, m, d, h, mi, s); }

// ---------- 十进制 ----------
Decimal ParseDecimal(std::string_view s, int32_t max_scale) {
    Decimal d;
    std::string owned = Trim(s);
    s = owned;
    if (s.empty() || s.size() > 24) return d;
    size_t i = 0;
    bool neg = false;
    if (s[i] == '+' || s[i] == '-') { neg = (s[i] == '-'); ++i; }
    int64_t unit = 0;
    int32_t scale = 0;
    bool any = false, frac = false;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (c == '.') {
            if (frac) return d;
            frac = true;
            continue;
        }
        if (c < '0' || c > '9') return d;
        any = true;
        if (unit > (INT64_MAX - 9) / 10) return d; // 溢出防护
        unit = unit * 10 + (c - '0');
        if (frac) {
            if (++scale > max_scale) { scale = max_scale; unit /= 10; } // 截断
        }
    }
    if (!any) return d;
    d.unit = unit; d.scale = scale; d.neg = neg && unit != 0; d.valid = true;
    return d;
}
Decimal Rescale(const Decimal& d, int32_t scale) {
    Decimal r = d;
    if (scale > d.scale) {
        for (int32_t i = d.scale; i < scale; ++i) {
            if (r.unit > (INT64_MAX - 9) / 10) { r = d; break; }
            r.unit *= 10;
        }
        r.scale = scale;
    } else if (scale < d.scale) {
        for (int32_t i = d.scale; i > scale; --i) r.unit /= 10;
        r.scale = scale;
    }
    return r;
}
int32_t MaxScale(const Decimal& a, const Decimal& b) { return a.scale > b.scale ? a.scale : b.scale; }
int64_t CompareDecimal(const Decimal& a, const Decimal& b) {
    int32_t s = MaxScale(a, b);
    Decimal x = Rescale(a, s), y = Rescale(b, s);
    if (!x.valid && !y.valid) return 0;
    if (!x.valid) return -1;
    if (!y.valid) return 1;
    if (x.neg && !y.neg) return -1;
    if (!x.neg && y.neg) return 1;
    int64_t c = x.unit < y.unit ? -1 : (x.unit > y.unit ? 1 : 0);
    if (x.neg) c = -c;
    return c;
}
std::string DecimalToString(const Decimal& d, int32_t scale) {
    if (!d.valid) return "";
    Decimal r = Rescale(d, scale);
    char buf[48];
    if (scale == 0) {
        sprintf_s(buf, "%s%lld", r.neg ? "-" : "", (long long)r.unit);
    } else {
        int64_t ip = r.unit;
        int64_t fp = 0, p = 1;
        for (int32_t i = 0; i < scale; ++i) p *= 10;
        fp = ip % p; ip /= p;
        // %0*lld：宽度取运行时的 scale，不构造格式串（避免定长缓冲越界）
        sprintf_s(buf, "%s%lld.%0*lld", r.neg ? "-" : "", (long long)ip, (int)scale,
                  (long long)fp);
    }
    return buf;
}
static const char* CurrencySymbol(const std::string& cur) {
    if (cur == "CNY" || cur == "RMB") return "¥";
    if (cur == "USD") return "$";
    if (cur == "EUR") return "€";
    return nullptr;
}
std::string FormatMoney(const Decimal& d, const std::string& currency) {
    if (!d.valid) return "";
    Decimal two = Rescale(d, 2);
    const char* sym = CurrencySymbol(currency);
    std::string code = sym ? "" : currency + " ";
    const char* s = sym ? sym : "";
    // 0 < |v| < 0.01：原值非零但两位显示会截成 0.00 → 显示 "<¥0.01"
    Decimal zero{}; zero.valid = true;
    if (CompareDecimal(d, zero) != 0 && two.unit == 0) {
        return std::string("<") + s + code + "0.01";
    }
    std::string body = DecimalToString(two, 2);
    if (sym) return std::string(s) + body;
    return code + body;
}

// ---------- 编码 ----------
std::string WideToUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), n, nullptr, nullptr);
    return out;
}
std::wstring Utf8ToWide(std::string_view u) {
    if (u.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, u.data(), (int)u.size(), nullptr, 0);
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, u.data(), (int)u.size(), out.data(), n);
    return out;
}

std::string Trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return std::string(s.substr(a, b - a));
}
std::wstring TrimW(std::wstring_view s) {
    // 宽字符版必须逐码点判空白；转 (unsigned char) 只看低 8 位，会把低字节 <=0x20 的
    // CJK 字符（U+4E00..U+4E20 段"一/三/上/下"等）静默剪掉（实测别名首字丢失）。
    auto is_ws = [](wchar_t c) {
        return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r' || c == L'\f' || c == L'\v';
    };
    size_t a = 0, b = s.size();
    while (a < b && is_ws(s[a])) ++a;
    while (b > a && is_ws(s[b - 1])) --b;
    return std::wstring(s.substr(a, b - a));
}
bool StartsWith(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.substr(0, p.size()) == p;
}
bool EndsWithI(std::wstring_view s, std::wstring_view p) {
    if (s.size() < p.size()) return false;
    return _wcsnicmp(s.data() + s.size() - p.size(), p.data(), p.size()) == 0;
}
std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}
std::string I64(int64_t v) {
    char buf[24];
    sprintf_s(buf, "%lld", (long long)v);
    return buf;
}
std::string Fnv1a64Hex(std::string_view s) {
    // 空输入 → 空输出：调用方以"非空"作为"存在可锚定身份"的判据
    if (s.empty()) return {};
    // FNV-1a 64（确定性哈希）。std::hash 每进程加盐（MSVC），不能做跨会话的持久标识。
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    static const char kHex[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[(size_t)i] = kHex[h & 0xF];
        h >>= 4;
    }
    return out;
}

// ---------- 版本资源 ----------
// exe 的 FileVersion 取前三段："1.3.0.0" → "1.3.0"。此前这份读数只在 UI 层（侧栏页脚），
// 而 HTTP 层另靠 APP_VERSION 宏拼请求头——该宏从未被任何构建脚本定义，请求头一直是兜底值
// QuotaBar/1.0.0。现在读数下沉到这里，两处同源。
const std::wstring& ExeVersion() {
    static const std::wstring cached = []() -> std::wstring {
        wchar_t path[MAX_PATH]{};
        const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (!n || n >= MAX_PATH) return {}; // 路径被截断：缓冲区不保证以 0 结尾
        DWORD handle = 0;
        const DWORD sz = GetFileVersionInfoSizeW(path, &handle);
        if (!sz) return {};
        std::vector<BYTE> buf(sz);
        if (!GetFileVersionInfoW(path, 0, sz, buf.data())) return {};
        // 语言/码页表 → 定位 StringFileInfo（quota_bar.rc 里只有 0804 04b0 一组）
        struct LangCodePage { WORD lang, code; };
        LangCodePage* tl = nullptr;
        UINT tln = 0;
        if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&tl, &tln) ||
            !tl || tln < (UINT)sizeof(LangCodePage))
            return {};
        wchar_t sub[80];
        swprintf_s(sub, L"\\StringFileInfo\\%04x%04x\\FileVersion", tl[0].lang, tl[0].code);
        wchar_t* ver = nullptr;
        UINT vn = 0;
        if (!VerQueryValueW(buf.data(), sub, (void**)&ver, &vn) || !ver) return {};
        const std::wstring s(ver);
        std::wstring out;
        size_t pos = 0;
        for (int parts = 0; parts < 3 && pos <= s.size(); ++parts) {
            const size_t dot = s.find(L'.', pos);
            if (parts) out += L'.';
            out += s.substr(pos, dot == std::wstring::npos ? std::wstring::npos : dot - pos);
            if (dot == std::wstring::npos) break;
            pos = dot + 1;
        }
        return out;
    }();
    return cached;
}

} // namespace qb
