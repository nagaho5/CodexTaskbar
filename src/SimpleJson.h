#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    JsonValue() = default;

    static JsonValue MakeNull();
    static JsonValue MakeBool(bool value);
    static JsonValue MakeNumber(double value);
    static JsonValue MakeString(std::string value);
    static JsonValue MakeArray(std::vector<JsonValue> value);
    static JsonValue MakeObject(std::map<std::string, JsonValue, std::less<>> value);

    Type type() const { return type_; }
    bool IsNull() const { return type_ == Type::Null; }
    bool IsBool() const { return type_ == Type::Bool; }
    bool IsNumber() const { return type_ == Type::Number; }
    bool IsString() const { return type_ == Type::String; }
    bool IsArray() const { return type_ == Type::Array; }
    bool IsObject() const { return type_ == Type::Object; }

    bool AsBool(bool fallback = false) const;
    double AsNumber(double fallback = 0.0) const;
    std::int64_t AsInt64(std::int64_t fallback = 0) const;
    const std::string& AsString() const;
    const std::vector<JsonValue>& AsArray() const;
    std::vector<JsonValue>& AsArray();
    const std::map<std::string, JsonValue, std::less<>>& AsObject() const;
    std::map<std::string, JsonValue, std::less<>>& AsObject();

    const JsonValue* Find(std::string_view key) const;
    JsonValue* Find(std::string_view key);

    static std::optional<JsonValue> Parse(std::string_view text);

private:
    Type type_{Type::Null};
    bool boolValue_{false};
    double numberValue_{0.0};
    std::string stringValue_;
    std::vector<JsonValue> arrayValue_;
    std::map<std::string, JsonValue, std::less<>> objectValue_;
};

bool ApplyJsonPatch(JsonValue& document, const JsonValue& patch);
