// Minimal in-house JSON parser for Litho GUI theme files.
// Supports objects, arrays, strings (with escapes, incl. \u), numbers,
// booleans, and null. No third-party code. Parsing is deliberately
// forgiving about nothing: a malformed document fails with an error
// string, and the theme loader falls back to defaults on failure.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace lithogui {

// A JSON value. Objects keep insertion order (vector of pairs); nothing
// in the theme loader iterates a hash map. Duplicate object keys are
// FIRST-WINS: find() returns the first occurrence, and the theme
// loader (which reads through find) therefore sees the first value
// for a repeated key (review SUP-10).
struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string str;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;

    const JsonValue* find(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        for (auto& kv : object) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    bool isObject() const { return type == Type::Object; }
    bool isNumber() const { return type == Type::Number; }
    bool isString() const { return type == Type::String; }
    bool isArray() const { return type == Type::Array; }
};

// Parses a complete document. Returns false with `error` set on any
// malformed input (including trailing garbage and excess nesting).
bool parseJson(const std::string& text, JsonValue& out, std::string& error);

} // namespace lithogui
