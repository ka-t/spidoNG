#ifdef SPIDO_WITH_JWT

#include "server/jwt.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/sha.h>

namespace spido {

namespace {

// base64url decode. Returns true on success. Output appended to `out`.
// Accepts no padding (JWT spec).
bool b64url_decode(std::string_view in, std::vector<uint8_t>& out) {
    static constexpr uint8_t kBad = 0xff;
    static constexpr uint8_t T[128] = {
        // 0x00..0x1f
        kBad,kBad,kBad,kBad,kBad,kBad,kBad,kBad, kBad,kBad,kBad,kBad,kBad,kBad,kBad,kBad,
        kBad,kBad,kBad,kBad,kBad,kBad,kBad,kBad, kBad,kBad,kBad,kBad,kBad,kBad,kBad,kBad,
        // 0x20..0x3f
        kBad,kBad,kBad,kBad,kBad,kBad,kBad,kBad, kBad,kBad,kBad,kBad,kBad, 62, kBad,kBad,
          52, 53, 54, 55, 56, 57, 58, 59,        60, 61, kBad,kBad,kBad,kBad,kBad,kBad,
        // 0x40..0x5f
        kBad,  0,  1,  2,  3,  4,  5,  6,         7,  8,  9, 10, 11, 12, 13, 14,
          15, 16, 17, 18, 19, 20, 21, 22,        23, 24, 25,kBad,kBad,kBad,kBad, 63,
        // 0x60..0x7f
        kBad, 26, 27, 28, 29, 30, 31, 32,        33, 34, 35, 36, 37, 38, 39, 40,
          41, 42, 43, 44, 45, 46, 47, 48,        49, 50, 51,kBad,kBad,kBad,kBad,kBad,
    };

    uint32_t buf = 0;
    int      bits = 0;
    out.reserve(out.size() + (in.size() * 3) / 4);
    for (char c : in) {
        if (static_cast<unsigned char>(c) >= 128) return false;
        uint8_t v = T[static_cast<unsigned char>(c)];
        if (v == kBad) return false;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xff));
        }
    }
    return true;
}

void sha256(const uint8_t* data, size_t len, std::array<uint8_t, 32>& out) {
    SHA256(data, len, out.data());
}

// Constant-time memory compare.
bool ct_equal(const void* a, const void* b, size_t n) {
    auto* x = static_cast<const uint8_t*>(a);
    auto* y = static_cast<const uint8_t*>(b);
    uint8_t r = 0;
    for (size_t i = 0; i < n; ++i) r |= x[i] ^ y[i];
    return r == 0;
}

uint64_t now_monotonic_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1'000'000'000ULL + uint64_t(ts.tv_nsec);
}

int64_t now_unix_seconds() {
    return static_cast<int64_t>(::time(nullptr));
}

// Minimal claim extractor: looks for `"<name>":<value>` in a flat-ish JSON
// payload. JWT payloads are conventionally a single-level object so this
// is sufficient. Handles string ("..."), number, and boolean values.
// Returns the value verbatim (string keeps the surrounding quotes stripped
// for strings; numbers/bools are returned as their literal text).
std::string_view get_claim(std::string_view json, std::string_view name) {
    std::string needle;
    needle.reserve(name.size() + 3);
    needle.push_back('"');
    needle.append(name);
    needle.push_back('"');
    size_t p = json.find(needle);
    if (p == std::string_view::npos) return {};
    p += needle.size();
    // Skip whitespace and the ':'
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
    if (p >= json.size() || json[p] != ':') return {};
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
    if (p >= json.size()) return {};

    if (json[p] == '"') {
        size_t s = p + 1;
        size_t e = json.find('"', s);
        // Track simple escapes — JWT claims rarely contain `\"` but be safe.
        while (e != std::string_view::npos && e > 0 && json[e - 1] == '\\') {
            e = json.find('"', e + 1);
        }
        if (e == std::string_view::npos) return {};
        return json.substr(s, e - s);
    }
    // number or bool/null — read until comma/}.
    size_t e = p;
    while (e < json.size() && json[e] != ',' && json[e] != '}') ++e;
    while (e > p && (json[e - 1] == ' ' || json[e - 1] == '\t')) --e;
    return json.substr(p, e - p);
}

void set_reason(JwtCacheEntry& e, const char* r) {
    std::strncpy(e.reason.data(), r, e.reason.size() - 1);
    e.reason[e.reason.size() - 1] = '\0';
}

} // namespace

// ---------------------------------------------------------------- JwtCache

