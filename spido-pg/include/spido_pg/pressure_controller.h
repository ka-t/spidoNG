#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "spido_pg/endpoint_registry.h"
#include "spido_pg/token_bucket.h"

namespace spido_pg {

class PgPool;

enum class PressureState : uint8_t {
    Healthy    = 0,
    Warm       = 1,
    Pressured  = 2,
    Overloaded = 3,
    Critical   = 4,
};

inline const char* to_string(PressureState s) noexcept {
    switch (s) {
        case PressureState::Healthy:    return "HEALTHY";
        case PressureState::Warm:       return "WARM";
        case PressureState::Pressured:  return "PRESSURED";
        case PressureState::Overloaded: return "OVERLOADED";
        case PressureState::Critical:   return "CRITICAL";
    }
    return "?";
}

struct PressureConfig {
    // Decision interval. 250 ms strikes a balance between reactivity to
    // bursts and overhead — every tick scans the registry and recomputes
    // token bucket rates.
    uint32_t decision_interval_ms = 250;

    // Latency thresholds (p95 estimate based on EWMA of pool round trips).
    // The pool also records its own checkout-latency window; the
    // controller combines them.
    double db_p95_warm_ms       = 5.0;
    double db_p95_pressured_ms  = 15.0;
    double db_p95_overloaded_ms = 30.0;
    double db_p95_critical_ms   = 100.0;

    // Pool saturation thresholds (fraction of max_conns in flight).
    double pool_saturation_warm       = 0.50;
    double pool_saturation_pressured  = 0.75;
    double pool_saturation_overloaded = 0.90;

    // PG error-rate triggers — any non-zero rate forces at least Pressured;
    // higher values escalate.
    double error_rate_pressured  = 0.01;
    double error_rate_overloaded = 0.05;
    double error_rate_critical   = 0.20;

    // Token bucket scaling per priority and pressure level. Numbers are
    // "writes per second budget per endpoint". Floors keep critical
    // endpoints alive even under CRITICAL state.
    struct PriorityBudgets {
        double critical    = 10000.0;
        double high        = 5000.0;
        double normal      = 1000.0;
        double low         = 200.0;
        double best_effort = 50.0;
    };
    // Indexed by PressureState casted to size_t.
    PriorityBudgets budgets_per_state[5] = {
        {10000.0, 5000.0, 1000.0,  200.0,   50.0},  // HEALTHY
        { 8000.0, 4000.0,  800.0,  100.0,   25.0},  // WARM
        { 6000.0, 2000.0,  400.0,   50.0,    5.0},  // PRESSURED
        { 4000.0, 1000.0,  100.0,   10.0,    1.0},  // OVERLOADED
        { 1000.0,  100.0,    0.0,    0.0,    0.0},  // CRITICAL — only critical/high survive
    };
};

// Snapshot returned by observe(). Aggregated from PgPool + BatchWriter +
// EndpointRegistry. Cheap to construct; the controller uses it both for
// its own decisions and exposes it via debug endpoints.
struct PressureSample {
    std::chrono::steady_clock::time_point ts;
    double      db_p95_latency_ms     = 0;
    double      db_avg_latency_ms     = 0;
    double      pool_saturation       = 0;
    uint32_t    pool_idle             = 0;
    uint32_t    pool_live             = 0;
    uint64_t    batch_queue_depth     = 0;
    double      error_rate            = 0;
    PressureState state               = PressureState::Healthy;
};

// Pressure controller orchestrates the system's reaction to PostgreSQL
// load. It runs a background thread that ticks every
// decision_interval_ms:
//   1. observe()  — sample pool + batch + registry
//   2. classify() — combine the inputs into a PressureState
//   3. apply()    — retune per-endpoint TokenBuckets, retune BatchWriter
//                   scale, refresh per-endpoint runtime aggregates
//
// The hot path (request handlers) never touches the controller; it just
// reads the endpoint's TokenBucket and runtime state, both updated here.
class PgPressureController {
public:
    using BatchScaleSetter = std::function<void(double scale)>;

    PgPressureController(PgPool& pool,
                         EndpointRegistry& registry,
                         PressureConfig config = {});
    ~PgPressureController();

    PgPressureController(const PgPressureController&)            = delete;
    PgPressureController& operator=(const PgPressureController&) = delete;

    void start();
    void stop();

    // Externally-recorded PG operation latency. The BatchWriter and
    // pool checkout path call this; the controller maintains an EWMA
    // and exposes p95 estimate via observe().
    void record_pg_latency(uint64_t microseconds) noexcept;
    void record_pg_error()                        noexcept;
    void record_batch_queue_depth(uint64_t total) noexcept {
        batch_queue_depth_.store(total, std::memory_order_relaxed);
    }

    // The BatchWriter exposes a scale setter so the controller can grow
    // its batch sizes under pressure without a tight coupling.
    void set_batch_scale_setter(BatchScaleSetter s) {
        std::lock_guard lk(scale_mtx_);
        batch_scale_setter_ = std::move(s);
    }

    // Snapshot the latest sample. Cheap; doesn't block the tick.
    PressureSample sample() const noexcept;
    PressureState  state()  const noexcept {
        return state_.load(std::memory_order_relaxed);
    }

    // Look up (or lazily create) the token bucket for an endpoint. The
    // controller retunes it every tick; the call site only reads it.
    TokenBucket& token_bucket_for(EndpointId id);

    const PressureConfig& config() const noexcept { return cfg_; }

private:
    void tick_loop();
    void apply(PressureState newst, const PressureSample& sample);

    PgPool&            pool_;
    EndpointRegistry&  registry_;
    PressureConfig     cfg_;

    std::atomic<PressureState> state_{PressureState::Healthy};
    std::atomic<uint64_t>      db_latency_ema_us_{0};
    std::atomic<uint64_t>      db_latency_max_us_{0};       // since last tick
    // Sticky high-water-mark for classification. Decays 30% per tick when
    // no new max arrives, so state relaxes gradually after a burst instead
    // of flapping back to HEALTHY the moment traffic pauses.
    std::atomic<uint64_t>      sticky_max_us_{0};
    std::atomic<uint64_t>      pg_errors_since_tick_{0};
    std::atomic<uint64_t>      pg_ops_since_tick_{0};
    std::atomic<uint64_t>      sticky_error_rate_pct_{0};   // bps × 100
    std::atomic<uint64_t>      batch_queue_depth_{0};

    mutable std::mutex                                  sample_mtx_;
    PressureSample                                      last_sample_;

    // Per-endpoint token buckets. Indexed by EndpointId. unique_ptr so
    // TokenBucket (atomic-bearing, non-movable) lives at a stable address.
    static constexpr size_t kMaxBuckets = EndpointRegistry::kMaxEndpoints;
    std::array<std::unique_ptr<TokenBucket>, kMaxBuckets> buckets_;
    std::mutex                                            bucket_mtx_;

    BatchScaleSetter   batch_scale_setter_;
    std::mutex         scale_mtx_;

    std::thread        thread_;
    std::atomic<bool>  stop_{false};
};

} // namespace spido_pg
