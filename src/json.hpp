// json.hpp — 小型严格 JSON 解析器（无依赖、可审计）。
// 限制：嵌套深度 ≤16；字符串 ≤32KiB；数值严格校验（拒绝前导零/悬挂小数点等）。
// 输入大小上限由调用方（http_client 64KiB）保证。
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace qb {

class JsonValue;
using JsonMember = std::pair<std::string, JsonValue>;
using JsonArray = std::vector<JsonValue>;
using JsonObject = std::vector<JsonMember>;

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    JsonValue() : type_(Type::Null) {}

    Type type() const { return type_; }
    bool IsNull() const { return type_ == Type::Null; }
    bool IsBool() const { return type_ == Type::Bool; }
    bool IsNumber() const { return type_ == Type::Number; }
    bool IsString() const { return type_ == Type::String; }
    bool IsArray() const { return type_ == Type::Array; }
    bool IsObject() const { return type_ == Type::Object; }

    bool AsBool(bool def = false) const { return type_ == Type::Bool ? bool_ : def; }
    // 数值：优先 int64，否则 double
    bool AsInt(int64_t& out) const;
    // int 便捷重载（内部字段多位 int）
    bool AsInt(int& out) const {
        int64_t v = 0;
        if (!AsInt(v)) return false;
        out = (int)v;
        return true;
    }
    double AsDouble(double def = 0.0) const;
    // 原始数值文本（用于十进制金额，避免二进制浮点）
    const std::string& RawNumber() const { return raw_num_; }
    const std::string& AsString() const { return str_; }
    const JsonArray& AsArray() const { return arr_; }
    const JsonObject& AsObject() const { return obj_; }

    // 对象成员查找（不存在或类型不符返回 null Value）
    const JsonValue& Find(std::string_view key) const;

    static JsonValue MakeBool(bool b);
    static JsonValue MakeNumber(std::string raw);
    static JsonValue MakeString(std::string s);
    static JsonValue MakeArray();
    static JsonValue MakeObject();

    JsonArray& MutArray() { return arr_; }
    JsonObject& MutObject() { return obj_; }

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    std::string raw_num_;
    std::string str_;
    JsonArray arr_;
    JsonObject obj_;
};

// 解析失败返回 nullptr；成功返回根对象。err 输出简短原因（可含位置）。
std::unique_ptr<JsonValue> JsonParse(std::string_view text, std::string* err = nullptr);

// 构建辅助：序列化（紧凑）
std::string JsonDump(const JsonValue& v);

// 序列化单个字符串（含引号；转义与 JsonDump 同口径，覆盖全部 <0x20 控制字符）。
// 手写 JSON 的落盘方（config/secrets/cache/history）必须走这里，不得自带局部转义：
// 局部版漏掉 \t/\r 等会写出非法 JSON，下次启动解析失败即整体回退默认配置。
std::string JsonQuote(const std::string& s);

} // namespace qb
