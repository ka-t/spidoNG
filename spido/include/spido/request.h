#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace spido {

enum class Method : uint8_t {
    Unknown = 0,
    GET, POST, PUT, DELETE, HEAD, OPTIONS, PATCH,
};

struct Header {
    std::string_view name;
    std::string_view value;
};

// Request holds views into a parser-owned buffer. Lifetime ends when the
// handler returns; copy any data you need to keep.
struct Request {
    Method            method = Method::Unknown;
    std::string_view  path;       // raw, %xx not decoded
    std::string_view  query;      // raw, %xx not decoded, '+' not space
    std::string_view  version;
    std::vector<Header> headers;
    std::string_view  body;

    // Path parameters bound by the router (e.g. /users/:id -> {"id", "42"}).
    // Views point into the request path buffer.
    std::vector<std::pair<std::string_view, std::string_view>> params;

    bool keep_alive = true;

    std::string_view header(std::string_view name) const noexcept {
        for (auto& h : headers) {
            if (h.name.size() == name.size()) {
                bool eq = true;
                for (size_t i = 0; i < name.size(); ++i) {
                    char a = h.name[i], b = name[i];
                    if (a >= 'A' && a <= 'Z') a |= 0x20;
                    if (b >= 'A' && b <= 'Z') b |= 0x20;
                    if (a != b) { eq = false; break; }
                }
                if (eq) return h.value;
            }
        }
        return {};
    }

    std::string_view param(std::string_view name) const noexcept {
        for (auto& [k, v] : params) if (k == name) return v;
        return {};
    }

    // Lazy percent-decode helpers. Each is computed on first call and cached
    // in the provided storage; pass the same string& across calls in a
    // request lifetime if you need the decoded value more than once.
    static std::string decode(std::string_view raw, bool plus_to_space = false);
    std::string decoded_path()  const { return decode(path,  false); }
    std::string decoded_query() const { return decode(query, true);  }
};

} // namespace spido
