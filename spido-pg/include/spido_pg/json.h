#pragma once

// Minimal JSON parser used by the generator-emitted handlers. We need just
// enough to walk a flat object body — {"name":"John","age":30} — and pull
// each value out as its canonical text representation, which then becomes
// a TEXT-format PgParam. PG casts to the actual column type on its end.
//
// Not a full RFC 8259 parser: nested objects/arrays are passed through as
// the raw substring (so jsonb columns work — INSERT '... raw object ...'
// :: jsonb succeeds), but the parser does *not* re-validate them. Garbage
// in → bad INSERT, you'll see the error in the PG response. Unicode escape
// handling is limited to \uXXXX BMP (no surrogate pairs).
//
// Header-only so the spido_pg static library stays self-contained.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "spido_pg/string_map.h"

namespace spido_pg {

class JsonBody {
public:
    // Parse a top-level JSON object. Returns empty on malformed input. Each
    // value is stored as its canonical string form:
    //   "John"   →   John          (quotes stripped, escapes decoded)
    //    42      →   42
    //    true    →   true
    //    null    →   <not present in map; lookup returns nullopt>
    //   { ... }  →   { ... }       (raw substring, validated only at outer braces)
    static std::optional<JsonBody> parse(std::string_view body);

    std::optional<std::string_view> get(std::string_view key) const noexcept {
        auto it = fields_.find(key);
        if (it == fields_.end()) return std::nullopt;
        return std::string_view(it->second);
    }

    // True if the key was present and the value was JSON `null`.
    bool is_null(std::string_view key) const noexcept {
        auto it = nulls_.find(key);
        return it != nulls_.end();
    }

    bool empty() const noexcept { return fields_.empty(); }

private:
    StringMap<std::string> fields_;
    StringMap<char>        nulls_; // set-of-keys
};

namespace detail {

inline void skip_ws(std::string_view s, size_t& i) {
    while (i < s.size() && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) ++i;
}

// Parse a quoted JSON string starting at s[i]=='"'. On success advances `i`
// past the closing quote and appends the decoded chars to `out`.
inline bool parse_string(std::string_view s, size_t& i, std::string& out) {
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    while (i < s.size()) {
        char c = s[i++];
        if (c == '"') return true;
        if (c == '\\') {
            if (i >= s.size()) return false;
            char e = s[i++];
            switch (e) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'n':  out.push_back('\n'); break;
                case 't':  out.push_back('\t'); break;
                case 'r':  out.push_back('\r'); break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'u': {
                    // \uXXXX — BMP only. Decode into UTF-8.
                    if (i + 4 > s.size()) return false;
                    uint32_t cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        char h = s[i++];
                        cp <<= 4;
                        if      (h >= '0' && h <= '9') cp |= h - '0';
                        else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                        else return false;
                    }
                    if (cp < 0x80) out.push_back(static_cast<char>(cp));
                    else if (cp < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: return false;
            }
        } else {
            out.push_back(c);
        }
    }
    return false;
}

// Walk a JSON value (any kind) starting at s[i]. Returns the substring of
// s that contains the entire value. Used for arrays/objects/numbers/bools
// where we just need the raw lexical form for downstream PG cast.
inline std::optional<std::string_view> parse_value_raw(std::string_view s, size_t& i) {
    skip_ws(s, i);
    if (i >= s.size()) return std::nullopt;
    size_t start = i;
    char c = s[i];
    if (c == '"') {
        // Skip past the string, honouring \" escapes. Caller dereferences
        // via parse_string for the decoded form when it actually wants it.
        ++i;
        while (i < s.size()) {
            if (s[i] == '\\') { i += 2; continue; }
            if (s[i] == '"')  { ++i; return s.substr(start, i - start); }
            ++i;
        }
        return std::nullopt;
    }
    if (c == '{' || c == '[') {
        char open = c, close = (c == '{' ? '}' : ']');
        int depth = 0;
        while (i < s.size()) {
            char d = s[i++];
            if (d == '"') {
                // Skip a quoted string within the structure.
                while (i < s.size() && s[i] != '"') {
                    if (s[i] == '\\') i += 2; else ++i;
                }
                if (i < s.size()) ++i;
            } else if (d == open) ++depth;
            else if (d == close) { if (--depth == 0) return s.substr(start, i - start); }
        }
        return std::nullopt;
    }
    // Number, true, false, null — read up to the next delimiter.
    while (i < s.size()) {
        char d = s[i];
        if (d == ',' || d == '}' || d == ']' || d==' '||d=='\t'||d=='\n'||d=='\r')
            break;
        ++i;
    }
    if (i == start) return std::nullopt;
    return s.substr(start, i - start);
}

} // namespace detail

inline std::optional<JsonBody> JsonBody::parse(std::string_view body) {
    using namespace detail;
    JsonBody out;
    size_t i = 0;
    skip_ws(body, i);
    if (i >= body.size() || body[i] != '{') return std::nullopt;
    ++i;
    skip_ws(body, i);
    if (i < body.size() && body[i] == '}') return out;

    while (i < body.size()) {
        skip_ws(body, i);
        std::string key;
        if (!parse_string(body, i, key)) return std::nullopt;
        skip_ws(body, i);
        if (i >= body.size() || body[i] != ':') return std::nullopt;
        ++i;
        skip_ws(body, i);

        // Peek to decide string vs raw.
        if (i >= body.size()) return std::nullopt;
        if (body[i] == '"') {
            std::string val;
            if (!parse_string(body, i, val)) return std::nullopt;
            out.fields_.emplace(std::move(key), std::move(val));
        } else {
            auto raw = parse_value_raw(body, i);
            if (!raw) return std::nullopt;
            // JSON literal `null` → record absence, not "null" the string.
            if (*raw == "null") out.nulls_.emplace(std::move(key), '\0');
            else                out.fields_.emplace(std::move(key), std::string(*raw));
        }

        skip_ws(body, i);
        if (i < body.size() && body[i] == ',') { ++i; continue; }
        if (i < body.size() && body[i] == '}') { ++i; return out; }
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace spido_pg
