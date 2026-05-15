#include "spido_pg/cache.h"
#include "sha256.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace spido_pg {

namespace {

// Per-thread L1: open-addressed hash table sized to a power of two so the
// modulus folds into a bitmask. Each get hashes the key once, probes up to
// kProbe slots, and returns. No locks, no allocations, no per-slot string
// compare unless the hash collides. Inserts pick the LRU victim *within*
// the probe range — so a single hot key never evicts an unrelated one
// just because they share an LRU counter ordering. Capacity is fixed at
// init time, must be a power of two.
struct L1Entry {
    // Fixed-size key buffer: avoids the std::string allocation on every
    // L1 put. SHA-256-hex keys are 64 bytes; we round up to 80 to leave
    // room for other key formats (the cache stays correct with longer
    // keys, they just spill past the buffer and fall back to comparing
    // by hash only — extremely low collision odds with 64-bit hashes).
    static constexpr size_t kKeyMax = 80;
    char                                              key[kKeyMax]{};
    uint8_t                                           key_len   = 0;
    QueryCache::ValuePtr                              value;     // borrowed from L2
    std::chrono::steady_clock::time_point             expires_at;
    uint64_t                                          last_used = 0;
    uint64_t                                          hash      = 0;
    bool                                              occupied  = false;

    bool matches(std::string_view k) const noexcept {
        // Fast hash-equality is the primary gate (caller already checked
        // hash). For the key compare itself we use a length check + memcmp
        // — both branchless on a successful match for our typical 64-byte
        // keys. If the caller's key is longer than our buffer, we can
        // only check the prefix; combined with the 64-bit hash match
        // that's still ~2^-64 collision probability.
        size_t n = k.size() < kKeyMax ? k.size() : kKeyMax;
        return key_len == k.size() && std::memcmp(key, k.data(), n) == 0;
    }
    void set_key(std::string_view k) noexcept {
        size_t n = k.size() < kKeyMax ? k.size() : kKeyMax;
        std::memcpy(key, k.data(), n);
        key_len = static_cast<uint8_t>(n);
    }
};

inline uint64_t l1_hash(std::string_view s) {
    // The make_cache_key() helper produces 64-char SHA-256 hex strings, so
    // by the time a key reaches the cache it's already a high-entropy
    // random byte stream. Pack the first 8 bytes directly — that's >32
    // bits of randomness for bucket selection, no FNV-mix required, and
    // it's a single misaligned load instead of a per-byte multiply loop.
    // For shorter / non-hex keys we fall back to FNV1a so the cache still
    // behaves correctly if someone uses it without make_cache_key.
    uint64_t h = 0;
    if (s.size() >= 8) {
        std::memcpy(&h, s.data(), 8);
    } else {
        h = 0xcbf29ce484222325ULL;
        for (unsigned char c : s) { h ^= c; h *= 0x100000001b3ULL; }
    }
    return h ? h : 1;
}

struct L1 {
    std::vector<L1Entry> slots;
    uint64_t             counter = 0;
    size_t               mask    = 0;

    static constexpr size_t kProbe = 8;

    void init(size_t cap) {
        if (!slots.empty()) return;
        // Round up to next power of two so the modulus is a bitmask.
        size_t n = 1;
        while (n < cap) n <<= 1;
        slots.resize(n);
        mask = n - 1;
    }

    // Returns the matched entry without doing a TTL check — caller does
    // that lazily. This lets get() skip the steady_clock::now() syscall on
    // every call and only pay it when we have a hit.
    L1Entry* find_raw(std::string_view key) {
        if (slots.empty()) return nullptr;
        uint64_t h = l1_hash(key);
        for (size_t i = 0; i < kProbe; ++i) {
            auto& s = slots[(h + i) & mask];
            if (!s.occupied) return nullptr;             // probe-run terminator
            if (s.hash == h && s.matches(key)) {
                s.last_used = ++counter;
                return &s;
            }
        }
        return nullptr;
    }

