#include "spido/response.h"
#include "spido/request.h"

#include <cstdio>
#include <string_view>

namespace spido {

namespace {
int hex_val(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
} // namespace

std::string Request::decode(std::string_view raw, bool plus_to_space) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        char c = raw[i];
        if (c == '%' && i + 2 < raw.size()) {
            int hi = hex_val(raw[i + 1]);
            int lo = hex_val(raw[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        if (plus_to_space && c == '+') { out.push_back(' '); continue; }
        out.push_back(c);
    }
    return out;
}

namespace {

std::string_view reason_phrase(int s) noexcept {
    switch (s) {
        case 100: return "Continue";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default:  return "OK";
    }
}

} // namespace

void Response::serialize(std::string& dest) const {
    char line[64];
    int n = std::snprintf(line, sizeof(line), "HTTP/1.1 %d ", status);
    dest.append(line, size_t(n));
    auto reason = reason_phrase(status);
    dest.append(reason.data(), reason.size());
    dest.append("\r\n", 2);

    bool has_len = false, has_conn = false;
    for (auto& [k, v] : headers) {
        if (k.size() == 14) { // "Content-Length"
            bool eq = true;
            static constexpr char want[] = "content-length";
            for (size_t i = 0; i < 14; ++i) {
                char a = k[i]; if (a >= 'A' && a <= 'Z') a |= 0x20;
                if (a != want[i]) { eq = false; break; }
            }
            if (eq) has_len = true;
        } else if (k.size() == 10) { // "Connection"
            bool eq = true;
            static constexpr char want[] = "connection";
            for (size_t i = 0; i < 10; ++i) {
                char a = k[i]; if (a >= 'A' && a <= 'Z') a |= 0x20;
                if (a != want[i]) { eq = false; break; }
            }
            if (eq) has_conn = true;
        }
        dest.append(k);
        dest.append(": ", 2);
        dest.append(v);
        dest.append("\r\n", 2);
    }

    if (!has_len) {
        n = std::snprintf(line, sizeof(line), "Content-Length: %zu\r\n", body.size());
        dest.append(line, size_t(n));
    }
    if (!has_conn) {
        dest.append(keep_alive ? "Connection: keep-alive\r\n"
                               : "Connection: close\r\n");
    }

    dest.append("\r\n", 2);
    dest.append(body);
}

} // namespace spido
