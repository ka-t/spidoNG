#pragma once

#ifdef SPIDO_WITH_JWT

#include <array>
#include <atomic>
#include <chrono>
#include <list>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <memory>

namespace spido {

// JWT validation configuration. When `enabled` is false the entire JWT
// path is bypassed at zero per-request cost (a single bool check before
// any header lookup).
struct JwtConfig {
    bool        enabled         = false;
    std::string algorithm       = "HS256";   // HS256 or RS256
    std::string secret;                      // HS256: shared key bytes
    std::string public_key_pem;              // RS256: PEM-encoded RSA public key
    std::vector<std::string> required_claims = {"exp"};
    std::string issuer;                      // if non-empty, must match `iss`
    std::string audience;                    // if non-empty, must match `aud`
    size_t      cache_size      = 65536;
    // Allow `nbf` to be in the past by this much (clock skew).
    unsigned    leeway_seconds  = 30;
};

// Result of JWT verification — kept tiny so the cache stores it cheaply.
struct JwtCacheEntry {
    int64_t  exp           = 0;   // unix seconds (claim "exp" or 0)
    uint64_t cached_at_ns  = 0;   // monotonic ns at insertion
    bool     valid         = false;
    // First failure reason (kept short for the 401 body).
    std::array<char, 24> reason{}; // null-terminated short string
};

// Thread-safe LRU cache: SHA-256(raw_token) → JwtCacheEntry. Hot path
// takes the shared lock for read; insertion takes the unique lock.
class JwtCache {
public:
    explicit JwtCache(size_t capacity);

    // Returns true if hit; copies the entry to `out`. The entry is moved
    // to the front of the LRU list (under the lock).
    bool lookup(const std::array<uint8_t, 32>& key, JwtCacheEntry& out);
    void insert(const std::array<uint8_t, 32>& key, JwtCacheEntry entry);

    size_t size() const noexcept;

private:
    struct Hasher {
        size_t operator()(const std::array<uint8_t, 32>& k) const noexcept {
            // FNV-1a over 32 bytes; the input is already cryptographic so
            // distribution is uniform — we only need a quick fold.
            uint64_t h = 1469598103934665603ULL;
            for (uint8_t b : k) {
                h ^= b;
                h *= 1099511628211ULL;
            }
            return static_cast<size_t>(h);
        }
    };

    using KeyType = std::array<uint8_t, 32>;
    using ListIt  = std::list<std::pair<KeyType, JwtCacheEntry>>::iterator;

    size_t                                     capacity_;
    mutable std::mutex                         mu_;
    std::list<std::pair<KeyType, JwtCacheEntry>> lru_;
    std::unordered_map<KeyType, ListIt, Hasher>  map_;
};

// Verification result reported to the worker. `entry` is the cached form
// suitable for re-insertion. When verifying succeeded the worker may
// also populate Request fields with claims for the handler.
enum class JwtStatus : uint8_t {
    Valid,
    Missing,        // no Authorization header
    Malformed,      // header not Bearer / token not three parts / bad b64
    BadSignature,
    Expired,
    NotYetValid,
    BadIssuer,
    BadAudience,
    MissingClaim,
    Internal,       // OpenSSL/EVP failure
};

const char* jwt_status_str(JwtStatus s) noexcept;

class JwtVerifier {
public:
    JwtVerifier() = default;
    bool init(const JwtConfig& cfg); // validates cfg + parses RS256 key

    // Verify a single token. Computes SHA-256 of the token, consults the
    // cache, and on miss runs full signature + claim validation. On exit
    // `out_entry` carries the result (and `entry.reason` an error code in
    // text form) ready to be cached/served.
    JwtStatus verify(std::string_view token, JwtCacheEntry& out_entry);

    const JwtConfig& config() const noexcept { return cfg_; }

private:
    JwtConfig cfg_;
    std::unique_ptr<JwtCache> cache_;
    // RS256: parsed EVP_PKEY*. We use a raw void* so the header doesn't
    // pull in OpenSSL — the .cpp side reinterpret_casts.
    void*     pkey_ = nullptr;
};

// Extract bearer token from an Authorization header value. Returns the
// view past "Bearer " (case-insensitive) or empty.
std::string_view extract_bearer(std::string_view authz) noexcept;

} // namespace spido

#endif // SPIDO_WITH_JWT

