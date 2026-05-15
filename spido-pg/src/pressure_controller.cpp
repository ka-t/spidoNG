#include "spido_pg/pressure_controller.h"
#include "spido_pg/pool.h"

#include <algorithm>
#include <cmath>

namespace spido_pg {

PgPressureController::PgPressureController(PgPool& pool,
                                           EndpointRegistry& registry,
                                           PressureConfig config)
    : pool_(pool), registry_(registry), cfg_(std::move(config))
{
    last_sample_.ts = std::chrono::steady_clock::now();
}

PgPressureController::~PgPressureController() { stop(); }

void PgPressureController::start() {
    if (thread_.joinable()) return;
    stop_.store(false, std::memory_order_release);
    thread_ = std::thread([this]{ tick_loop(); });
}

void PgPressureController::stop() {
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
}

void PgPressureController::record_pg_latency(uint64_t us) noexcept {
    // EWMA with alpha=1/64 — ~1-second smoothing at the kind of write
    // rate we expect (~ms-scale ticks). Cheap exponential moving average
    // computed entirely in integer microseconds.
    uint64_t prev = db_latency_ema_us_.load(std::memory_order_relaxed);
    uint64_t next = (prev * 63 + us) / 64;
    db_latency_ema_us_.store(next, std::memory_order_relaxed);

    uint64_t cur_max = db_latency_max_us_.load(std::memory_order_relaxed);
    while (us > cur_max &&
           !db_latency_max_us_.compare_exchange_weak(cur_max, us,
                                                     std::memory_order_relaxed)) {}
    pg_ops_since_tick_.fetch_add(1, std::memory_order_relaxed);
}

void PgPressureController::record_pg_error() noexcept {
    pg_errors_since_tick_.fetch_add(1, std::memory_order_relaxed);
}

TokenBucket& PgPressureController::token_bucket_for(EndpointId id) {
    if (id == kEndpointUnknown || id > kMaxBuckets) {
        // Fallback singleton bucket for unknown endpoints. Configured
        // permissively so unrouted-but-registered handlers still work;
        // the runtime never falls back here in well-generated services.
        static TokenBucket unknown(1000.0, 1000.0);
        return unknown;
    }
    size_t idx = id - 1;
    if (!buckets_[idx]) {
        std::lock_guard lk(bucket_mtx_);
        if (!buckets_[idx]) {
            // Initial budget: Normal priority under HEALTHY state. The
            // first apply() tick will overwrite this with the
            // priority-aware value.
            buckets_[idx] = std::make_unique<TokenBucket>(
                cfg_.budgets_per_state[0].normal,
                cfg_.budgets_per_state[0].normal);
        }
    }
    return *buckets_[idx];
}

PressureSample PgPressureController::sample() const noexcept {
    std::lock_guard lk(sample_mtx_);
    return last_sample_;
}

void PgPressureController::tick_loop() {
    using namespace std::chrono;
    auto tick = milliseconds(cfg_.decision_interval_ms);
    while (!stop_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(tick);
        if (stop_.load(std::memory_order_acquire)) return;

        // ---- Observe ----
        PressureSample s;
        s.ts = steady_clock::now();

        // EMA-derived avg; for p95 we use the per-tick max as a stand-in.
        // A real online-quantile estimator (CKMS / t-digest) is deferred
        // to Faz 4 — until then the max-since-last-tick is a good proxy
        // for "tail latency right now".
        uint64_t ema_us = db_latency_ema_us_.load(std::memory_order_relaxed);
        uint64_t max_us = db_latency_max_us_.exchange(0,
            std::memory_order_relaxed);
        // Sticky high-water-mark: take max of current-tick max and a 70%
        // decayed previous sticky. This gives us a "p95-ish" number that
        // captures bursts within one tick and relaxes over ~10 ticks once
        // the burst stops. Real p95 (CKMS/t-digest) is Faz 4.
        uint64_t prev_sticky = sticky_max_us_.load(std::memory_order_relaxed);
        uint64_t new_sticky  = std::max(max_us, prev_sticky * 7 / 10);
        sticky_max_us_.store(new_sticky, std::memory_order_relaxed);
        s.db_avg_latency_ms = ema_us / 1000.0;
        s.db_p95_latency_ms = new_sticky / 1000.0;

        s.pool_live     = pool_.live_conns();
        s.pool_idle     = pool_.idle_conns();
        uint32_t max_c  = pool_.config().max_conns;
        s.pool_saturation = max_c > 0
            ? static_cast<double>(s.pool_live - s.pool_idle) / max_c
            : 0.0;

        s.batch_queue_depth = batch_queue_depth_.load(std::memory_order_relaxed);

        uint64_t errors = pg_errors_since_tick_.exchange(0,
            std::memory_order_relaxed);
        uint64_t ops    = pg_ops_since_tick_.exchange(0,
            std::memory_order_relaxed);
        double tick_rate = ops > 0 ? static_cast<double>(errors) / ops : 0.0;
        // Sticky error rate: same idea as the latency high-water-mark.
        // Stored as basis points × 100 (so 0.05 → 500) inside a uint64
        // so we can keep the atomic plain integer.
        uint64_t prev_err_sticky = sticky_error_rate_pct_.load(std::memory_order_relaxed);
        uint64_t cur_err_pct = static_cast<uint64_t>(tick_rate * 10000.0);
        uint64_t new_err_sticky = std::max(cur_err_pct, prev_err_sticky * 7 / 10);
        sticky_error_rate_pct_.store(new_err_sticky, std::memory_order_relaxed);
        s.error_rate = new_err_sticky / 10000.0;

        // ---- Classify ----
        // Take the most pessimistic signal across latency, saturation,
        // and error rate.
        PressureState st = PressureState::Healthy;
        auto upgrade = [&](PressureState target) {
            if (static_cast<int>(target) > static_cast<int>(st)) st = target;
        };
        if (s.db_p95_latency_ms >= cfg_.db_p95_warm_ms)       upgrade(PressureState::Warm);
        if (s.db_p95_latency_ms >= cfg_.db_p95_pressured_ms)  upgrade(PressureState::Pressured);
        if (s.db_p95_latency_ms >= cfg_.db_p95_overloaded_ms) upgrade(PressureState::Overloaded);
        if (s.db_p95_latency_ms >= cfg_.db_p95_critical_ms)   upgrade(PressureState::Critical);

        if (s.pool_saturation >= cfg_.pool_saturation_warm)        upgrade(PressureState::Warm);
        if (s.pool_saturation >= cfg_.pool_saturation_pressured)   upgrade(PressureState::Pressured);
        if (s.pool_saturation >= cfg_.pool_saturation_overloaded)  upgrade(PressureState::Overloaded);

        if (s.error_rate >= cfg_.error_rate_pressured)  upgrade(PressureState::Pressured);
        if (s.error_rate >= cfg_.error_rate_overloaded) upgrade(PressureState::Overloaded);
        if (s.error_rate >= cfg_.error_rate_critical)   upgrade(PressureState::Critical);

        s.state = st;

        {
            std::lock_guard lk(sample_mtx_);
            last_sample_ = s;
        }
        PressureState old = state_.exchange(st, std::memory_order_relaxed);
        (void)old;  // hooks for state-change callbacks live in Faz 4

        // ---- Apply ----
        apply(st, s);
    }
}

void PgPressureController::apply(PressureState st, const PressureSample& s) {
    const auto& b = cfg_.budgets_per_state[static_cast<size_t>(st)];

    // 1. Retune every registered endpoint's token bucket according to its
    //    priority. Capacity = 1 second of rate so a brief lull lets the
    //    bucket refill enough for a small burst without runaway carry.
    for (EndpointId id : registry_.all_ids()) {
        const auto* p = registry_.policy(id);
        if (!p) continue;
        double rate = b.normal;
        switch (p->priority) {
            case Priority::Critical:   rate = b.critical;    break;
            case Priority::High:       rate = b.high;        break;
            case Priority::Normal:     rate = b.normal;      break;
            case Priority::Low:        rate = b.low;         break;
            case Priority::BestEffort: rate = b.best_effort; break;
        }
        token_bucket_for(id).reset(rate, rate);

        // 2. Roll per-endpoint window counters into smoothed rates so
        //    the metrics endpoint (and Faz 4 top-K heuristics) have a
        //    fresh view. Window = decision_interval, so dividing by
        //    interval/1000 gives ops-per-second.
        auto& rs = registry_.state(id);
        double dt = cfg_.decision_interval_ms / 1000.0;
        if (dt > 0) {
            uint64_t r = rs.reads_in_window.exchange(0,  std::memory_order_relaxed);
            uint64_t w = rs.writes_in_window.exchange(0, std::memory_order_relaxed);
            double new_read_rps  = r / dt;
            double new_write_rps = w / dt;
            // EWMA smoothing on top of the window snapshot so a single
            // bursty tick doesn't whip the rate.
            double prev_read  = rs.read_rps.load(std::memory_order_relaxed);
            double prev_write = rs.write_rps.load(std::memory_order_relaxed);
            rs.read_rps.store ( 0.5 * prev_read  + 0.5 * new_read_rps,
                                std::memory_order_relaxed);
            rs.write_rps.store( 0.5 * prev_write + 0.5 * new_write_rps,
                                std::memory_order_relaxed);
            rs.rps.store(rs.read_rps.load(std::memory_order_relaxed)
                       + rs.write_rps.load(std::memory_order_relaxed),
                         std::memory_order_relaxed);
        }
        uint64_t sum = rs.latency_us_sum.exchange(0,   std::memory_order_relaxed);
        uint64_t cnt = rs.latency_us_count.exchange(0, std::memory_order_relaxed);
        if (cnt > 0) {
            rs.avg_latency_us.store(static_cast<double>(sum) / cnt,
                                    std::memory_order_relaxed);
        }
    }

    // 3. Tell the BatchWriter to scale its batch sizes. Larger batches
    //    under pressure amortize per-flush PG cost; smaller batches when
    //    healthy keep tail latency low.
    double batch_scale = 1.0;
    switch (st) {
        case PressureState::Healthy:    batch_scale = 1.0;  break;
        case PressureState::Warm:       batch_scale = 1.5;  break;
        case PressureState::Pressured:  batch_scale = 2.5;  break;
        case PressureState::Overloaded: batch_scale = 4.0;  break;
        case PressureState::Critical:   batch_scale = 6.0;  break;
    }
    BatchScaleSetter setter;
    {
        std::lock_guard lk(scale_mtx_);
        setter = batch_scale_setter_;
    }
    if (setter) setter(batch_scale);

    (void)s;  // s reserved for future signals (we already pulled what we need)
}

} // namespace spido_pg
