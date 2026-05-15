#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace spido_pg {

// 32-bit numeric handle. The generator assigns one per resource at code-gen
// time, so all hot-path lookups (policy fetch, runtime-state bump) are
// O(1) array indexing — never a string hash. EndpointId(0) is reserved as
// "unknown / not classified".
using EndpointId = uint32_t;
constexpr EndpointId kEndpointUnknown = 0;

using TableId = uint32_t;
constexpr TableId kTableUnknown = 0;

// Data model decides whether duplicate-pk writes in the same batch should be
// folded into one (State) or preserved (Event). The BatchWriter uses this to
// pick between UPSERT-with-version and plain multi-row INSERT.
enum class DataModel : uint8_t {
    State = 0,    // device_status, user_profile, settings  → coalesce by pk
    Event = 1,    // logs, payments, audits                  → preserve every row
};

enum class Priority : uint8_t {
    Critical   = 0,
    High       = 1,
    Normal     = 2,
    Low        = 3,
    BestEffort = 4,
};

enum class ReadMode : uint8_t {
    DirectDb              = 0,
    CacheFirst            = 1,
    CacheOnlyWhenPressure = 2,
    StaleCacheAllowed     = 3,
    RamEntityFirst        = 4,
};

enum class WriteMode : uint8_t {
    Sync          = 0,
    AsyncDurable  = 1,
    BatchDurable  = 2,
    AsyncMemory   = 3,
    BatchMemory   = 4,
    Disabled      = 5,
};

enum class ConsistencyLevel : uint8_t {
    Strict          = 0,
    ReadYourWrites  = 1,
    EventualDurable = 2,
    EventualMemory  = 3,
    BestEffort      = 4,
};

enum class OverloadBehavior : uint8_t {
    Delay         = 0,
    Queue         = 1,
    Return202     = 2,
    Return429     = 3,
    Return503     = 4,
    StaleCache    = 5,
    DropBestEffort= 6,
    BatchOnly     = 7,
    WalOnly       = 8,
};

// Static, immutable per-endpoint configuration. The generator emits one
// instance per resource into `generated_policies.h`. Fields default to
// safe values; missing config keys → defaults below.
//
// Note on string fields: we keep them as `std::string` for now (set once
// at startup, read-only afterwards). If the 10k-endpoint case shows
// allocator pressure we'll switch to interned ids backed by an arena.
struct EndpointPolicy {
    EndpointId      endpoint_id = kEndpointUnknown;
    TableId         table_id    = kTableUnknown;
    std::string     path;                  // "/users/:id"
    std::string     method;                // "GET" / "POST" / ...
    std::string     table_name;
    std::string     primary_key   = "id";
    std::vector<std::string> columns;

    DataModel        data_model        = DataModel::State;
    Priority         priority          = Priority::Normal;
    ReadMode         read_mode         = ReadMode::CacheFirst;
    WriteMode        write_mode        = WriteMode::Sync;
    ConsistencyLevel consistency_level = ConsistencyLevel::Strict;

    uint32_t  cache_ttl_s                = 5;
    uint32_t  stale_while_revalidate_s   = 0;
    uint32_t  hotset_size                = 0;     // Faz 3 honours this
    std::string preload_query;                    // Faz 3 honours this

    uint32_t  batch_size_min             = 100;
    uint32_t  batch_size_default         = 500;
    uint32_t  batch_size_max             = 5000;
    uint32_t  flush_interval_min_ms      = 5;
    uint32_t  flush_interval_default_ms  = 50;
    uint32_t  flush_interval_max_ms      = 2000;
    uint32_t  max_queue_depth            = 100000;

    OverloadBehavior overload_behavior   = OverloadBehavior::Return503;

    // Knobs that toggle policy permissions. Defaults are conservative — the
    // generator flips them based on config.json.
    bool allow_coalescing = false;     // overrides DataModel for special cases
    bool allow_stale      = false;
    bool allow_drop       = false;
    bool allow_memory_only= false;
    bool require_wal      = false;
    bool require_pg_confirm = false;
    bool pin_hotset       = false;
    bool auth             = false;
};

// Per-endpoint runtime state. Hot-path code (handlers, batch writer) bumps
// counters; the pressure controller reads aggregates. We use relaxed
// atomics because we don't need any cross-counter ordering — the worst
// outcome of a torn read is a slightly stale rate estimate, which the
// EWMA smoothing in the pressure loop hides.
//
// Each state struct is cache-line aligned to keep counters from adjacent
// endpoints off the same cache line under burst load.
struct alignas(64) EndpointRuntimeState {
    EndpointId  endpoint_id = kEndpointUnknown;

