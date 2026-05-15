#include "spido_pg/batch_writer.h"
#include "spido_pg/pool.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

namespace spido_pg {

namespace {

// "(${a},${b},...,${z})" placeholder group, advancing the parameter index.
std::string placeholder_group(size_t cols, size_t& next_idx) {
    std::string s; s.reserve(cols * 5 + 2);
    s.push_back('(');
    for (size_t i = 0; i < cols; ++i) {
        if (i) s.push_back(',');
        char buf[16]; std::snprintf(buf, sizeof buf, "$%zu", next_idx++);
        s.append(buf);
    }
    s.push_back(')');
    return s;
}

std::string join_csv(const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) { if (i) s.push_back(','); s.append(v[i]); }
    return s;
}

} // namespace

BatchWriter::BatchWriter(PgPool& pool, BatchWriterConfig cfg)
    : pool_(pool), cfg_(std::move(cfg))
{
    for (auto& [table, tcfg] : cfg_.tables) {
        auto s = std::make_unique<TableState>();
        s->cfg = tcfg;

        // Compute column-index hints. Faster than a string compare every
        // flush, and lets us emit a correct UPSERT WHERE version guard
        // for State tables that include a "version" column.
        for (size_t i = 0; i < s->cfg.columns.size(); ++i) {
            if (s->cfg.columns[i] == s->cfg.primary_key) s->cfg.pk_index      = static_cast<int>(i);
            if (s->cfg.columns[i] == "version")          s->cfg.version_index = static_cast<int>(i);
        }
        // State tables imply upsert semantics — the whole point of the
        // model is "latest write wins for this pk". Honour explicit
        // upsert=false only for Event tables.
        if (s->cfg.data_model == DataModel::State) s->cfg.upsert = true;

        s->dyn_batch_size.store(tcfg.batch_size, std::memory_order_relaxed);
        s->dyn_flush_ms.store(tcfg.flush_interval_ms, std::memory_order_relaxed);
        TableState* raw = s.get();
        std::string table_copy = table;
        s->flusher = std::thread([this, table_copy, raw]{ flusher_loop(table_copy, raw); });
        states_.emplace(table, std::move(s));
    }
    controller_ = std::thread([this]{ controller_loop(); });
}

BatchWriter::~BatchWriter() {
    stop_.store(true, std::memory_order_release);
    for (auto& [_, s] : states_) {
        {
            std::lock_guard lk(s->mtx);
            s->stop = true;
        }
        s->cv.notify_all();
    }
    for (auto& [_, s] : states_) {
        if (s->flusher.joinable()) s->flusher.join();
    }
    if (controller_.joinable()) controller_.join();
}

void BatchWriter::set_flush_observer(FlushObserver obs) {
    std::lock_guard lk(obs_mtx_);
    observer_ = std::move(obs);
}

void BatchWriter::enqueue(std::string_view endpoint,
                          std::string_view table,
                          std::vector<PgParam> row)
{
    auto it = states_.find(table);
    if (it == states_.end()) return;
    auto* s = it->second.get();

    // Endpoint counter — single atomic increment, no allocation if the
    // endpoint is already registered.
    {
        std::lock_guard lk(ep_mtx_);
        auto eit = endpoints_.find(endpoint);
        if (eit == endpoints_.end()) {
            eit = endpoints_.emplace(std::string(endpoint),
                                     std::make_unique<EndpointStats>()).first;
        }
        eit->second->in_window.fetch_add(1, std::memory_order_relaxed);
    }

    bool wake = false;
    {
        std::lock_guard lk(s->mtx);
        s->queue.emplace_back(std::string(endpoint), std::move(row));
        if (s->queue.size() >= s->dyn_batch_size.load(std::memory_order_relaxed)) {
            s->flush_request = true; wake = true;
        }
    }
    if (wake) s->cv.notify_one();
}