JwtCache::JwtCache(size_t capacity) : capacity_(capacity ? capacity : 1) {}

bool JwtCache::lookup(const std::array<uint8_t, 32>& key, JwtCacheEntry& out) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = map_.find(key);
    if (it == map_.end()) return false;
    // Move to front.
    lru_.splice(lru_.begin(), lru_, it->second);
    out = it->second->second;
    return true;
}

void JwtCache::insert(const std::array<uint8_t, 32>& key, JwtCacheEntry entry) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = map_.find(key);
    if (it != map_.end()) {
        it->second->second = entry;
        lru_.splice(lru_.begin(), lru_, it->second);
        return;
    }
    if (lru_.size() >= capacity_) {
        auto& victim = lru_.back();
        map_.erase(victim.first);
        lru_.pop_back();
    }
    lru_.emplace_front(key, entry);
    map_[key] = lru_.begin();
}

size_t JwtCache::size() const noexcept {
    std::lock_guard<std::mutex> g(mu_);
    return lru_.size();
}

// ------------------------------------------------------------- JwtVerifier

bool JwtVerifier::init(const JwtConfig& cfg) {
    cfg_ = cfg;
    cache_ = std::make_unique<JwtCache>(cfg.cache_size);

    if (cfg_.algorithm == "RS256") {
        if (cfg_.public_key_pem.empty()) return false;
        BIO* bio = BIO_new_mem_buf(cfg_.public_key_pem.data(),
                                   int(cfg_.public_key_pem.size()));
        if (!bio) return false;
        EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!pkey) return false;
        pkey_ = pkey;
    } else if (cfg_.algorithm == "HS256") {
        if (cfg_.secret.empty()) return false;
    } else {
        return false; // unsupported algorithm
    }
    return true;
}

