#include "SimpleJson.h"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <utility>

namespace {

const std::string kEmptyString;
const std::vector<JsonValue> kEmptyArray;
const std::map<std::string, JsonValue, std::less<>> kEmptyObject;

void AppendUtf8(std::string& out, std::uint32_t codepoint) {
    if (codepoint <= 0x7F) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

int HexDigit(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
    if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
    return -1;
}

class Parser {
public:
    explicit Parser(std::string_view input) : input_(input) {}

    std::optional<JsonValue> ParseDocument() {
        SkipWhitespace();
        auto value = ParseValue(0);
        if (!value) return std::nullopt;
        SkipWhitespace();
        if (pos_ != input_.size()) return std::nullopt;
        return value;
    }

private:
    static constexpr int kMaxDepth = 128;

    std::optional<JsonValue> ParseValue(int depth) {
        if (depth > kMaxDepth || pos_ >= input_.size()) return std::nullopt;

        switch (input_[pos_]) {
        case 'n':
            return ConsumeLiteral("null") ? std::optional<JsonValue>(JsonValue::MakeNull()) : std::nullopt;
        case 't':
            return ConsumeLiteral("true") ? std::optional<JsonValue>(JsonValue::MakeBool(true)) : std::nullopt;
        case 'f':
            return ConsumeLiteral("false") ? std::optional<JsonValue>(JsonValue::MakeBool(false)) : std::nullopt;
        case '"': {
            auto value = ParseString();
            return value ? std::optional<JsonValue>(JsonValue::MakeString(std::move(*value))) : std::nullopt;
        }
        case '[':
            return ParseArray(depth + 1);
        case '{':
            return ParseObject(depth + 1);
        default:
            if (input_[pos_] == '-' || (input_[pos_] >= '0' && input_[pos_] <= '9')) {
                return ParseNumber();
            }
            return std::nullopt;
        }
    }

    std::optional<JsonValue> ParseArray(int depth) {
        if (input_[pos_] != '[') return std::nullopt;
        ++pos_;
        SkipWhitespace();

        std::vector<JsonValue> values;
        if (pos_ < input_.size() && input_[pos_] == ']') {
            ++pos_;
            return JsonValue::MakeArray(std::move(values));
        }

        while (pos_ < input_.size()) {
            SkipWhitespace();
            auto value = ParseValue(depth);
            if (!value) return std::nullopt;
            values.push_back(std::move(*value));
            SkipWhitespace();

            if (pos_ >= input_.size()) return std::nullopt;
            if (input_[pos_] == ']') {
                ++pos_;
                return JsonValue::MakeArray(std::move(values));
            }
            if (input_[pos_] != ',') return std::nullopt;
            ++pos_;
        }
        return std::nullopt;
    }

    std::optional<JsonValue> ParseObject(int depth) {
        if (input_[pos_] != '{') return std::nullopt;
        ++pos_;
        SkipWhitespace();

        std::map<std::string, JsonValue, std::less<>> values;
        if (pos_ < input_.size() && input_[pos_] == '}') {
            ++pos_;
            return JsonValue::MakeObject(std::move(values));
        }

        while (pos_ < input_.size()) {
            SkipWhitespace();
            auto key = ParseString();
            if (!key) return std::nullopt;
            SkipWhitespace();
            if (pos_ >= input_.size() || input_[pos_] != ':') return std::nullopt;
            ++pos_;
            SkipWhitespace();

            auto value = ParseValue(depth);
            if (!value) return std::nullopt;
            values.insert_or_assign(std::move(*key), std::move(*value));

            SkipWhitespace();
            if (pos_ >= input_.size()) return std::nullopt;
            if (input_[pos_] == '}') {
                ++pos_;
                return JsonValue::MakeObject(std::move(values));
            }
            if (input_[pos_] != ',') return std::nullopt;
            ++pos_;
        }
        return std::nullopt;
    }

    std::optional<std::string> ParseString() {
        if (pos_ >= input_.size() || input_[pos_] != '"') return std::nullopt;
        ++pos_;

        std::string result;
        while (pos_ < input_.size()) {
            const char ch = input_[pos_++];
            if (ch == '"') return result;
            if (static_cast<unsigned char>(ch) < 0x20) return std::nullopt;
            if (ch != '\\') {
                result.push_back(ch);
                continue;
            }

            if (pos_ >= input_.size()) return std::nullopt;
            const char escaped = input_[pos_++];
            switch (escaped) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                auto first = ParseHexCodeUnit();
                if (!first) return std::nullopt;
                std::uint32_t codepoint = *first;
                if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                    if (pos_ + 1 >= input_.size() || input_[pos_] != '\\' || input_[pos_ + 1] != 'u') {
                        return std::nullopt;
                    }
                    pos_ += 2;
                    auto second = ParseHexCodeUnit();
                    if (!second || *second < 0xDC00 || *second > 0xDFFF) return std::nullopt;
                    codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (*second - 0xDC00);
                } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
                    return std::nullopt;
                }
                AppendUtf8(result, codepoint);
                break;
            }
            default:
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<std::uint32_t> ParseHexCodeUnit() {
        if (pos_ + 4 > input_.size()) return std::nullopt;
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const int digit = HexDigit(input_[pos_++]);
            if (digit < 0) return std::nullopt;
            value = (value << 4) | static_cast<std::uint32_t>(digit);
        }
        return value;
    }