    void put(std::string_view key,
             QueryCache::ValuePtr v,
             std::chrono::steady_clock::time_point expires)
    {
        if (slots.empty()) return;
        uint64_t h = l1_hash(key);
        L1Entry* victim = nullptr;
        for (size_t i = 0; i < kProbe; ++i) {
            auto& s = slots[(h + i) & mask];
            if (!s.occupied) { victim = &s; break; }
            if (s.hash == h && s.matches(key)) { victim = &s; break; }
        }
        if (!victim) {
            victim = &slots[h & mask];
            for (size_t i = 1; i < kProbe; ++i) {
                auto& s = slots[(h + i) & mask];
                if (s.last_used < victim->last_used) victim = &s;
            }
        }
        victim->set_key(key);
        victim->value      = std::move(v);
        victim->expires_at = expires;
        victim->hash       = h;
        victim->last_used  = ++counter;
        victim->occupied   = true;
    }

    void clear() { for (auto& s : slots) s.occupied = false; }
};

thread_local L1 t_l1;

// Stats counters. We tried a TLS-accumulator design here but it was
// fragile across thread lifetimes (registry-of-pointers + thread_local
// destruction order = use-after-free at process teardown) and the wins
// were marginal (~5 ns/get) compared to relaxed-atomic counters. Stick
// with atomics: simple, correct, and almost-free on x86 when the cache
// line isn't contended.
struct CacheStatsCounters {
    std::atomic<uint64_t> l1_hits{0};
    std::atomic<uint64_t> l1_misses{0};
    std::atomic<uint64_t> l2_hits{0};
    std::atomic<uint64_t> l2_misses{0};
};

// Approximate size of a PgResult — used to bound L2 by max_bytes.
size_t result_size_bytes(const PgResult& r) {
    size_t s = sizeof(PgResult);
    for (const auto& f : r.fields) s += f.name.size() + sizeof(PgField);
    for (const auto& row : r.rows) {
        s += sizeof(PgRow);
        for (const auto& cell : row.cells) s += cell.size();
        s += row.nulls.size();
    }
    s += r.tag.size() + r.error_severity.size()
       + r.error_code.size() + r.error_message.size();
    return s;
}

} // namespace

std::string make_cache_key(std::string_view sql, std::span<const PgParam> params) {
    // SHA-256(sql || 0xFF || for each param: type(u32 LE) | nullbyte | bytes)
    std::string materialized;
    materialized.reserve(sql.size() + params.size() * 16);
    materialized.append(sql);
    materialized.push_back('\xff');
    for (const auto& p : params) {
        uint32_t t = static_cast<uint32_t>(p.type);
        materialized.append(reinterpret_cast<const char*>(&t), 4);
        materialized.push_back(p.is_null ? '\x01' : '\x00');
        materialized.append(p.value);
        materialized.push_back('\xfe');
    }
    return sha256::hex(materialized);
}

QueryCache::QueryCache(CacheConfig cfg) : cfg_(cfg) {
    // Heap-allocate each shard — 64 × (1024 slot × ~256 byte slot) ≈ 16 MB.
    // alignas(64) on Slot keeps each slot on its own cache line so writes
    // to neighbouring slots don't false-share.
    for (auto& sh : shards_) sh = std::make_unique<Shard>();
    if (cfg_.sweep_interval_ms > 0) {
        sweep_thread_ = std::thread([this]{ sweep_loop(); });
    }
}

QueryCache::~QueryCache() {
    stop_.store(true, std::memory_order_release);
    if (sweep_thread_.joinable()) sweep_thread_.join();
}

// Hash full SHA-256-hex keys cheaply by packing two 8-byte words. For
// shorter / arbitrary keys fall back to FNV-1a so the cache stays correct
// regardless of the caller's key style.
uint64_t QueryCache::key_hash(std::string_view key) noexcept {
    if (key.size() >= 16) {
        uint64_t a, b;
        std::memcpy(&a, key.data(),     8);
        std::memcpy(&b, key.data() + 8, 8);
        uint64_t h = a ^ (b * 0x9e3779b97f4a7c15ULL);
        return h ? h : 1;                    // 0 reserved as empty marker
    }
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : key) { h ^= c; h *= 0x100000001b3ULL; }
    return h ? h : 1;
}

