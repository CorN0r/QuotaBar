#include "json.hpp"
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace qb {

bool JsonValue::AsInt(int64_t& out) const {
    if (type_ != Type::Number) return false;
    errno = 0;
    char* end = nullptr;
    // 有符号解析 + ERANGE 检查：_strtoui64 对溢出静默返回 _UI64_MAX、对负数
    // 实现定义回绕——错误值一旦进了 cred_rev/百分比等字段，凭证隔离与阈值全部失真
    long long v = _strtoi64(raw_num_.c_str(), &end, 10);
    if (end && *end == 0 && errno != ERANGE) { out = v; return true; }
    double d = strtod(raw_num_.c_str(), &end);
    if (end && *end == 0 && d == (double)(long long)d) { out = (int64_t)d; return true; }
    return false;
}
double JsonValue::AsDouble(double def) const {
    if (type_ != Type::Number) return def;
    char* end = nullptr;
    double d = strtod(raw_num_.c_str(), &end);
    if (!end || *end != 0) return def;
    return d;
}
const JsonValue& JsonValue::Find(std::string_view key) const {
    static const JsonValue kNull;
    if (type_ != Type::Object) return kNull;
    for (const auto& kv : obj_) {
        if (kv.first == key) return kv.second;
    }
    return kNull;
}
JsonValue JsonValue::MakeBool(bool b) { JsonValue v; v.type_ = Type::Bool; v.bool_ = b; return v; }
JsonValue JsonValue::MakeNumber(std::string raw) { JsonValue v; v.type_ = Type::Number; v.raw_num_ = std::move(raw); return v; }
JsonValue JsonValue::MakeString(std::string s) { JsonValue v; v.type_ = Type::String; v.str_ = std::move(s); return v; }
JsonValue JsonValue::MakeArray() { JsonValue v; v.type_ = Type::Array; return v; }
JsonValue JsonValue::MakeObject() { JsonValue v; v.type_ = Type::Object; return v; }

namespace {

constexpr int kMaxDepth = 16;
constexpr size_t kMaxStringLen = 32 * 1024;

struct Parser {
    std::string_view s;
    size_t p = 0;
    int depth = 0;
    std::string err;

    bool Fail(const char* msg) {
        if (err.empty()) {
            err = std::string(msg) + " @" + std::to_string(p);
        }
        return false;
    }
    void SkipWs() {
        while (p < s.size()) {
            char c = s[p];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++p;
            else break;
        }
    }
    bool ParseValue(JsonValue& out) {
        if (depth >= kMaxDepth) return Fail("depth limit");
        SkipWs();
        if (p >= s.size()) return Fail("unexpected end");
        char c = s[p];
        switch (c) {
            case '{': return ParseObject(out);
            case '[': return ParseArray(out);
            case '"': {
                std::string str;
                if (!ParseString(str)) return false;
                out = JsonValue::MakeString(std::move(str));
                return true;
            }
            case 't': return ParseLit("true", [&]{ out = JsonValue::MakeBool(true); return true; });
            case 'f': return ParseLit("false", [&]{ out = JsonValue::MakeBool(false); return true; });
            case 'n': return ParseLit("null", [&]{ out = JsonValue(); return true; });
            default: return ParseNumber(out);
        }
    }
    template <typename F>
    bool ParseLit(const char* lit, F&& done) {
        size_t n = strlen(lit);
        if (s.size() - p < n || memcmp(s.data() + p, lit, n) != 0) return Fail("bad literal");
        p += n;
        return done();
    }
    bool ParseObject(JsonValue& out) {
        ++depth;
        out = JsonValue::MakeObject();
        ++p; // {
        SkipWs();
        if (p < s.size() && s[p] == '}') { ++p; --depth; return true; }
        while (true) {
            SkipWs();
            if (p >= s.size() || s[p] != '"') { --depth; return Fail("expected key"); }
            std::string key;
            if (!ParseString(key)) { --depth; return false; }
            SkipWs();
            if (p >= s.size() || s[p] != ':') { --depth; return Fail("expected ':'"); }
            ++p;
            JsonValue val;
            if (!ParseValue(val)) { --depth; return false; }
            out.MutObject().emplace_back(std::move(key), std::move(val));
            SkipWs();
            if (p < s.size() && s[p] == ',') { ++p; continue; }
            if (p < s.size() && s[p] == '}') { ++p; --depth; return true; }
            --depth;
            return Fail("expected ',' or '}'");
        }
    }
    bool ParseArray(JsonValue& out) {
        ++depth;
        out = JsonValue::MakeArray();
        ++p; // [
        SkipWs();
        if (p < s.size() && s[p] == ']') { ++p; --depth; return true; }
        while (true) {
            JsonValue val;
            if (!ParseValue(val)) { --depth; return false; }
            out.MutArray().push_back(std::move(val));
            SkipWs();
            if (p < s.size() && s[p] == ',') { ++p; continue; }
            if (p < s.size() && s[p] == ']') { ++p; --depth; return true; }
            --depth;
            return Fail("expected ',' or ']'");
        }
    }
    bool ParseString(std::string& out) {
        ++p; // opening quote
        out.clear();
        while (true) {
            if (p >= s.size()) return Fail("unterminated string");
            char c = s[p];
            if (c == '"') { ++p; return true; }
            if (c == '\\') {
                ++p;
                if (p >= s.size()) return Fail("bad escape");
                char e = s[p++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        uint32_t cp;
                        if (!ParseHex4(cp)) return false;
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            // 代理对
                            if (p + 1 < s.size() && s[p] == '\\' && s[p + 1] == 'u') {
                                p += 2;
                                uint32_t lo;
                                if (!ParseHex4(lo)) return false;
                                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                } else {
                                    return Fail("bad surrogate");
                                }
                            } else {
                                return Fail("lone surrogate");
                            }
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            return Fail("lone surrogate");
                        }
                        AppendUtf8(out, cp);
                        break;
                    }
                    default: return Fail("bad escape");
                }
                if (out.size() > kMaxStringLen) return Fail("string too long");
            } else if ((unsigned char)c < 0x20) {
                return Fail("control char in string");
            } else {
                out += c;
                ++p;
                if (out.size() > kMaxStringLen) return Fail("string too long");
            }
        }
    }
    bool ParseHex4(uint32_t& out) {
        if (p + 4 > s.size()) return Fail("bad \\u");
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s[p + i];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (c - '0');
            else if (c >= 'a' && c <= 'f') v |= (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (c - 'A' + 10);
            else return Fail("bad \\u hex");
        }
        p += 4;
        out = v;
        return true;
    }
    static void AppendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }
    bool ParseNumber(JsonValue& out) {
        size_t start = p;
        if (p < s.size() && s[p] == '-') ++p;
        // 整数部分
        if (p >= s.size() || s[p] < '0' || s[p] > '9') return Fail("bad number");
        if (s[p] == '0') {
            ++p;
        } else {
            while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
        }
        // 小数部分
        if (p < s.size() && s[p] == '.') {
            ++p;
            if (p >= s.size() || s[p] < '0' || s[p] > '9') return Fail("bad fraction");
            while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
        }
        // 指数
        if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
            ++p;
            if (p < s.size() && (s[p] == '+' || s[p] == '-')) ++p;
            if (p >= s.size() || s[p] < '0' || s[p] > '9') return Fail("bad exponent");
            while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
        }
        std::string raw(s.substr(start, p - start));
        // 防御极端数值长度
        if (raw.size() > 64) return Fail("number too long");
        out = JsonValue::MakeNumber(std::move(raw));
        return true;
    }
};

