#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "spido_pg/connection.h"
#include "spido_pg/string_map.h"

namespace spido_pg {

struct CacheStats {
    uint64_t l1_hits   = 0;
    uint64_t l1_misses = 0;
    uint64_t l2_hits   = 0;
    uint64_t l2_misses = 0;
    uint64_t evictions = 0;
    uint64_t invalidations = 0;
    uint64_t bytes_used = 0;
};

struct CacheConfig {
    size_t   l1_capacity = 1024;
    size_t   l2_max_bytes = 256ull * 1024 * 1024;
    uint32_t sweep_interval_ms = 1000;
};

// SHA-256(hex) of (sql_template, params). Used as the cache key.
std::string make_cache_key(std::string_view sql,
                           std::span<const PgParam> params);

class QueryCache {
public:
    explicit QueryCache(CacheConfig cfg = {});
    ~QueryCache();

    QueryCache(const QueryCache&)            = delete;
    QueryCache& operator=(const QueryCache&) = delete;

    // Cache hits return a shared_ptr to an immutable PgResult so callers
    // don't pay the (vector-of-vector-of-string) copy on every access.
    // The cache owns one copy of each value; readers borrow via the
    // shared_ptr's atomic refcount. Callers that need a mutable copy can
    // dereference and copy — but the L1/L2 nano-bench paths and the
    // generator-emitted handlers operate on the const reference directly.
    using ValuePtr = std::shared_ptr<const PgResult>;

    // Cache miss returns a null ValuePtr. shared_ptr already has the
    // empty/non-empty distinction so wrapping in std::optional just
    // doubles the discriminator — costs a branch and ~10 ns per call.
    // Callers use `if (auto hit = cache.get(key)) { *hit; }`.
    ValuePtr get(std::string_view key);
    void                    put(std::string_view key,
                                PgResult value,
                                std::chrono::seconds ttl,
                                std::string_view table_tag);

    // Invalidate every entry tagged with `table`. O(N tagged entries).
    void invalidate_table(std::string_view table);

    // Mark a table dirty. Entries with this tag will not be evicted by the
    // LRU walker or the TTL sweep until mark_clean() is called for the
    // table. Writers (BatchWriter, write_sync) flip this on enqueue.
    void mark_dirty(std::string_view table);
    void mark_clean(std::string_view table);
    bool is_dirty (std::string_view table) const;

    CacheStats stats() const;

    // Used in tests / hot path to verify L1 actually saved an L2 trip.
    void reset_thread_l1();

private:
    // L2 storage. Per-shard open-addressed hash table:
    //   - hash lookup is fully lock-free (atomic load of slot.hash); on a
    //     miss the reader returns without ever touching a mutex.
    //   - on a hash match the reader takes a per-slot shared_mutex to copy
    //     the value out (cheap — ~10 ns uncontended on x86), still no
    //     shard-wide lock. Concurrent readers don't block each other.
    //   - writers take the slot's unique_lock; one writer per slot at a
    //     time. Across slots writes scale linearly.
    //   - LRU is approximate: each slot tracks last_used (atomic, relaxed);
    //     puts that need to evict scan the probe range and replace the
    //     smallest. Globally-perfect LRU isn't worth the centralized
    //     structure cost.
    //   - bytes_ is per-shard, written under the slot's writer lock.
    struct alignas(64) Slot {
        // hash != 0 ⇒ slot occupied. 0 reserved for empty / in-flight wipe.
        std::atomic<uint64_t>                                   hash{0};
        mutable std::shared_mutex                               mtx;
        std::string                                             key;
        std::string                                             table_tag;
        ValuePtr                                                value;
        std::chrono::steady_clock::time_point                   expires_at;
        size_t                                                  size_bytes = 0;
        // Relaxed counter — coarse LRU only.
        std::atomic<uint64_t>                                   last_used{0};
    };

    static constexpr size_t kShards        = 64;
    static constexpr size_t kSlotsPerShard = 1024;   // power of two — used as bitmask
    static constexpr size_t kSlotMask      = kSlotsPerShard - 1;
    static constexpr size_t kProbeRange    = 16;     // chain length on collision

    struct Shard {
        std::array<Slot, kSlotsPerShard>  slots;
        std::atomic<size_t>               bytes{0};
        std::atomic<uint64_t>             counter{0};   // monotonically increasing
        std::mutex                        evict_mtx;    // serializes eviction scans
    };

    static size_t shard_for(uint64_t h) noexcept { return (h >> 32) & (kShards - 1); }
    static size_t slot_for (uint64_t h) noexcept { return h & kSlotMask; }
    static uint64_t key_hash(std::string_view key) noexcept;

    void sweep_loop();

    CacheConfig                            cfg_;
    std::array<std::unique_ptr<Shard>, kShards>  shards_;

    // Tag → set of keys that carry it. Guarded by tag_mtx_.
    std::mutex                                                  tag_mtx_;
    StringMap<std::vector<std::string>>                         tags_;

    // Tags with at least one un-flushed write. Eviction (LRU+TTL) skips
    // entries whose tag is in here, and put() refuses to evict its own
    // entries when over-quota — instead it grows the shard temporarily.
    // A BatchWriter flush observer clears the bit when PG ack's the batch.
    mutable std::mutex                                          dirty_mtx_;
    StringMap<uint64_t>                                         dirty_;     // tag → in-flight count

    std::thread                       sweep_thread_;
    std::atomic<bool>                 stop_{false};

    mutable std::atomic<uint64_t>     l1_hits_{0};
    mutable std::atomic<uint64_t>     l1_misses_{0};
    mutable std::atomic<uint64_t>     l2_hits_{0};
    mutable std::atomic<uint64_t>     l2_misses_{0};
    mutable std::atomic<uint64_t>     evictions_{0};
    mutable std::atomic<uint64_t>     invalidations_{0};
};

} // namespace spido_pg