void BatchWriter::flush_now(std::string_view table) {
    auto wait_one = [](TableState* s) {
        std::unique_lock lk(s->mtx);
        s->flush_request = true;
        s->cv.notify_one();
        s->drain_cv.wait(lk, [&]{ return s->queue.empty() || s->stop; });
    };
    if (table.empty()) {
        for (auto& [_, s] : states_) wait_one(s.get());
    } else {
        auto it = states_.find(table);
        if (it != states_.end()) wait_one(it->second.get());
    }
}

uint64_t BatchWriter::total_queue_depth() const noexcept {
    uint64_t total = 0;
    for (const auto& [_, s] : states_) {
        std::lock_guard lk(s->mtx);
        total += s->queue.size();
    }
    return total;
}

BatchWriter::Stats BatchWriter::stats(std::string_view table) const {
    auto it = states_.find(table);
    if (it == states_.end()) return {};
    auto* s = it->second.get();
    Stats st;
    st.flushed = s->flushed.load(std::memory_order_relaxed);
    st.failed  = s->failed.load(std::memory_order_relaxed);
    st.current_batch_size = s->dyn_batch_size.load(std::memory_order_relaxed);
    st.pending_pg_avg_us  = pg_avg_us_.load(std::memory_order_relaxed);
    {
        std::lock_guard lk(s->mtx);
        st.queued = s->queue.size();
    }
    return st;
}

std::vector<BatchWriter::EndpointLoad> BatchWriter::endpoint_loads() const {
    std::vector<EndpointLoad> out;
    std::lock_guard lk(ep_mtx_);
    out.reserve(endpoints_.size());
    double total = 0;
    for (const auto& [_, st] : endpoints_) total += st->last_rps;
    for (const auto& [name, st] : endpoints_) {
        if (st->last_rps <= 0) continue;
        EndpointLoad e;
        e.endpoint = name;
        e.rps      = st->last_rps;
        e.share    = total > 0 ? st->last_rps / total : 0;
        e.write_slots = st->slots.load(std::memory_order_relaxed);
        out.push_back(std::move(e));
    }
    return out;
}

