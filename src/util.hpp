// util.hpp — 基础工具：时间(UTC/ISO8601/单调钟)、十进制金额、编码、抖动。
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace qb {

// ---------- 时间 ----------
// UTC 秒（Unix 纪元）。调度与持久化使用；展示时转本地时区。
int64_t NowUnix();
// 单调毫秒（QPC）。仅当前进程内用于数据年龄/调度。
int64_t NowMono();
// Unix 秒 → FILETIME
FILETIME UnixToFileTime(int64_t unix_sec);
int64_t FileTimeToUnix(const FILETIME& ft);
// 本地系统时间被大幅调整时（如 NTP 校正）返回 true（用于把缓存年龄按未知处理）
bool SystemTimeJumped(int64_t prev_unix);

// ISO 8601 解析：支持 Z / ±hh:mm / ±hhmm / 秒后小数（截断到秒）。
// 非法返回 nullopt（调用方保留额度但重置时间显示未知，不使整个请求失败）。
std::optional<int64_t> ParseIso8601Utc(std::string_view s);
// RFC 7231 HTTP-date 解析（Retry-After 两种表达之一）
std::optional<int64_t> ParseHttpDate(std::string_view s);
// Unix → "YYYY-MM-DD HH:MM"(本地) / "YYYY-MM-DDTHH:MM:SSZ"(UTC)
std::string FormatLocalShort(int64_t unix_sec);
std::string FormatIso8601Utc(int64_t unix_sec);
// 剩余时间中文倒计时:minute_precision=true → "3小时25分"(分粒度);
// false → "3天6小时"(小时粒度)。零分量裁剪("2小时"而非"2小时0分");
// 低于粒度 → "不足1分"/"不足1小时";负值按 0 处理。
std::string FormatRemainingZh(int64_t seconds_left, bool minute_precision);
// 构造 UTC 秒（按 UTC 解释各分量；selftest 夹具用）
int64_t MakeUnixForTest(int y, int m, int d, int h, int mi, int s);

// ---------- 十进制金额 ----------
// 定点十进制：unit = 整数部分，scale = 小数位数（0..8），neg = 负号。
// 不用二进制浮点做金额运算；只做展示与阈值比较。
struct Decimal {
    int64_t unit = 0;   // 绝对值的整型（按 scale）
    int32_t scale = 0;
    bool neg = false;
    bool valid = false;
};
// 解析十进制字符串（允许前导 +/-、小数点；位数超限或非法 → invalid）。
Decimal ParseDecimal(std::string_view s, int32_t max_scale = 8);
// 归一化到指定 scale（补零/截断向零），用于比较与展示。
Decimal Rescale(const Decimal& d, int32_t scale);
int32_t MaxScale(const Decimal& a, const Decimal& b);
int64_t CompareDecimal(const Decimal& a, const Decimal& b); // -1/0/1
std::string DecimalToString(const Decimal& d, int32_t scale); // 定点字符串，含负号
// 金额展示：两位小数；0 < |v| < 0.01 显示 "<¥0.01" 形式。
// currency: CNY→¥ USD→$ 其他→"CODE "
std::string FormatMoney(const Decimal& d, const std::string& currency);

// ---------- 编码 ----------
std::string WideToUtf8(std::wstring_view w);
std::wstring Utf8ToWide(std::string_view u);

// ---------- 其他 ----------
std::string Trim(std::string_view s);
std::wstring TrimW(std::wstring_view s);
bool StartsWith(std::string_view s, std::string_view p);
bool EndsWithI(std::wstring_view s, std::wstring_view p);
std::string ToLower(std::string s);
// 0..max_pct% 正向抖动（秒）
int64_t JitterSeconds(int64_t base_sec, int max_pct);
// [lo, hi] 随机整数
int64_t RandRange(int64_t lo, int64_t hi);
void SeedRng();
// 简单数字转字符串
std::string I64(int64_t v);
// FNV-1a 64 → 16 位小写十六进制。确定性哈希（std::hash 每进程加盐，不能做跨会话
// 的持久标识）；空输入 → 空输出（调用方以"非空"为"存在可锚定身份"的判据）。
std::string Fnv1a64Hex(std::string_view s);

// ---------- 版本资源 ----------
// exe 版本资源 FileVersion 的前三段（"1.3.0"）。assets/quota_bar.rc 是版本号的唯一来源：
// 侧栏页脚与 HTTP 请求头 User-Agent 都读这里,代码里不要再写版本字面量。
// 读不到（exe 无版本资源）→ 空串,兜底文案由调用方决定。进程内缓存。
const std::wstring& ExeVersion();

} // namespace qb
