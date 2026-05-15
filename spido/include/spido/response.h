#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "spido/json.h"

namespace spido {

// Response builds the wire bytes for a single reply. Handlers mutate it;
// the server takes ownership and serializes once the handler returns.
class Response {
public:
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    bool keep_alive = true;

    Response& set_status(int s)      noexcept { status = s; return *this; }
    Response& header(std::string k, std::string v) {
        headers.emplace_back(std::move(k), std::move(v));
        return *this;
    }

    Response& text(std::string_view s) {
        body.assign(s.data(), s.size());
        set_content_type("text/plain; charset=utf-8");
        return *this;
    }

    Response& json(const Json& j) {
        body = j.dump();
        set_content_type("application/json");
        return *this;
    }

    Response& html(std::string_view s) {
        body.assign(s.data(), s.size());
        set_content_type("text/html; charset=utf-8");
        return *this;
    }

    // Serialize headers + body. Reuses dest by appending.
    void serialize(std::string& dest) const;

private:
    void set_content_type(std::string_view ct) {
        for (auto& [k, _] : headers) {
            if (k.size() == 12) { // "Content-Type"
                bool eq = true;
                for (size_t i = 0; i < 12; ++i) {
                    char a = k[i]; if (a >= 'A' && a <= 'Z') a |= 0x20;
                    char b = "content-type"[i];
                    if (a != b) { eq = false; break; }
                }
                if (eq) return;
            }
        }
        headers.emplace_back("Content-Type", std::string(ct));
    }
};

} // namespace spido