    std::optional<JsonValue> ParseNumber() {
        const std::size_t start = pos_;
        if (input_[pos_] == '-') ++pos_;

        if (pos_ >= input_.size()) return std::nullopt;
        if (input_[pos_] == '0') {
            ++pos_;
        } else {
            if (input_[pos_] < '1' || input_[pos_] > '9') return std::nullopt;
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
        }

        if (pos_ < input_.size() && input_[pos_] == '.') {
            ++pos_;
            if (pos_ >= input_.size() || input_[pos_] < '0' || input_[pos_] > '9') return std::nullopt;
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
        }

        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) ++pos_;
            if (pos_ >= input_.size() || input_[pos_] < '0' || input_[pos_] > '9') return std::nullopt;
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') ++pos_;
        }

        const std::string number(input_.substr(start, pos_ - start));
        char* end = nullptr;
        const double value = std::strtod(number.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(value)) return std::nullopt;
        return JsonValue::MakeNumber(value);
    }

    bool ConsumeLiteral(std::string_view literal) {
        if (input_.substr(pos_, literal.size()) != literal) return false;
        pos_ += literal.size();
        return true;
    }

    void SkipWhitespace() {
        while (pos_ < input_.size()) {
            const char ch = input_[pos_];
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') break;
            ++pos_;
        }
    }

    std::string_view input_;
    std::size_t pos_{0};
};

bool ParseArrayIndex(const JsonValue& token, std::size_t& index, bool allowEnd, std::size_t size) {
    if (token.IsNumber()) {
        const auto raw = token.AsInt64(-1);
        if (raw < 0) return false;
        index = static_cast<std::size_t>(raw);
    } else if (token.IsString()) {
        const auto& text = token.AsString();
        if (text == "-" && allowEnd) {
            index = size;
            return true;
        }
        if (text.empty()) return false;
        std::size_t parsed = 0;
        for (char ch : text) {
            if (ch < '0' || ch > '9') return false;
            parsed = parsed * 10 + static_cast<std::size_t>(ch - '0');
        }
        index = parsed;
    } else {
        return false;
    }
    return allowEnd ? index <= size : index < size;
}

JsonValue* ResolveParent(JsonValue& document, const std::vector<JsonValue>& path) {
    JsonValue* current = &document;
    if (path.empty()) return current;

    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        const JsonValue& token = path[i];
        if (current->IsObject() && token.IsString()) {
            current = current->Find(token.AsString());
            if (!current) return nullptr;
        } else if (current->IsArray()) {
            std::size_t index = 0;
            if (!ParseArrayIndex(token, index, false, current->AsArray().size())) return nullptr;
            current = &current->AsArray()[index];
        } else {
            return nullptr;
        }
    }
    return current;
}