QueryCache::ValuePtr QueryCache::get(std::string_view key) {
    // L1 hot path. NO clock call here — at ~30 ns per syscall (vDSO),
    // steady_clock::now() would dominate the path. We trade strict-TTL
    // accuracy for speed: an expired L1 entry can be served until the
    // background sweep_loop drops it (worst case = sweep_interval_ms of
    // staleness on top of the TTL). Stats live in TLS counters.
    t_l1.init(cfg_.l1_capacity);
    if (auto* hit = t_l1.find_raw(key)) {
        l1_hits_.fetch_add(1, std::memory_order_relaxed);
        return hit->value;
    }
    l1_misses_.fetch_add(1, std::memory_order_relaxed);

    // L2 — lock-free hash probe; only take the per-slot shared_lock on a
    // hash hit so misses don't touch any mutex at all.
    uint64_t h = key_hash(key);
    auto&    sh = *shards_[shard_for(h)];
    size_t   base = slot_for(h);
    for (size_t i = 0; i < kProbeRange; ++i) {
        Slot& s = sh.slots[(base + i) & kSlotMask];
        uint64_t slot_h = s.hash.load(std::memory_order_acquire);
        if (slot_h == 0) {
            // Empty slot — end of probe chain (open-addressed convention).
            l2_misses_.fetch_add(1, std::memory_order_relaxed);
            return {};
        }
        if (slot_h != h) continue;

        // Hash match. Short shared_lock for the key/value compare; on
        // collision (hash matches but key doesn't, very rare with the
        // SHA-256-derived keys we use) we fall through to the next probe.
        std::shared_lock lk(s.mtx);
        if (s.hash.load(std::memory_order_relaxed) != h) continue;  // recycled
        if (s.key != key) continue;                                  // collision

        // TTL skip on the hot path too — sweep handles real expiry; dirty
        // entries are pinned by sweep so we can safely serve stale dirty
        // state until the BatchWriter ACKs.
        ValuePtr v = s.value;
        auto     expires = s.expires_at;
        s.last_used.store(sh.counter.fetch_add(1, std::memory_order_relaxed) + 1,
                          std::memory_order_relaxed);
        l2_hits_.fetch_add(1, std::memory_order_relaxed);
        lk.unlock();
        // Promote to L1 outside the slot lock — t_l1 is TLS, no contention.
        t_l1.put(key, v, expires);
        return v;
    }
    // Probe range exhausted without finding the key.
    l2_misses_.fetch_add(1, std::memory_order_relaxed);
    return {};
}

