#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace spido {

// Minimal JSON value. Supports object/array/string/number/bool/null and
// brace-initialization like Json{{"key", value}, ...} for response bodies.
class Json {
public:
    using Object = std::vector<std::pair<std::string, Json>>;
    using Array  = std::vector<Json>;

    Json() = default;
    Json(std::nullptr_t)            : v_(std::monostate{}) {}
    Json(bool b)                    : v_(b) {}
    Json(int i)                     : v_(static_cast<int64_t>(i)) {}
    Json(long i)                    : v_(static_cast<int64_t>(i)) {}
    Json(long long i)               : v_(static_cast<int64_t>(i)) {}
    Json(unsigned i)                : v_(static_cast<int64_t>(i)) {}
    Json(unsigned long i)           : v_(static_cast<int64_t>(i)) {}
    Json(unsigned long long i)      : v_(static_cast<int64_t>(i)) {}
    Json(double d)                  : v_(d) {}
    Json(const char* s)             : v_(std::string(s)) {}
    Json(std::string s)             : v_(std::move(s)) {}
    Json(std::string_view s)        : v_(std::string(s)) {}
    Json(Array a)                   : v_(std::move(a)) {}
    Json(Object o)                  : v_(std::move(o)) {}

    Json(std::initializer_list<std::pair<const char*, Json>> init) {
        Object o;
        o.reserve(init.size());
        for (auto& [k, val] : init) o.emplace_back(k, val);
        v_ = std::move(o);
    }

    static Json array(std::initializer_list<Json> init) {
        return Json(Array(init.begin(), init.end()));
    }

    std::string dump() const {
        std::string out;
        out.reserve(64);
        write(out);
        return out;
    }

private:
    using Variant = std::variant<std::monostate, bool, int64_t, double,
                                 std::string, Array, Object>;
    Variant v_;

    static void escape(std::string& out, std::string_view s) {
        out.push_back('"');
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                case '\b': out += "\\b";  break;
                case '\f': out += "\\f";  break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out.push_back(c);
                    }
            }
        }
        out.push_back('"');
    }

    void write(std::string& out) const {
        std::visit([&](auto&& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                out += "null";
            } else if constexpr (std::is_same_v<T, bool>) {
                out += x ? "true" : "false";
            } else if constexpr (std::is_same_v<T, int64_t>) {
                out += std::to_string(x);
            } else if constexpr (std::is_same_v<T, double>) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.17g", x);
                out += buf;
            } else if constexpr (std::is_same_v<T, std::string>) {
                escape(out, x);
            } else if constexpr (std::is_same_v<T, Array>) {
                out.push_back('[');
                bool first = true;
                for (auto& el : x) {
                    if (!first) out.push_back(',');
                    first = false;
                    el.write(out);
                }
                out.push_back(']');
            } else if constexpr (std::is_same_v<T, Object>) {
                out.push_back('{');
                bool first = true;
                for (auto& [k, v] : x) {
                    if (!first) out.push_back(',');
                    first = false;
                    escape(out, k);
                    out.push_back(':');
                    v.write(out);
                }
                out.push_back('}');
            }
        }, v_);
    }
};

} // namespace spido
