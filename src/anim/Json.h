#pragma once

#include <string>
#include <utility>
#include <vector>

namespace mc {

// Minimal JSON value + parser. Supports // line comments (used by Bedrock packs).
struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;

    bool        boolean = false;
    double      number = 0.0;
    std::string string;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object; // insertion order preserved

    bool isObject() const { return type == Type::Object; }
    bool isArray() const { return type == Type::Array; }
    bool isNumber() const { return type == Type::Number; }
    bool isString() const { return type == Type::String; }

    // Object member by key, or nullptr.
    const JsonValue* find(const std::string& key) const;
};

// Both throw std::runtime_error on malformed input / unreadable file.
JsonValue parseJson(const std::string& text);
JsonValue parseJsonFile(const std::string& path);

} // namespace mc
