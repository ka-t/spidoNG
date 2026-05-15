#include "server/http_parser.h"

#include <array>
#include <cstring>
#include <limits>
#include <string_view>

namespace spido {

namespace {

constexpr auto make_token_table() noexcept {
    std::array<unsigned char, 256> t{};

    for (unsigned c = 'a'; c <= 'z'; ++c) t[c] = 1;
    for (unsigned c = 'A'; c <= 'Z'; ++c) t[c] = 1;
    for (unsigned c = '0'; c <= '9'; ++c) t[c] = 1;

    t[static_cast<unsigned>('!')]  = 1;
    t[static_cast<unsigned>('#')]  = 1;
    t[static_cast<unsigned>('$')]  = 1;
    t[static_cast<unsigned>('%')]  = 1;
    t[static_cast<unsigned>('&')]  = 1;
    t[static_cast<unsigned>('\'')] = 1;
    t[static_cast<unsigned>('*')]  = 1;
    t[static_cast<unsigned>('+')]  = 1;
    t[static_cast<unsigned>('-')]  = 1;
    t[static_cast<unsigned>('.')]  = 1;
    t[static_cast<unsigned>('^')]  = 1;
    t[static_cast<unsigned>('_')]  = 1;
    t[static_cast<unsigned>('`')]  = 1;
    t[static_cast<unsigned>('|')]  = 1;
    t[static_cast<unsigned>('~')]  = 1;

    return t;
}

static constexpr auto kTokenTable = make_token_table();

inline bool is_token(char c) noexcept {
    return kTokenTable[static_cast<unsigned char>(c)] != 0;
}

inline char to_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? char(c | 0x20) : c;
}

inline bool eq_lit(std::string_view a, const char* lit, size_t n) noexcept {
    return a.size() == n && std::memcmp(a.data(), lit, n) == 0;
}

inline bool ieq_lit(std::string_view a, const char* lit_lower, size_t n) noexcept {
    if (a.size() != n) return false;

    for (size_t i = 0; i < n; ++i) {
        if (to_lower(a[i]) != lit_lower[i]) return false;
    }

    return true;
}

inline bool ieq(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;

    for (size_t i = 0; i < a.size(); ++i) {
        if (to_lower(a[i]) != to_lower(b[i])) return false;
    }

    return true;
}

Method method_from(std::string_view s) noexcept {
    switch (s.size()) {
        case 3:
            if (std::memcmp(s.data(), "GET", 3) == 0) return Method::GET;
            if (std::memcmp(s.data(), "PUT", 3) == 0) return Method::PUT;
            break;

        case 4:
            if (std::memcmp(s.data(), "POST", 4) == 0) return Method::POST;
            if (std::memcmp(s.data(), "HEAD", 4) == 0) return Method::HEAD;
            break;

        case 5:
            if (std::memcmp(s.data(), "PATCH", 5) == 0) return Method::PATCH;
            break;

        case 6:
            if (std::memcmp(s.data(), "DELETE", 6) == 0) return Method::DELETE;
            break;

        case 7:
            if (std::memcmp(s.data(), "OPTIONS", 7) == 0) return Method::OPTIONS;
            break;
    }

    return Method::Unknown;
}

enum class HeaderKind : unsigned char {
    Other,
    ContentLength,
    Connection,
    TransferEncoding
};

inline HeaderKind header_kind(std::string_view name) noexcept {
    switch (name.size()) {
        case 10:
            if (eq_lit(name, "Connection", 10) ||
                ieq_lit(name, "connection", 10)) {
                return HeaderKind::Connection;
            }
            break;

        case 14:
            if (eq_lit(name, "Content-Length", 14) ||
                ieq_lit(name, "content-length", 14)) {
                return HeaderKind::ContentLength;
            }
            break;

        case 17:
            if (eq_lit(name, "Transfer-Encoding", 17) ||
                ieq_lit(name, "transfer-encoding", 17)) {
                return HeaderKind::TransferEncoding;
            }
            break;
    }

    return HeaderKind::Other;
}

inline bool parse_content_length(
    std::string_view value,
    size_t max_body,
    size_t& out
) noexcept {
    size_t v = 0;

    for (char c : value) {
        if (c < '0' || c > '9') return false;

        const size_t digit = size_t(c - '0');

        if (v > (std::numeric_limits<size_t>::max() - digit) / 10) {
            out = max_body + 1;
            return true;
        }

        v = v * 10 + digit;

        if (v > max_body) {
            out = v;
            return true;
        }
    }

    out = v;
    return true;
}

} // namespace