bool BatchWriter::send_batch(const std::string& table,
                             TableState* s,
                             std::vector<std::vector<PgParam>>& rows)
{
    if (rows.empty()) return true;
    size_t cols = s->cfg.columns.size();

    // State-table coalescing. When the same primary key appears multiple
    // times in this flush window we collapse to the newest entry, because
    // the data model promises "latest state wins". Event tables skip this
    // (every row preserved).
    if (s->cfg.data_model == DataModel::State && s->cfg.pk_index >= 0) {
        // Walk forward; remember the index of the last row seen per pk.
        // We do this in-place by marking earlier duplicates as null-rows
        // then erase-remove. A hashmap keyed on the raw byte payload of
        // the pk param keeps it O(N).
        std::unordered_map<std::string, size_t> last_idx;
        last_idx.reserve(rows.size());
        for (size_t i = 0; i < rows.size(); ++i) {
            const auto& pk = rows[i][s->cfg.pk_index].value;
            last_idx[pk] = i;
        }
        if (last_idx.size() < rows.size()) {
            std::vector<std::vector<PgParam>> coalesced;
            coalesced.reserve(last_idx.size());
            for (size_t i = 0; i < rows.size(); ++i) {
                const auto& pk = rows[i][s->cfg.pk_index].value;
                if (last_idx[pk] == i) coalesced.push_back(std::move(rows[i]));
            }
            rows = std::move(coalesced);
        }
    }

    std::string sql;
    sql.reserve(64 + table.size() + cols * 16 + rows.size() * cols * 6);
    sql.append("INSERT INTO ").append(table).append(" (")
       .append(join_csv(s->cfg.columns)).append(") VALUES ");
    size_t next = 1;
    for (size_t r = 0; r < rows.size(); ++r) {
        if (r) sql.push_back(',');
        sql.append(placeholder_group(cols, next));
    }
    if (s->cfg.upsert && !s->cfg.primary_key.empty()) {
        sql.append(" ON CONFLICT (").append(s->cfg.primary_key).append(") DO UPDATE SET ");
        bool first = true;
        for (const auto& c : s->cfg.columns) {
            if (c == s->cfg.primary_key) continue;
            if (!first) sql.push_back(',');
            first = false;
            sql.append(c).append("=EXCLUDED.").append(c);
        }
        // Version guard: never overwrite a newer row. Requires both
        // a "version" column on the table and that the incoming row
        // carries the version param at version_index.
        if (s->cfg.version_index >= 0) {
            sql.append(" WHERE ").append(table).append(".version <= EXCLUDED.version");
        }
    }

    std::vector<PgParam> flat;
    flat.reserve(rows.size() * cols);
    std::vector<Oid> types;
    types.reserve(rows.size() * cols);
    for (auto& row : rows) {
        for (auto& p : row) {
            types.push_back(p.type);
            flat.push_back(std::move(p));
        }
    }

    uint32_t attempt = 0;
    auto backoff = std::chrono::milliseconds(50);
    for (;;) {
        auto sz_str = std::to_string(rows.size());
        std::string stmt_name;
        stmt_name.reserve(4 + table.size() + sz_str.size());
        stmt_name.append("bw_").append(table).push_back('_');
        stmt_name.append(sz_str);

        auto t0 = std::chrono::steady_clock::now();
        auto r  = pool_.exec_prepared(stmt_name, sql,
                                      std::span<const Oid>(types),
                                      std::span<const PgParam>(flat));
        auto dt = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - t0).count();

        // Per-row EMA — divide by row count so a 1000-row batch doesn't
        // permanently dominate the rolling estimate.
        double per_row_us = static_cast<double>(dt) / static_cast<double>(rows.size());
        uint64_t prev = pg_avg_us_.load(std::memory_order_relaxed);
        // alpha = 1/64 → ~1s smoothing at our tick rate; cheap on x86.
        uint64_t next_ema = (prev * 63 + static_cast<uint64_t>(per_row_us)) / 64;
        pg_avg_us_.store(next_ema, std::memory_order_relaxed);

        if (r.ok()) {
            s->flushed.fetch_add(rows.size(), std::memory_order_relaxed);
            FlushObserver obs;
            { std::lock_guard lk(obs_mtx_); obs = observer_; }
            if (obs) obs(table, rows.size(), true);
            return true;
        }
        if (++attempt >= s->cfg.max_retry) {
            std::string dlq = table + cfg_.dlq_suffix;
            std::string dlq_sql = "INSERT INTO " + dlq + " (payload) VALUES ($1)";
            std::string blob = sql + " [" + r.error_message + "]";
            std::vector<PgParam> ps = { PgParam::text(blob) };
            std::vector<Oid> pt = { Oid::Text };
            pool_.exec_prepared(stmt_name + "_dlq", dlq_sql, pt, ps);
            s->failed.fetch_add(rows.size(), std::memory_order_relaxed);
            FlushObserver obs;
            { std::lock_guard lk(obs_mtx_); obs = observer_; }
            if (obs) obs(table, rows.size(), false);
            return false;
        }
        std::this_thread::sleep_for(backoff);
        backoff = std::min(backoff * 2, std::chrono::milliseconds(2000));
    }
}

void BatchWriter::flusher_loop(std::string table, TableState* s) {
    using namespace std::chrono;
    while (true) {
        std::vector<std::vector<PgParam>> drained;
        {
            std::unique_lock lk(s->mtx);
            auto wait_ms = milliseconds(s->dyn_flush_ms.load(std::memory_order_relaxed));
            s->cv.wait_for(lk, wait_ms, [&]{
                return s->stop || s->flush_request ||
                       s->queue.size() >= s->dyn_batch_size.load(std::memory_order_relaxed);
            });
            if (s->stop && s->queue.empty()) return;
            uint32_t take = std::min<uint32_t>(
                static_cast<uint32_t>(s->queue.size()),
                s->dyn_batch_size.load(std::memory_order_relaxed));
            drained.reserve(take);
            for (uint32_t i = 0; i < take; ++i) {
                drained.push_back(std::move(s->queue.front().second));
                s->queue.pop_front();
            }
            s->flush_request = false;
        }
        if (!drained.empty()) send_batch(table, s, drained);
        {
            std::lock_guard lk(s->mtx);
            if (s->queue.empty()) s->drain_cv.notify_all();
        }
    }
}

