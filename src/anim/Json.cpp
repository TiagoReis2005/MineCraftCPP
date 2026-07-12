#include "anim/Json.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace mc {
namespace {

class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}

    JsonValue parse() {
        JsonValue v = parseValue();
        skipWs();
        if (pos_ != s_.size()) fail("trailing characters");
        return v;
    }

private:
    const std::string& s_;
    size_t pos_ = 0;

    [[noreturn]] void fail(const char* why) {
        throw std::runtime_error("JSON parse error at offset " + std::to_string(pos_) + ": " + why);
    }

    void skipWs() {
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else if (c == '/' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '/') {
                while (pos_ < s_.size() && s_[pos_] != '\n') ++pos_;
            } else {
                break;
            }
        }
    }

    char peek() {
        if (pos_ >= s_.size()) fail("unexpected end of input");
        return s_[pos_];
    }

    void expect(char c) {
        if (peek() != c) fail("unexpected character");
        ++pos_;
    }

    JsonValue parseValue() {
        skipWs();
        char c = peek();
        switch (c) {
            case '{': return parseObject();
            case '[': return parseArray();
            case '"': { JsonValue v; v.type = JsonValue::Type::String; v.string = parseString(); return v; }
            case 't': case 'f': return parseBool();
            case 'n': parseLiteral("null"); return JsonValue{};
            default: return parseNumber();
        }
    }

    void parseLiteral(const char* lit) {
        for (const char* p = lit; *p; ++p) {
            if (pos_ >= s_.size() || s_[pos_] != *p) fail("bad literal");
            ++pos_;
        }
    }

    JsonValue parseBool() {
        JsonValue v;
        v.type = JsonValue::Type::Bool;
        if (peek() == 't') { parseLiteral("true"); v.boolean = true; }
        else { parseLiteral("false"); v.boolean = false; }
        return v;
    }

    JsonValue parseNumber() {
        size_t start = pos_;
        if (peek() == '-') ++pos_;
        while (pos_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[pos_])) ||
                                    s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E' ||
                                    s_[pos_] == '+' || s_[pos_] == '-')) {
            ++pos_;
        }
        if (pos_ == start) fail("expected number");
        JsonValue v;
        v.type = JsonValue::Type::Number;
        v.number = std::strtod(s_.c_str() + start, nullptr);
        return v;
    }

    std::string parseString() {
        expect('"');
        std::string out;
        while (true) {
            if (pos_ >= s_.size()) fail("unterminated string");
            char c = s_[pos_++];
            if (c == '"') break;
            if (c == '\\') {
                if (pos_ >= s_.size()) fail("bad escape");
                char e = s_[pos_++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': // keep raw \uXXXX unescaped (not needed for animation files)
                        out += "\\u";
                        break;
                    default: fail("bad escape");
                }
            } else {
                out += c;
            }
        }
        return out;
    }

    JsonValue parseObject() {
        expect('{');
        JsonValue v;
        v.type = JsonValue::Type::Object;
        skipWs();
        if (peek() == '}') { ++pos_; return v; }
        while (true) {
            skipWs();
            std::string key = parseString();
            skipWs();
            expect(':');
            v.object.emplace_back(std::move(key), parseValue());
            skipWs();
            if (peek() == ',') { ++pos_; continue; }
            expect('}');
            break;
        }
        return v;
    }

    JsonValue parseArray() {
        expect('[');
        JsonValue v;
        v.type = JsonValue::Type::Array;
        skipWs();
        if (peek() == ']') { ++pos_; return v; }
        while (true) {
            v.array.push_back(parseValue());
            skipWs();
            if (peek() == ',') { ++pos_; continue; }
            expect(']');
            break;
        }
        return v;
    }
};

} // namespace

const JsonValue* JsonValue::find(const std::string& key) const {
    for (const auto& [k, v] : object) {
        if (k == key) return &v;
    }
    return nullptr;
}

JsonValue parseJson(const std::string& text) {
    return Parser(text).parse();
}

JsonValue parseJsonFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("Failed to open " + path);
    std::ostringstream ss;
    ss << file.rdbuf();
    return parseJson(ss.str());
}

} // namespace mc