HttpParser::Status
HttpParser::parse(char* data, size_t len, size_t max_body, Request& out) noexcept {
    consumed_ = 0;

    out.headers.clear();
    out.body = {};
    out.query = {};

    if (out.headers.capacity() < 16) {
        out.headers.reserve(16);
    }

    char* const begin = data;
    char* const end   = data + len;
    char* p = begin;

    // METHOD SP
    char* method_b = p;

    for (;;) {
        if (p >= end) return Status::NeedMore;

        const char c = *p;

        if (c == ' ') break;
        if (!is_token(c)) return Status::BadRequest;

        ++p;
    }

    out.method = method_from({method_b, size_t(p - method_b)});
    if (out.method == Method::Unknown) return Status::BadRequest;

    ++p; // SP

    // TARGET SP
    char* target_b = p;

    for (;;) {
        if (p >= end) return Status::NeedMore;

        const char c = *p;

        if (c == ' ') break;
        if (c == '\r') return Status::BadRequest;

        ++p;
    }

    {
        std::string_view target{target_b, size_t(p - target_b)};
        const size_t q = target.find('?');

        if (q == std::string_view::npos) {
            out.path = target;
        } else {
            out.path  = target.substr(0, q);
            out.query = target.substr(q + 1);
        }
    }

    ++p; // SP

    // HTTP version: fast path for HTTP/1.1 and HTTP/1.0
    if (size_t(end - p) < 8) {
        const void* cr = std::memchr(p, '\r', size_t(end - p));
        return cr ? Status::BadRequest : Status::NeedMore;
    }

    const bool is_http11 = std::memcmp(p, "HTTP/1.1", 8) == 0;
    const bool is_http10 = std::memcmp(p, "HTTP/1.0", 8) == 0;

    if (!is_http11 && !is_http10) {
        const void* cr = std::memchr(p, '\r', size_t(end - p));
        return cr ? Status::BadRequest : Status::NeedMore;
    }

    if (size_t(end - p) < 10) return Status::NeedMore;
    if (p[8] != '\r' || p[9] != '\n') return Status::BadRequest;

    out.version = {p, 8};
    out.keep_alive = is_http11;

    p += 10;

    size_t content_length = 0;
    bool   has_cl         = false;
    bool   has_te_chunked = false;

    // Headers
    for (;;) {
        if (p >= end) return Status::NeedMore;

        if (*p == '\r') {
            if (size_t(end - p) < 2) return Status::NeedMore;
            if (p[1] != '\n') return Status::BadRequest;

            p += 2;
            break;
        }

        char* name_b = p;

        for (;;) {
            if (p >= end) return Status::NeedMore;

            const char c = *p;

            if (c == ':') break;
            if (!is_token(c)) return Status::BadRequest;

            ++p;
        }

        std::string_view name{name_b, size_t(p - name_b)};

        ++p; // ':'

        while (p < end && (*p == ' ' || *p == '\t')) {
            ++p;
        }

        char* val_b = p;
        char* cr = static_cast<char*>(
            std::memchr(p, '\r', size_t(end - p))
        );

        if (!cr) return Status::NeedMore;

        std::string_view value{val_b, size_t(cr - val_b)};

        while (!value.empty() &&
               (value.back() == ' ' || value.back() == '\t')) {
            value.remove_suffix(1);
        }

        if (size_t(end - cr) < 2) return Status::NeedMore;
        if (cr[1] != '\n') return Status::BadRequest;

        p = cr + 2;

        switch (header_kind(name)) {
            case HeaderKind::ContentLength: {
                size_t v = 0;

                if (!parse_content_length(value, max_body, v)) {
                    return Status::BadRequest;
                }

                if (v > max_body) {
                    return Status::TooLarge;
                }

                content_length = v;
                has_cl = true;
                break;
            }

            case HeaderKind::Connection:
                if (eq_lit(value, "close", 5) ||
                    ieq_lit(value, "close", 5)) {
                    out.keep_alive = false;
                } else if (eq_lit(value, "keep-alive", 10) ||
                           ieq_lit(value, "keep-alive", 10)) {
                    out.keep_alive = true;
                }
                break;

            case HeaderKind::TransferEncoding:
                // Only "chunked" is supported. Everything else (gzip, deflate,
                // multiple codings) is rejected so we don't silently mishandle.
                if (eq_lit(value, "chunked", 7) ||
                    ieq_lit(value, "chunked", 7)) {
                    has_te_chunked = true;
                } else {
                    return Status::BadRequest;
                }
                break;

            case HeaderKind::Other:
                break;
        }

        out.headers.push_back({name, value});
    }

    // Body
    const size_t header_bytes = size_t(p - begin);

    if (has_te_chunked && has_cl) {
        // RFC 7230 §3.3.3: receiver MUST reject this — ambiguous framing.
        return Status::BadRequest;
    }

    if (has_te_chunked) {
        // In-place chunked decode. Both pointers start at the body region.
        // Chunked encoding always has overhead (size lines + CRLFs) so the
        // write pointer never catches the read pointer.
        char* const body_start = p;
        char* write = body_start;

        for (;;) {
            // chunk-size = 1*HEXDIG [ chunk-ext ]
            char* size_b = p;
            size_t size = 0;
            while (p < end) {
                char c = *p;
                int hv;
                if (c >= '0' && c <= '9')       hv = c - '0';
                else if (c >= 'a' && c <= 'f')  hv = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F')  hv = c - 'A' + 10;
                else break;
                if (size > (size_t(-1) - size_t(hv)) / 16)
                    return Status::TooLarge;
                size = size * 16 + size_t(hv);
                ++p;
            }
            if (p == size_b) return Status::BadRequest;

            // Skip chunk-ext (anything until CRLF).
            while (p < end && *p != '\r' && *p != '\n') ++p;
            if (size_t(end - p) < 2) return Status::NeedMore;
            if (p[0] != '\r' || p[1] != '\n') return Status::BadRequest;
            p += 2;

            if (size == 0) {
                // last-chunk → optional trailer-section → CRLF
                for (;;) {
                    if (size_t(end - p) < 2) return Status::NeedMore;
                    if (p[0] == '\r' && p[1] == '\n') { p += 2; break; }
                    char* tcr = static_cast<char*>(
                        std::memchr(p, '\r', size_t(end - p)));
                    if (!tcr) return Status::NeedMore;
                    if (size_t(end - tcr) < 2 || tcr[1] != '\n')
                        return Status::BadRequest;
                    p = tcr + 2;
                }
                out.body  = {body_start, size_t(write - body_start)};
                consumed_ = header_bytes + size_t(p - body_start);
                return Status::Done;
            }

            // chunk-data CRLF
            if (size > max_body) return Status::TooLarge;
            if (size_t(write - body_start) + size > max_body)
                return Status::TooLarge;
            if (size_t(end - p) < size + 2) return Status::NeedMore;
            std::memmove(write, p, size);
            write += size;
            p     += size;
            if (p[0] != '\r' || p[1] != '\n') return Status::BadRequest;
            p += 2;
        }
    }

    if (has_cl) {
        if (content_length > max_body) {
            return Status::TooLarge;
        }

        if (content_length > size_t(end - p)) {
            return Status::NeedMore;
        }

        out.body = {p, content_length};
        consumed_ = header_bytes + content_length;
    } else {
        out.body = {};
        consumed_ = header_bytes;
    }

    return Status::Done;
}

} // namespace spido