// Re-evaluate per-endpoint allocations and PG-load-driven batch scaling.
// Cheap fixed-time work (~O(endpoints+tables)) so we can tick every
// controller_tick_ms without showing up in profiles.
void BatchWriter::rebalance() {
    using namespace std::chrono;
    const double tick_s = cfg_.controller_tick_ms / 1000.0;

    // ---- endpoint load measurement ----
    double total_rps = 0;
    {
        std::lock_guard lk(ep_mtx_);
        for (auto& [_, st] : endpoints_) {
            uint64_t n = st->in_window.exchange(0, std::memory_order_relaxed);
            st->last_rps = static_cast<double>(n) / tick_s;
            total_rps += st->last_rps;
        }
        // Allocate ≥1 slot to every active endpoint, distributing remaining
        // slots in proportion to RPS share. We treat each table's
        // current_batch_size as a slot budget for shaping enqueue priority.
        // Endpoints map indirectly to tables via the writers calling
        // enqueue(endpoint, table) — the slot value is informational here
        // (consumed by tests/metrics); the real per-endpoint shaping
        // happens because high-rps endpoints fill queues faster which
        // triggers batch_size-driven flushes.
        const uint32_t total_slots = 100;
        for (auto& [_, st] : endpoints_) {
            double share = total_rps > 0 ? st->last_rps / total_rps : 0;
            uint32_t slots = total_rps > 0
                ? std::max<uint32_t>(1, static_cast<uint32_t>(std::round(share * total_slots)))
                : 1;
            st->slots.store(slots, std::memory_order_relaxed);
        }
    }

    // ---- PG load scaling ----
    uint64_t avg_us = pg_avg_us_.load(std::memory_order_relaxed);
    double   thresh_us = cfg_.pg_slow_threshold_ms * 1000.0;
    double   scale = dyn_scale_.load(std::memory_order_relaxed);
    if (avg_us > thresh_us) {
        // PG is slow → back off: bigger batches, longer intervals. This
        // amortizes round-trip cost and lets PG drain its own backlog.
        scale = std::min(cfg_.scale_max, scale * 1.25);
    } else if (avg_us > 0 && avg_us < thresh_us / 2.0) {
        // PG is fast → tighten: smaller batches, faster intervals, lower
        // tail latency for the request that triggered the write.
        scale = std::max(cfg_.scale_min, scale * 0.85);
    }
    dyn_scale_.store(scale, std::memory_order_relaxed);

    // Combine the internal PG-load EMA (dyn_scale_) with the external
    // PressureController scale. Two knobs allow independent tuning:
    // dyn_scale_ reacts to per-batch RTT trend, external_scale_ reacts
    // to the global pressure FSM state.
    double ext = external_scale_.load(std::memory_order_relaxed);
    double combined = scale * ext;
    if (combined < 0.1)  combined = 0.1;
    if (combined > 16.0) combined = 16.0;

    for (auto& [_, s] : states_) {
        uint32_t new_bs = static_cast<uint32_t>(
            std::round(static_cast<double>(s->cfg.batch_size) * combined));
        new_bs = std::max<uint32_t>(1, new_bs);
        uint32_t new_fi = static_cast<uint32_t>(
            std::round(static_cast<double>(s->cfg.flush_interval_ms) * combined));
        new_fi = std::max<uint32_t>(1, new_fi);
        s->dyn_batch_size.store(new_bs, std::memory_order_relaxed);
        s->dyn_flush_ms.store(new_fi, std::memory_order_relaxed);
    }
}

void BatchWriter::controller_loop() {
    using namespace std::chrono;
    auto tick = milliseconds(cfg_.controller_tick_ms);
    while (!stop_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(tick);
        if (stop_.load(std::memory_order_acquire)) return;
        rebalance();
    }
}

} // namespace spido_pg