JwtStatus JwtVerifier::verify(std::string_view token, JwtCacheEntry& out) {
    out = {};
    out.cached_at_ns = now_monotonic_ns();

    // Hash for cache key.
    std::array<uint8_t, 32> key;
    sha256(reinterpret_cast<const uint8_t*>(token.data()), token.size(), key);

    // Cache hit?
    JwtCacheEntry cached;
    if (cache_->lookup(key, cached)) {
        // Verify exp against now (cache entries can stale through expiry).
        if (cached.valid && cached.exp != 0
            && cached.exp <= now_unix_seconds()) {
            cached.valid = false;
            set_reason(cached, "expired");
            cache_->insert(key, cached);
            out = cached;
            return JwtStatus::Expired;
        }
        out = cached;
        return cached.valid ? JwtStatus::Valid
                            : (cached.reason[0] == 'e' && cached.reason[1] == 'x'
                               ? JwtStatus::Expired
                               : JwtStatus::BadSignature);
    }

    // Parse: header.payload.signature
    size_t d1 = token.find('.');
    if (d1 == std::string_view::npos) {
        set_reason(out, "malformed"); cache_->insert(key, out);
        return JwtStatus::Malformed;
    }
    size_t d2 = token.find('.', d1 + 1);
    if (d2 == std::string_view::npos) {
        set_reason(out, "malformed"); cache_->insert(key, out);
        return JwtStatus::Malformed;
    }

    std::string_view header_b64    = token.substr(0, d1);
    std::string_view payload_b64   = token.substr(d1 + 1, d2 - d1 - 1);
    std::string_view sig_b64       = token.substr(d2 + 1);
    std::string_view signed_bytes  = token.substr(0, d2);

    std::vector<uint8_t> sig;
    if (!b64url_decode(sig_b64, sig)) {
        set_reason(out, "malformed"); cache_->insert(key, out);
        return JwtStatus::Malformed;
    }

    // Signature verify.
    bool sig_ok = false;
    if (cfg_.algorithm == "HS256") {
        unsigned char mac[32];
        unsigned int  mac_len = 0;
        HMAC(EVP_sha256(),
             cfg_.secret.data(), int(cfg_.secret.size()),
             reinterpret_cast<const unsigned char*>(signed_bytes.data()),
             signed_bytes.size(),
             mac, &mac_len);
        sig_ok = mac_len == 32 && sig.size() == 32 &&
                 ct_equal(mac, sig.data(), 32);
    } else { // RS256
        auto* pkey = static_cast<EVP_PKEY*>(pkey_);
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (!ctx) {
            set_reason(out, "internal"); cache_->insert(key, out);
            return JwtStatus::Internal;
        }
        if (EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) == 1 &&
            EVP_DigestVerifyUpdate(ctx,
                reinterpret_cast<const unsigned char*>(signed_bytes.data()),
                signed_bytes.size()) == 1 &&
            EVP_DigestVerifyFinal(ctx, sig.data(), sig.size()) == 1) {
            sig_ok = true;
        }
        EVP_MD_CTX_free(ctx);
    }
    if (!sig_ok) {
        set_reason(out, "bad_signature"); cache_->insert(key, out);
        return JwtStatus::BadSignature;
    }

    // Decode payload to check claims.
    std::vector<uint8_t> payload;
    if (!b64url_decode(payload_b64, payload)) {
        set_reason(out, "malformed"); cache_->insert(key, out);
        return JwtStatus::Malformed;
    }
    std::string_view pjson(reinterpret_cast<const char*>(payload.data()),
                           payload.size());

    // exp / nbf
    int64_t now_s = now_unix_seconds();
    auto exp_v = get_claim(pjson, "exp");
    if (!exp_v.empty()) {
        int64_t exp = 0;
        for (char c : exp_v) {
            if (c < '0' || c > '9') { exp = 0; break; }
            exp = exp * 10 + (c - '0');
        }
        out.exp = exp;
        if (exp && exp <= now_s) {
            out.valid = false; set_reason(out, "expired");
            cache_->insert(key, out);
            return JwtStatus::Expired;
        }
    }
    auto nbf_v = get_claim(pjson, "nbf");
    if (!nbf_v.empty()) {
        int64_t nbf = 0;
        for (char c : nbf_v) {
            if (c < '0' || c > '9') { nbf = 0; break; }
            nbf = nbf * 10 + (c - '0');
        }
        if (nbf > now_s + int64_t(cfg_.leeway_seconds)) {
            out.valid = false; set_reason(out, "not_yet_valid");
            cache_->insert(key, out);
            return JwtStatus::NotYetValid;
        }
    }

    // iss / aud
    if (!cfg_.issuer.empty()) {
        auto iss = get_claim(pjson, "iss");
        if (iss != cfg_.issuer) {
            out.valid = false; set_reason(out, "bad_issuer");
            cache_->insert(key, out);
            return JwtStatus::BadIssuer;
        }
    }
    if (!cfg_.audience.empty()) {
        auto aud = get_claim(pjson, "aud");
        if (aud != cfg_.audience) {
            out.valid = false; set_reason(out, "bad_audience");
            cache_->insert(key, out);
            return JwtStatus::BadAudience;
        }
    }

    // Required claims (presence only — semantics belong to the app).
    for (const auto& name : cfg_.required_claims) {
        if (name == "exp" || name == "nbf" ||
            name == "iss" || name == "aud") continue;
        if (get_claim(pjson, name).empty()) {
            out.valid = false; set_reason(out, "missing_claim");
            cache_->insert(key, out);
            return JwtStatus::MissingClaim;
        }
    }

    out.valid = true;
    set_reason(out, "valid");
    cache_->insert(key, out);
    return JwtStatus::Valid;
}

// ------------------------------------------------------------ Bearer / strings

const char* jwt_status_str(JwtStatus s) noexcept {
    switch (s) {
        case JwtStatus::Valid:        return "valid";
        case JwtStatus::Missing:      return "missing";
        case JwtStatus::Malformed:    return "malformed";
        case JwtStatus::BadSignature: return "bad_signature";
        case JwtStatus::Expired:      return "expired";
        case JwtStatus::NotYetValid:  return "not_yet_valid";
        case JwtStatus::BadIssuer:    return "bad_issuer";
        case JwtStatus::BadAudience:  return "bad_audience";
        case JwtStatus::MissingClaim: return "missing_claim";
        case JwtStatus::Internal:     return "internal";
    }
    return "unknown";
}

std::string_view extract_bearer(std::string_view authz) noexcept {
    // Strip leading whitespace
    while (!authz.empty() && (authz.front() == ' ' || authz.front() == '\t'))
        authz.remove_prefix(1);
    constexpr std::string_view prefix("Bearer ");
    if (authz.size() < prefix.size()) return {};
    // Case-insensitive prefix match.
    for (size_t i = 0; i < prefix.size(); ++i) {
        char a = authz[i], b = prefix[i];
        if (a >= 'A' && a <= 'Z') a |= 0x20;
        if (b >= 'A' && b <= 'Z') b |= 0x20;
        if (a != b) return {};
    }
    auto tok = authz.substr(prefix.size());
    while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t'))
        tok.remove_prefix(1);
    while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t'))
        tok.remove_suffix(1);
    return tok;
}

} // namespace spido

#endif // SPIDO_WITH_JWT
