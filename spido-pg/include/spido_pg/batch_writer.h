#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "spido_pg/connection.h"
#include "spido_pg/endpoint_registry.h"
#include "spido_pg/string_map.h"

namespace spido_pg {

class PgPool;

// One table's batch configuration. The flusher thread reads queue tail,
// builds a multi-row INSERT, and ships it through the pool. Sizes here are
// *baseline* — the dynamic priority controller scales them up/down each
// 250ms based on observed load.
struct BatchTableConfig {
    std::vector<std::string> columns;
    std::string              primary_key;
    uint32_t                 batch_size        = 500;
    uint32_t                 flush_interval_ms = 10;
    uint32_t                 max_retry         = 3;
    bool                     upsert            = false;

    // Faz 1: data model decides whether duplicate-pk writes in the same
    // flush get coalesced (State → keep last) or preserved (Event →
    // every row makes it to PG). Defaults to State to match the
    // previous behaviour for callers that don't set it.
    DataModel                data_model        = DataModel::State;

    // Index into `columns` of the primary key, computed by BatchWriter
    // ctor. State coalescing uses this; -1 means "no pk in row data".
    int                      pk_index          = -1;

    // Index of an optional `version` column. When present (data_model =
    // State + columns includes "version"), the generated UPSERT adds a
    // `WHERE table.version <= EXCLUDED.version` guard so a stale write
    // can never overwrite a newer one. -1 means no version column.
    int                      version_index     = -1;
};

struct BatchWriterConfig {
    std::unordered_map<std::string, BatchTableConfig> tables;
    std::string                                       dlq_suffix = "_dlq";

    // ---- Dynamic priority knobs ----
    // Tick interval for the rebalance / load loop.
    uint32_t controller_tick_ms = 250;
    // PG round-trip threshold above which we throttle (back off, bigger
    // batches). Below threshold/2 we tighten (smaller batches, faster flush).
    uint32_t pg_slow_threshold_ms = 10;
    // Bounds for adaptive scaling — never shrink below min, never grow past
    // max. Scaling is multiplicative around the baseline batch_size.
    double   scale_min = 0.25;
    double   scale_max = 4.0;
};

// Optional flush observer — invoked by the flusher when a batch has been
// successfully sent (or DLQ'd on terminal failure). The cache layer uses
// this to clear its dirty bit for tagged keys, so that writes acknowledged
// by PG can be evicted from cache normally.
using FlushObserver =
    std::function<void(std::string_view table, size_t rows, bool ok)>;

class BatchWriter {
public:
    BatchWriter(PgPool& pool, BatchWriterConfig cfg);
    ~BatchWriter();

    BatchWriter(const BatchWriter&)            = delete;
    BatchWriter& operator=(const BatchWriter&) = delete;

    // Enqueue a row for `table`. `endpoint` is the routing key that
    // groups requests for the dynamic-priority controller — typically the
    // HTTP path that produced this row (e.g. "/orders"). row.size() must
    // equal cfg.tables[table].columns.size().
    void enqueue(std::string_view endpoint,
                 std::string_view table,
                 std::vector<PgParam> row);

    // Block until the named table's queue is drained, or all if name empty.
    void flush_now(std::string_view table = {});

    // Subscribe a flush observer. Single observer; later calls replace.
    void set_flush_observer(FlushObserver obs);

    // External scale knob — the PressureController calls this every tick
    // to scale batch sizes up under pressure (bigger batches amortize
    // PG round-trip cost) or down when PG is healthy (smaller batches
    // keep latency low). Stacks multiplicatively with the internal
    // PG-load-driven dyn_scale_.
    void set_pressure_scale(double scale) noexcept {
        external_scale_.store(scale, std::memory_order_relaxed);
    }

    // Sum of queued rows across every table. Read by the PressureController
    // and exposed via metrics.
    uint64_t total_queue_depth() const noexcept;

    struct Stats {
        uint64_t flushed             = 0;
        uint64_t queued              = 0;
        uint64_t failed              = 0;
        uint64_t current_batch_size  = 0;   // post-scaling
        uint64_t pending_pg_avg_us   = 0;
    };
    Stats stats(std::string_view table) const;

    // Endpoint-level load snapshot — exposed for tests and metrics
    // endpoints. Endpoints that haven't seen traffic in the last window
    // are omitted.
    struct EndpointLoad {
        std::string endpoint;
        double      rps;
        double      share;            // 0..1 fraction of total observed rps
        uint32_t    write_slots;      // current allocation
    };
    std::vector<EndpointLoad> endpoint_loads() const;

private:
    struct TableState {
        BatchTableConfig                                  cfg;
        std::mutex                                        mtx;
        std::condition_variable                           cv;
        std::deque<std::pair<std::string,
                             std::vector<PgParam>>>       queue;  // (endpoint, row)
        std::thread                                       flusher;
        std::atomic<uint64_t>                             flushed{0};
        std::atomic<uint64_t>                             failed{0};
        std::atomic<uint32_t>                             dyn_batch_size{0};
        std::atomic<uint32_t>                             dyn_flush_ms{0};
        bool                                              flush_request = false;
        bool                                              stop = false;
        std::condition_variable                           drain_cv;
    };

    struct EndpointStats {
        std::atomic<uint64_t> in_window{0};
        std::atomic<uint32_t> slots{1};       // current allocation
        double                last_rps = 0;
    };

    void flusher_loop(std::string table, TableState* s);
    bool send_batch(const std::string& table,
                    TableState* s,
                    std::vector<std::vector<PgParam>>& rows);
    void controller_loop();
    void rebalance();

    PgPool&                                                          pool_;
    BatchWriterConfig                                                cfg_;
    StringMap<std::unique_ptr<TableState>>                           states_;

    // Endpoint load tracking — indexed by endpoint string. The controller
    // reads + resets these atomics each tick.
    mutable std::mutex                                               ep_mtx_;
    StringMap<std::unique_ptr<EndpointStats>>                        endpoints_;

    // PG load detector. Each successful query measures its wall time and
    // contributes to a rolling EMA (1024-sample window approximation via
    // alpha = 1/1024). The controller reads pg_avg_us_ to decide whether
    // to scale batches up (PG slow) or down (PG fast).
    std::atomic<uint64_t>                                            pg_avg_us_{0};
    std::atomic<double>                                              dyn_scale_{1.0};
    std::atomic<double>                                              external_scale_{1.0};

    std::thread                                                      controller_;
    std::atomic<bool>                                                stop_{false};

    std::mutex                                                       obs_mtx_;
    FlushObserver                                                    observer_;
};

} // namespace spido_pg