    // Request counters since process start.
    std::atomic<uint64_t> total_requests{0};
    std::atomic<uint64_t> reads{0};
    std::atomic<uint64_t> writes{0};

    // Window counters reset every pressure tick — the controller reads then
    // exchanges them with 0 atomically to derive an instantaneous rate.
    std::atomic<uint64_t> reads_in_window{0};
    std::atomic<uint64_t> writes_in_window{0};

    // Microsecond accumulators for latency. We track sum + count and let
    // the aggregator compute mean; a real p95/p99 estimator (CKMS,
    // t-digest) is deferred to Faz 4 — for now the controller relies on
    // mean + max which is enough for state classification.
    std::atomic<uint64_t> latency_us_sum{0};
    std::atomic<uint64_t> latency_us_count{0};
    std::atomic<uint64_t> latency_us_max{0};

    // Cache & overload counters.
    std::atomic<uint64_t> cache_hits{0};
    std::atomic<uint64_t> cache_misses{0};
    std::atomic<uint64_t> stale_served{0};
    std::atomic<uint64_t> queue_depth{0};
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint64_t> delayed{0};
    std::atomic<uint64_t> rejected{0};
    std::atomic<uint64_t> errors{0};

    // Smoothed rates (updated by controller, read by handlers via metrics).
    std::atomic<double> rps{0.0};
    std::atomic<double> read_rps{0.0};
    std::atomic<double> write_rps{0.0};
    std::atomic<double> avg_latency_us{0.0};

    void record_read(uint64_t latency_us) noexcept {
        total_requests.fetch_add(1, std::memory_order_relaxed);
        reads.fetch_add(1,           std::memory_order_relaxed);
        reads_in_window.fetch_add(1, std::memory_order_relaxed);
        record_latency(latency_us);
    }
    void record_write(uint64_t latency_us) noexcept {
        total_requests.fetch_add(1, std::memory_order_relaxed);
        writes.fetch_add(1,           std::memory_order_relaxed);
        writes_in_window.fetch_add(1, std::memory_order_relaxed);
        record_latency(latency_us);
    }
    void record_latency(uint64_t latency_us) noexcept {
        latency_us_sum.fetch_add(latency_us, std::memory_order_relaxed);
        latency_us_count.fetch_add(1,         std::memory_order_relaxed);
        uint64_t cur = latency_us_max.load(std::memory_order_relaxed);
        while (latency_us > cur &&
               !latency_us_max.compare_exchange_weak(cur, latency_us,
                                                     std::memory_order_relaxed)) {}
    }
};

// Registry of all known endpoints. The generator calls register_policy()
// at startup with each EndpointPolicy; runtime state is allocated lazily
// on first record_* call. Lookups are O(1) array indexing — we store
// policies and runtime state in a fixed-size flat array, indexed by
// EndpointId minus 1 (since 0 is "unknown").
//
// Capacity is bounded at kMaxEndpoints; if the generator ever ships more
// than that we'll grow the bound — the per-endpoint memory overhead is
// modest (~512 bytes counters + ~500 bytes policy).
class EndpointRegistry {
public:
    static constexpr size_t kMaxEndpoints = 16384;

    EndpointRegistry();

    // Register or replace a policy. The generator calls this once per
    // resource at startup; runtime registration after handlers start
    // serving is supported but should be rare.
    void register_policy(EndpointPolicy policy);

    // Null if unknown. Pointer is stable for the lifetime of the registry.
    const EndpointPolicy*    policy(EndpointId id) const noexcept;
    EndpointRuntimeState&    state (EndpointId id);

    // Iterate every registered endpoint. Snapshot under lock; the
    // returned vector is a copy of pointers so the caller can read at
    // leisure without holding any lock.
    std::vector<EndpointId>  all_ids() const;
    size_t                   count() const noexcept;

    // Approximate top-K hot endpoints by write_rps / read_rps. Used by
    // the pressure controller for targeted throttling — we don't need
    // exact ordering, just "these are the loud ones". O(N log K).
    struct HotEntry { EndpointId id; double rps; };
    std::vector<HotEntry> top_k_writers(size_t k) const;
    std::vector<HotEntry> top_k_readers(size_t k) const;

private:
    struct Slot {
        std::atomic<bool>                       occupied{false};
        std::unique_ptr<EndpointPolicy>         policy;
        std::unique_ptr<EndpointRuntimeState>   state;
    };

    static size_t index_of(EndpointId id) noexcept { return id - 1; }

    mutable std::mutex          register_mtx_;  // guards register_policy
    std::array<Slot, kMaxEndpoints> slots_;
    std::atomic<size_t>         registered_{0};
};

} // namespace spido_pg