void DumpString(const std::string& str, std::string& out) {
    out += '"';
    for (unsigned char c : str) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    sprintf_s(buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    out += '"';
}

void DumpValue(const JsonValue& v, std::string& out) {
    switch (v.type()) {
        case JsonValue::Type::Null: out += "null"; break;
        case JsonValue::Type::Bool: out += v.AsBool() ? "true" : "false"; break;
        case JsonValue::Type::Number: out += v.RawNumber(); break;
        case JsonValue::Type::String: DumpString(v.AsString(), out); break;
        case JsonValue::Type::Array: {
            out += '[';
            bool first = true;
            for (const auto& item : v.AsArray()) {
                if (!first) out += ',';
                first = false;
                DumpValue(item, out);
            }
            out += ']';
            break;
        }
        case JsonValue::Type::Object: {
            out += '{';
            bool first = true;
            for (const auto& kv : v.AsObject()) {
                if (!first) out += ',';
                first = false;
                DumpString(kv.first, out);
                out += ':';
                DumpValue(kv.second, out);
            }
            out += '}';
            break;
        }
    }
}

} // namespace

std::unique_ptr<JsonValue> JsonParse(std::string_view text, std::string* err) {
    Parser ps;
    ps.s = text;
    JsonValue root;
    if (!ps.ParseValue(root)) {
        if (err) *err = ps.err;
        return nullptr;
    }
    ps.SkipWs();
    if (ps.p != ps.s.size()) {
        if (err) *err = "trailing garbage @" + std::to_string(ps.p);
        return nullptr;
    }
    auto result = std::make_unique<JsonValue>(std::move(root));
    if (err) err->clear();
    return result;
}

std::string JsonDump(const JsonValue& v) {
    std::string out;
    DumpValue(v, out);
    return out;
}

std::string JsonQuote(const std::string& s) {
    std::string out;
    DumpString(s, out);
    return out;
}

} // namespace qb