void QueryCache::put(std::string_view key,
                     PgResult value,
                     std::chrono::seconds ttl,
                     std::string_view table_tag)
{
    auto expires = std::chrono::steady_clock::now() + ttl;
    size_t bytes = result_size_bytes(value);
    // One allocation; the value lives behind a shared_ptr from here on so
    // every subsequent get() avoids the PgResult copy.
    auto ptr = std::make_shared<const PgResult>(std::move(value));

    uint64_t h     = key_hash(key);
    auto&    sh    = *shards_[shard_for(h)];
    size_t   base  = slot_for(h);

    // 1. Look for an existing entry with the same hash+key in probe range.
    // 2. Else look for an empty slot.
    // 3. Else pick the LRU victim within the probe range (skipping dirty).
    Slot*    target      = nullptr;
    Slot*    empty_slot  = nullptr;
    Slot*    lru_victim  = nullptr;
    uint64_t lru_seen    = UINT64_MAX;

    auto is_dirty = [&](std::string_view tag) {
        if (tag.empty()) return false;
        std::lock_guard dl(dirty_mtx_);
        auto it = dirty_.find(tag);
        return it != dirty_.end() && it->second > 0;
    };

    for (size_t i = 0; i < kProbeRange; ++i) {
        Slot& s = sh.slots[(base + i) & kSlotMask];
        uint64_t slot_h = s.hash.load(std::memory_order_acquire);
        if (slot_h == 0) {
            if (!empty_slot) empty_slot = &s;
            continue;
        }
        if (slot_h == h) {
            // Possible match — confirm key under shared_lock, then upgrade.
            std::shared_lock rlk(s.mtx);
            if (s.hash.load(std::memory_order_relaxed) == h && s.key == key) {
                target = &s;
                break;
            }
        }
        // Track LRU candidate (relaxed read OK; LRU is approximate).
        uint64_t lu = s.last_used.load(std::memory_order_relaxed);
        if (lu < lru_seen) { lru_seen = lu; lru_victim = &s; }
    }

    if (!target) target = empty_slot;
    if (!target) {
        // Probe range full. Pick the LRU victim, but only if it's not
        // pinned dirty — otherwise grow the shard temporarily by spilling
        // into the *next* slot beyond the probe range. The shard is fixed
        // size though, so if we truly can't find a non-dirty slot we
        // accept losing the LRU entry under contention (better than
        // crashing). Most workloads never hit this branch.
        if (lru_victim) {
            std::shared_lock rlk(lru_victim->mtx);
            if (is_dirty(lru_victim->table_tag)) {
                // Try to find ANY non-dirty slot in probe range.
                for (size_t i = 0; i < kProbeRange; ++i) {
                    Slot& s = sh.slots[(base + i) & kSlotMask];
                    std::shared_lock rl(s.mtx);
                    if (!is_dirty(s.table_tag)) { lru_victim = &s; break; }
                }
            }
        }
        target = lru_victim;
    }
    if (!target) {
        // All slots dirty — drop the new value, do not corrupt. Eviction
        // will succeed once a flush observer clears a dirty bit.
        return;
    }

    {
        std::unique_lock lk(target->mtx);
        // Hash clear before mutating fields ensures concurrent readers
        // bail out at the hash check rather than racing on key/value.
        target->hash.store(0, std::memory_order_release);
        if (target->size_bytes) {
            sh.bytes.fetch_sub(target->size_bytes, std::memory_order_relaxed);
            evictions_.fetch_add(1, std::memory_order_relaxed);
        }
        target->key.assign(key);
        target->table_tag.assign(table_tag);
        target->value      = std::move(ptr);
        target->expires_at = expires;
        target->size_bytes = bytes;
        target->last_used.store(
            sh.counter.fetch_add(1, std::memory_order_relaxed) + 1,
            std::memory_order_relaxed);
        sh.bytes.fetch_add(bytes, std::memory_order_relaxed);
        // Publish the hash last with release ordering — concurrent readers
        // that load this hash with acquire are guaranteed to see all the
        // field writes above.
        target->hash.store(h, std::memory_order_release);
    }

    if (!table_tag.empty()) {
        std::lock_guard tlk(tag_mtx_);
        auto tit = tags_.find(table_tag);
        if (tit == tags_.end()) {
            tit = tags_.emplace(std::string(table_tag),
                                std::vector<std::string>{}).first;
        }
        tit->second.emplace_back(key);
    }

    // Per-shard byte-quota enforcement. The probe-range LRU isn't enough on
    // its own — entries scatter across slots and the shard total can grow
    // unbounded even with empty probe ranges. After every put, if we're
    // over budget, walk the shard and evict the LRU non-dirty slot until
    // under quota. O(slot count) per overshoot, amortized rare.
    size_t quota = (cfg_.l2_max_bytes / kShards) + 1;
    while (sh.bytes.load(std::memory_order_relaxed) > quota) {
        Slot* victim = nullptr;
        uint64_t oldest = UINT64_MAX;
        for (auto& s : sh.slots) {
            if (s.hash.load(std::memory_order_relaxed) == 0) continue;
            if (is_dirty(s.table_tag)) continue;
            uint64_t lu = s.last_used.load(std::memory_order_relaxed);
            if (lu < oldest) { oldest = lu; victim = &s; }
        }
        if (!victim) break;          // every slot is either empty or dirty
        std::unique_lock vk(victim->mtx);
        if (victim->hash.load(std::memory_order_relaxed) == 0) continue;
        victim->hash.store(0, std::memory_order_release);
        sh.bytes.fetch_sub(victim->size_bytes, std::memory_order_relaxed);
        victim->size_bytes = 0;
        victim->value.reset();
        victim->key.clear();
        victim->table_tag.clear();
        evictions_.fetch_add(1, std::memory_order_relaxed);
    }

    t_l1.init(cfg_.l1_capacity);
}

