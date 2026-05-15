// Minimal self-contained test runner for spido's HTTP/1.1 parser.
// No external dep; counts pass/fail and exits non-zero on any failure.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "server/http_parser.h"
#include "server/router.h"
#include "spido/request.h"
#include "spido/response.h"

#ifdef SPIDO_WITH_JWT
#include "server/jwt.h"
#include <openssl/hmac.h>
#endif

using namespace spido;

namespace {

int g_failed = 0;
int g_passed = 0;
const char* g_current = "";

#define CHECK(cond) do {                                                     \
    if (!(cond)) {                                                           \
        std::fprintf(stderr, "  FAIL %s:%d in %s: %s\n",                     \
                     __FILE__, __LINE__, g_current, #cond);                  \
        ++g_failed;                                                          \
    } else {                                                                 \
        ++g_passed;                                                          \
    }                                                                        \
} while (0)

#define RUN(name) do { g_current = #name; std::printf("== %s ==\n", #name); \
                       test_##name(); } while (0)

// Helper: feed `s` to parser, returns status + Request.
HttpParser::Status feed(const std::string& s_in, Request& out,
                        size_t max_body = 1 << 20) {
    static thread_local std::string buf;
    buf = s_in;  // mutable copy because parser mutates for chunked decode
    HttpParser p;
    return p.parse(buf.data(), buf.size(), max_body, out);
}

void test_simple_get() {
    Request r;
    auto s = feed("GET /health HTTP/1.1\r\nHost: x\r\n\r\n", r);
    CHECK(s == HttpParser::Status::Done);
    CHECK(r.method == Method::GET);
    CHECK(r.path == "/health");
    CHECK(r.version == "HTTP/1.1");
    CHECK(r.keep_alive == true);
    CHECK(r.headers.size() == 1);
    CHECK(r.body.empty());
}

void test_query_string() {
    Request r;
    auto s = feed("GET /search?q=hello&n=5 HTTP/1.1\r\nHost: x\r\n\r\n", r);
    CHECK(s == HttpParser::Status::Done);
    CHECK(r.path == "/search");
    CHECK(r.query == "q=hello&n=5");
}

void test_connection_close() {
    Request r;
    auto s = feed("GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", r);
    CHECK(s == HttpParser::Status::Done);
    CHECK(r.keep_alive == false);
}

void test_http_10_defaults_close() {
    Request r;
    auto s = feed("GET / HTTP/1.0\r\nHost: x\r\n\r\n", r);
    CHECK(s == HttpParser::Status::Done);
    CHECK(r.keep_alive == false);  // 1.0 defaults to close
}

void test_post_content_length() {
    Request r;
    auto s = feed(
        "POST /up HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello", r);
    CHECK(s == HttpParser::Status::Done);
    CHECK(r.method == Method::POST);
    CHECK(r.body == "hello");
}

void test_partial_body_need_more() {
    Request r;
    auto s = feed("POST /up HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nhi", r);
    CHECK(s == HttpParser::Status::NeedMore);
}

void test_bad_method() {
    Request r;
    auto s = feed("YEET / HTTP/1.1\r\n\r\n", r);
    CHECK(s == HttpParser::Status::BadRequest);
}

void test_bad_version() {
    Request r;
    auto s = feed("GET / HTTP/9.9\r\n\r\n", r);
    CHECK(s == HttpParser::Status::BadRequest);
}

void test_oversized_body() {
    Request r;
    auto s = feed("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1000\r\n\r\n",
                  r, /*max_body=*/100);
    CHECK(s == HttpParser::Status::TooLarge);
}

void test_chunked_basic() {
    Request r;
    // 4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n → "Wikipedia"
    auto s = feed(
        "POST /u HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
        "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n", r);
    CHECK(s == HttpParser::Status::Done);
    CHECK(r.body == "Wikipedia");
}

void test_chunked_with_extension() {
    Request r;
    auto s = feed(
        "POST /u HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
        "4;foo=bar\r\nABCD\r\n0\r\n\r\n", r);
    CHECK(s == HttpParser::Status::Done);
    CHECK(r.body == "ABCD");
}

void test_chunked_partial() {
    Request r;
    auto s = feed(
        "POST /u HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
        "4\r\nWik", r);
    CHECK(s == HttpParser::Status::NeedMore);
}

void test_chunked_with_trailer() {
    Request r;
    auto s = feed(
        "POST /u HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nhello\r\n0\r\nX-Trail: yes\r\n\r\n", r);
    CHECK(s == HttpParser::Status::Done);
    CHECK(r.body == "hello");
}

void test_both_cl_and_te_rejected() {
    Request r;
    auto s = feed(
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n"
        "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n", r);
    CHECK(s == HttpParser::Status::BadRequest);
}

void test_unknown_transfer_encoding_rejected() {
    Request r;
    auto s = feed(
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip\r\n\r\n", r);
    CHECK(s == HttpParser::Status::BadRequest);
}

void test_pipelined() {
    Request r1;
    std::string two = "GET /a HTTP/1.1\r\nHost: x\r\n\r\n"
                      "GET /b HTTP/1.1\r\nHost: x\r\n\r\n";
    static thread_local std::string buf;
    buf = two;
    HttpParser p;
    auto s1 = p.parse(buf.data(), buf.size(), 1<<20, r1);
    CHECK(s1 == HttpParser::Status::Done);
    CHECK(r1.path == "/a");
    size_t used = p.consumed();
    CHECK(used == strlen("GET /a HTTP/1.1\r\nHost: x\r\n\r\n"));

    Request r2;
    auto s2 = p.parse(buf.data() + used, buf.size() - used, 1<<20, r2);
    CHECK(s2 == HttpParser::Status::Done);
    CHECK(r2.path == "/b");
}

void test_url_decode() {
    CHECK(Request::decode("/hello%20world") == "/hello world");
    CHECK(Request::decode("a%2Bb%3Dc")     == "a+b=c");
    CHECK(Request::decode("nopct")          == "nopct");
    CHECK(Request::decode("a+b", true)      == "a b");
    CHECK(Request::decode("a+b", false)     == "a+b");
    CHECK(Request::decode("bad%2") == "bad%2");   // truncated %xx kept as-is
}

void test_router_static_match() {
    Router r;
    r.add(Method::GET, "/health", [](Request&, Response&){});
    auto* e = r.find(Method::GET, "/health");
    CHECK(e != nullptr);
    CHECK(r.find(Method::GET, "/missing") == nullptr);
    CHECK(r.find(Method::POST, "/health") == nullptr);
}

void test_router_param() {
    Router r;
    r.add(Method::GET, "/users/:id", [](Request&, Response&){});
    std::vector<std::pair<std::string_view, std::string_view>> p;
    auto* e = r.find(Method::GET, "/users/42", &p);
    CHECK(e != nullptr);
    CHECK(p.size() == 1);
    CHECK(p[0].first == "id");
    CHECK(p[0].second == "42");
}

void test_router_two_params() {
    Router r;
    r.add(Method::GET, "/orgs/:org/repos/:repo", [](Request&, Response&){});
    std::vector<std::pair<std::string_view, std::string_view>> p;
    auto* e = r.find(Method::GET, "/orgs/spido/repos/main", &p);
    CHECK(e != nullptr);
    CHECK(p.size() == 2);
    CHECK(p[0].first == "org");   CHECK(p[0].second == "spido");
    CHECK(p[1].first == "repo");  CHECK(p[1].second == "main");
}

#ifdef SPIDO_WITH_JWT

// Helpers for hand-rolled JWT.
std::string b64u(const std::string& in) {
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    int bits = 0; uint32_t buf = 0;
    for (unsigned char c : in) {
        buf = (buf << 8) | c; bits += 8;
        while (bits >= 6) { bits -= 6; out.push_back(A[(buf >> bits) & 0x3f]); }
    }
    if (bits) out.push_back(A[(buf << (6 - bits)) & 0x3f]);
    return out;
}
std::string hs256(const std::string& key, const std::string& msg) {
    unsigned char mac[32]; unsigned int ml = 0;
    HMAC(EVP_sha256(), key.data(), int(key.size()),
         reinterpret_cast<const unsigned char*>(msg.data()), msg.size(),
         mac, &ml);
    return std::string(reinterpret_cast<char*>(mac), ml);
}
std::string mint_jwt(const std::string& secret, const std::string& payload_json) {
    std::string h = b64u("{\"alg\":\"HS256\",\"typ\":\"JWT\"}");
    std::string p = b64u(payload_json);
    std::string s = b64u(hs256(secret, h + "." + p));
    return h + "." + p + "." + s;
}

void test_jwt_bearer_extract() {
    CHECK(extract_bearer("Bearer abc.def.ghi") == "abc.def.ghi");
    CHECK(extract_bearer("bearer xyz")          == "xyz");
    CHECK(extract_bearer("  Bearer  xyz  ")     == "xyz");
    CHECK(extract_bearer("Basic xyz").empty());
    CHECK(extract_bearer("").empty());
}

void test_jwt_valid() {
    JwtConfig c;
    c.algorithm = "HS256";
    c.secret    = "k";
    c.required_claims = {"exp"};
    JwtVerifier v;
    CHECK(v.init(c));
    auto now = std::time(nullptr) + 1000;
    auto tok = mint_jwt(c.secret, "{\"sub\":\"a\",\"exp\":" + std::to_string(now) + "}");
    JwtCacheEntry e;
    CHECK(v.verify(tok, e) == JwtStatus::Valid);
    CHECK(e.valid);
}

void test_jwt_expired() {
    JwtConfig c; c.algorithm = "HS256"; c.secret = "k";
    JwtVerifier v; CHECK(v.init(c));
    auto past = std::time(nullptr) - 1000;
    auto tok = mint_jwt(c.secret, "{\"exp\":" + std::to_string(past) + "}");
    JwtCacheEntry e;
    CHECK(v.verify(tok, e) == JwtStatus::Expired);
    CHECK(!e.valid);
}

void test_jwt_bad_sig() {
    JwtConfig c; c.algorithm = "HS256"; c.secret = "k";
    JwtVerifier v; CHECK(v.init(c));
    auto tok = mint_jwt("wrong-key", "{\"exp\":9999999999}");
    JwtCacheEntry e;
    CHECK(v.verify(tok, e) == JwtStatus::BadSignature);
}

void test_jwt_bad_issuer() {
    JwtConfig c; c.algorithm = "HS256"; c.secret = "k"; c.issuer = "us";
    JwtVerifier v; CHECK(v.init(c));
    auto tok = mint_jwt(c.secret, "{\"iss\":\"them\",\"exp\":9999999999}");
    JwtCacheEntry e;
    CHECK(v.verify(tok, e) == JwtStatus::BadIssuer);
}

void test_jwt_missing_claim() {
    JwtConfig c; c.algorithm = "HS256"; c.secret = "k";
    c.required_claims = {"exp", "sub"};
    JwtVerifier v; CHECK(v.init(c));
    auto tok = mint_jwt(c.secret, "{\"exp\":9999999999}");  // no sub
    JwtCacheEntry e;
    CHECK(v.verify(tok, e) == JwtStatus::MissingClaim);
}

void test_jwt_malformed() {
    JwtConfig c; c.algorithm = "HS256"; c.secret = "k";
    JwtVerifier v; CHECK(v.init(c));
    JwtCacheEntry e;
    CHECK(v.verify("not-a-jwt", e) == JwtStatus::Malformed);
    CHECK(v.verify("only.two", e)  == JwtStatus::Malformed);
}

void test_jwt_cache_hit() {
    JwtConfig c; c.algorithm = "HS256"; c.secret = "k"; c.cache_size = 4;
    JwtVerifier v; CHECK(v.init(c));
    auto tok = mint_jwt(c.secret, "{\"exp\":9999999999}");
    JwtCacheEntry e1, e2;
    CHECK(v.verify(tok, e1) == JwtStatus::Valid);
    // Second call should hit cache (verified by cached_at_ns being older
    // than now). We just check it still returns Valid quickly.
    CHECK(v.verify(tok, e2) == JwtStatus::Valid);
    CHECK(e2.valid);
}

#endif // SPIDO_WITH_JWT

void test_router_static_preferred_over_wildcard() {
    Router r;
    r.add(Method::GET, "/users/:id", [](Request&, Response&){ });
    r.add(Method::GET, "/users/me",  [](Request&, Response&){ });
    std::vector<std::pair<std::string_view, std::string_view>> p;
    // Static literal "me" must win over wildcard.
    auto* e = r.find(Method::GET, "/users/me", &p);
    CHECK(e != nullptr);
    CHECK(p.empty());
}

} // namespace

int main() {
    RUN(simple_get);
    RUN(query_string);
    RUN(connection_close);
    RUN(http_10_defaults_close);
    RUN(post_content_length);
    RUN(partial_body_need_more);
    RUN(bad_method);
    RUN(bad_version);
    RUN(oversized_body);
    RUN(chunked_basic);
    RUN(chunked_with_extension);
    RUN(chunked_partial);
    RUN(chunked_with_trailer);
    RUN(both_cl_and_te_rejected);
    RUN(unknown_transfer_encoding_rejected);
    RUN(pipelined);
    RUN(url_decode);
    RUN(router_static_match);
    RUN(router_param);
    RUN(router_two_params);
    RUN(router_static_preferred_over_wildcard);
#ifdef SPIDO_WITH_JWT
    RUN(jwt_bearer_extract);
    RUN(jwt_valid);
    RUN(jwt_expired);
    RUN(jwt_bad_sig);
    RUN(jwt_bad_issuer);
    RUN(jwt_missing_claim);
    RUN(jwt_malformed);
    RUN(jwt_cache_hit);
#endif

    std::printf("\n--- %d passed, %d failed ---\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