bool ApplyOne(JsonValue& document, const JsonValue& operation) {
    if (!operation.IsObject()) return false;
    const JsonValue* opValue = operation.Find("op");
    const JsonValue* pathValue = operation.Find("path");
    if (!opValue || !opValue->IsString() || !pathValue || !pathValue->IsArray()) return false;

    const std::string& op = opValue->AsString();
    const auto& path = pathValue->AsArray();

    if (path.empty()) {
        if (op == "remove") {
            document = JsonValue::MakeNull();
            return true;
        }
        const JsonValue* value = operation.Find("value");
        if ((op == "add" || op == "replace") && value) {
            document = *value;
            return true;
        }
        return false;
    }

    JsonValue* parent = ResolveParent(document, path);
    if (!parent) return false;
    const JsonValue& leaf = path.back();

    if (parent->IsObject() && leaf.IsString()) {
        auto& object = parent->AsObject();
        const std::string& key = leaf.AsString();
        if (op == "remove") {
            object.erase(key);
            return true;
        }
        const JsonValue* value = operation.Find("value");
        if ((op == "add" || op == "replace") && value) {
            object.insert_or_assign(key, *value);
            return true;
        }
        return false;
    }

    if (parent->IsArray()) {
        auto& array = parent->AsArray();
        std::size_t index = 0;
        if (op == "remove") {
            if (!ParseArrayIndex(leaf, index, false, array.size())) return false;
            array.erase(array.begin() + static_cast<std::ptrdiff_t>(index));
            return true;
        }

        const JsonValue* value = operation.Find("value");
        if (!value) return false;
        if (op == "add") {
            if (!ParseArrayIndex(leaf, index, true, array.size())) return false;
            array.insert(array.begin() + static_cast<std::ptrdiff_t>(index), *value);
            return true;
        }
        if (op == "replace") {
            if (!ParseArrayIndex(leaf, index, false, array.size())) return false;
            array[index] = *value;
            return true;
        }
    }

    return false;
}

} // namespace

JsonValue JsonValue::MakeNull() {
    return JsonValue();
}

JsonValue JsonValue::MakeBool(bool value) {
    JsonValue result;
    result.type_ = Type::Bool;
    result.boolValue_ = value;
    return result;
}

JsonValue JsonValue::MakeNumber(double value) {
    JsonValue result;
    result.type_ = Type::Number;
    result.numberValue_ = value;
    return result;
}

JsonValue JsonValue::MakeString(std::string value) {
    JsonValue result;
    result.type_ = Type::String;
    result.stringValue_ = std::move(value);
    return result;
}

JsonValue JsonValue::MakeArray(std::vector<JsonValue> value) {
    JsonValue result;
    result.type_ = Type::Array;
    result.arrayValue_ = std::move(value);
    return result;
}

JsonValue JsonValue::MakeObject(std::map<std::string, JsonValue, std::less<>> value) {
    JsonValue result;
    result.type_ = Type::Object;
    result.objectValue_ = std::move(value);
    return result;
}

bool JsonValue::AsBool(bool fallback) const {
    return IsBool() ? boolValue_ : fallback;
}

double JsonValue::AsNumber(double fallback) const {
    return IsNumber() ? numberValue_ : fallback;
}

std::int64_t JsonValue::AsInt64(std::int64_t fallback) const {
    if (!IsNumber()) return fallback;
    if (numberValue_ < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
        numberValue_ > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return fallback;
    }
    return static_cast<std::int64_t>(numberValue_);
}

const std::string& JsonValue::AsString() const {
    return IsString() ? stringValue_ : kEmptyString;
}

const std::vector<JsonValue>& JsonValue::AsArray() const {
    return IsArray() ? arrayValue_ : kEmptyArray;
}

std::vector<JsonValue>& JsonValue::AsArray() {
    if (!IsArray()) {
        type_ = Type::Array;
        arrayValue_.clear();
        objectValue_.clear();
        stringValue_.clear();
    }
    return arrayValue_;
}

const std::map<std::string, JsonValue, std::less<>>& JsonValue::AsObject() const {
    return IsObject() ? objectValue_ : kEmptyObject;
}

std::map<std::string, JsonValue, std::less<>>& JsonValue::AsObject() {
    if (!IsObject()) {
        type_ = Type::Object;
        objectValue_.clear();
        arrayValue_.clear();
        stringValue_.clear();
    }
    return objectValue_;
}

const JsonValue* JsonValue::Find(std::string_view key) const {
    if (!IsObject()) return nullptr;
    const auto it = objectValue_.find(key);
    return it == objectValue_.end() ? nullptr : &it->second;
}

JsonValue* JsonValue::Find(std::string_view key) {
    if (!IsObject()) return nullptr;
    const auto it = objectValue_.find(key);
    return it == objectValue_.end() ? nullptr : &it->second;
}

std::optional<JsonValue> JsonValue::Parse(std::string_view text) {
    return Parser(text).ParseDocument();
}

bool ApplyJsonPatch(JsonValue& document, const JsonValue& patch) {
    if (!patch.IsArray()) return false;
    for (const JsonValue& operation : patch.AsArray()) {
        if (!ApplyOne(document, operation)) return false;
    }
    return true;
}