void QueryCache::invalidate_table(std::string_view table) {
    // Pop the tag → keys list and look each key up in its shard's slot
    // table. Without the secondary tags_ map we'd have to walk every slot
    // (~65 k) per invalidation; the map keeps invalidation O(matching
    // entries) instead of O(table size).
    std::vector<std::string> keys;
    {
        std::lock_guard lk(tag_mtx_);
        auto it = tags_.find(table);
        if (it != tags_.end()) { keys = std::move(it->second); tags_.erase(it); }
    }
    for (const auto& k : keys) {
        uint64_t h = key_hash(k);
        auto&    sh = *shards_[shard_for(h)];
        size_t   base = slot_for(h);
        for (size_t i = 0; i < kProbeRange; ++i) {
            Slot& s = sh.slots[(base + i) & kSlotMask];
            if (s.hash.load(std::memory_order_acquire) != h) continue;
            std::unique_lock lk(s.mtx);
            if (s.hash.load(std::memory_order_relaxed) != h || s.key != k) continue;
            s.hash.store(0, std::memory_order_release);
            sh.bytes.fetch_sub(s.size_bytes, std::memory_order_relaxed);
            s.size_bytes = 0;
            s.value.reset();
            s.key.clear();
            s.table_tag.clear();
            invalidations_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
    // L1 isn't tag-indexed; entries naturally drop on TTL or LRU. For a hard
    // invalidation, callers can call reset_thread_l1() from the worker side.
    t_l1.clear();
}

CacheStats QueryCache::stats() const {
    CacheStats s;
    // Hot-path counters live in TLS — sum them across every thread that
    // touched the cache. Threads that have exited still have their stats
    // remembered until the process dies (we never remove from the
    // registry); this is fine for the typical worker-pool topology where
    // threads live for the process lifetime.
    s.l1_hits        = l1_hits_.load(std::memory_order_relaxed);
    s.l1_misses      = l1_misses_.load(std::memory_order_relaxed);
    s.l2_hits        = l2_hits_.load(std::memory_order_relaxed);
    s.l2_misses      = l2_misses_.load(std::memory_order_relaxed);
    s.evictions      = evictions_.load(std::memory_order_relaxed);
    s.invalidations  = invalidations_.load(std::memory_order_relaxed);
    size_t total = 0;
    for (const auto& shp : shards_) total += shp->bytes.load(std::memory_order_relaxed);
    s.bytes_used = total;
    return s;
}

void QueryCache::reset_thread_l1() { t_l1.clear(); }

void QueryCache::sweep_loop() {
    using namespace std::chrono;
    while (!stop_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(milliseconds(cfg_.sweep_interval_ms));
        if (stop_.load(std::memory_order_acquire)) return;
        auto now = steady_clock::now();
        for (auto& shp : shards_) {
            auto& sh = *shp;
            for (auto& s : sh.slots) {
                if (s.hash.load(std::memory_order_acquire) == 0) continue;
                std::unique_lock lk(s.mtx, std::try_to_lock);
                if (!lk) continue;             // contention → next sweep
                if (s.hash.load(std::memory_order_relaxed) == 0) continue;
                if (s.expires_at > now) continue;

                bool dirty;
                {
                    std::lock_guard dl(dirty_mtx_);
                    auto dit = dirty_.find(s.table_tag);
                    dirty = dit != dirty_.end() && dit->second > 0;
                }
                if (dirty) continue;            // un-flushed write — pinned

                s.hash.store(0, std::memory_order_release);
                sh.bytes.fetch_sub(s.size_bytes, std::memory_order_relaxed);
                s.size_bytes = 0;
                s.value.reset();
                s.key.clear();
                s.table_tag.clear();
                evictions_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

void QueryCache::mark_dirty(std::string_view table) {
    std::lock_guard lk(dirty_mtx_);
    auto it = dirty_.find(table);
    if (it == dirty_.end()) {
        it = dirty_.emplace(std::string(table), 0).first;
    }
    ++it->second;
}

void QueryCache::mark_clean(std::string_view table) {
    std::lock_guard lk(dirty_mtx_);
    auto it = dirty_.find(table);
    if (it == dirty_.end()) return;
    if (it->second > 0) --it->second;
    if (it->second == 0) dirty_.erase(it);
}

bool QueryCache::is_dirty(std::string_view table) const {
    std::lock_guard lk(dirty_mtx_);
    auto it = dirty_.find(table);
    return it != dirty_.end() && it->second > 0;
}

} // namespace spido_pg
