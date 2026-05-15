#include "spido_pg/pool.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <unistd.h>

namespace spido_pg {

// ---------- PgConn ----------

PgConn::~PgConn() {
    if (pool_ && conn_) pool_->checkin(std::move(conn_));
}
PgConn::PgConn(PgConn&& o) noexcept : pool_(o.pool_), conn_(std::move(o.conn_)) {
    o.pool_ = nullptr;
}
PgConn& PgConn::operator=(PgConn&& o) noexcept {
    if (this != &o) {
        if (pool_ && conn_) pool_->checkin(std::move(conn_));
        pool_ = o.pool_; conn_ = std::move(o.conn_); o.pool_ = nullptr;
    }
    return *this;
}

// ---------- PgPool ----------

PgPool::PgPool(PoolConfig cfg) : cfg_(std::move(cfg)) {
    if (cfg_.min_conns == 0) cfg_.min_conns = std::max(1u, std::thread::hardware_concurrency());
    if (cfg_.max_conns == 0) cfg_.max_conns = cfg_.min_conns * 4;
    if (cfg_.max_conns < cfg_.min_conns) cfg_.max_conns = cfg_.min_conns;

    // Eagerly fill min_conns. Failure to reach min is non-fatal — callers
    // will retry on first checkout, and the health loop keeps trying.
    {
        std::unique_lock lk(mtx_);
        for (uint32_t i = 0; i < cfg_.min_conns; ++i) {
            lk.unlock();
            auto c = create_one();
            lk.lock();
            if (c) {
                idle_.push_back(std::move(c));
                live_.fetch_add(1, std::memory_order_relaxed);
            } else {
                break;
            }
        }
    }

    if (cfg_.health_check_s > 0) {
        health_thread_ = std::thread([this]{ health_loop(); });
    }
}

PgPool::~PgPool() {
    stop_.store(true, std::memory_order_release);
    cv_.notify_all();
    if (health_thread_.joinable()) health_thread_.join();
    std::lock_guard lk(mtx_);
    idle_.clear();
}

std::unique_ptr<PgConnection> PgPool::create_one() {
    auto c = std::make_unique<PgConnection>();
    if (!c->connect(cfg_.socket_path, cfg_.user, cfg_.password, cfg_.dbname)) {
        return nullptr;
    }
    return c;
}

PgConn PgPool::checkout() {
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::seconds(cfg_.connect_timeout_s);

    std::unique_lock lk(mtx_);
    for (;;) {
        if (stop_.load(std::memory_order_acquire)) return {};
        if (!idle_.empty()) {
            auto c = std::move(idle_.front()); idle_.pop_front();
            lk.unlock();
            if (!c->is_ready()) {
                // Connection died while idle — drop it and try again from scratch.
                live_.fetch_sub(1, std::memory_order_relaxed);
                c.reset();
                lk.lock();
                continue;
            }
            return PgConn(this, std::move(c));
        }

        uint32_t cur = live_.load(std::memory_order_relaxed);
        if (cur < cfg_.max_conns) {
            // Try to grow. Atomically reserve the slot so concurrent
            // checkers don't all race past max_conns.
            if (live_.compare_exchange_strong(cur, cur + 1,
                                              std::memory_order_acq_rel)) {
                lk.unlock();
                auto c = create_one();
                if (!c) {
                    live_.fetch_sub(1, std::memory_order_relaxed);
                    // Exponential-ish: short pause, then re-enter loop.
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    lk.lock();
                    if (std::chrono::steady_clock::now() >= deadline) return {};
                    continue;
                }
                return PgConn(this, std::move(c));
            } else {
                // CAS failed → another thread reserved; loop and pick up an
                // idle, or try again.
                continue;
            }
        }

        // Pool is at max. Wait for a checkin or timeout.
        if (cv_.wait_until(lk, deadline) == std::cv_status::timeout)
            return {};
    }
}

void PgPool::checkin(std::unique_ptr<PgConnection> c) noexcept {
    if (!c) return;
    bool healthy = c->is_ready();
    {
        std::lock_guard lk(mtx_);
        if (!healthy) {
            // Drop. Live count goes down.
            live_.fetch_sub(1, std::memory_order_relaxed);
        } else {
            idle_.push_back(std::move(c));
        }
    }
    cv_.notify_one();
}

uint32_t PgPool::idle_conns() const noexcept {
    std::lock_guard lk(const_cast<std::mutex&>(mtx_));
    return static_cast<uint32_t>(idle_.size());
}

PgResult PgPool::exec(std::string_view sql) {
    auto c = checkout();
    if (!c) {
        PgResult r; r.had_error = true; r.error_message = "pool exhausted";
        return r;
    }
    return c->exec(sql);
}

PgResult PgPool::exec_prepared(std::string_view stmt_name,
                               std::string_view sql,
                               std::span<const Oid> param_types,
                               std::span<const PgParam> params)
{
    auto c = checkout();
    if (!c) {
        PgResult r; r.had_error = true; r.error_message = "pool exhausted";
        return r;
    }

    // Decide whether we need to (re-)prepare on this conn. We key on the SQL
    // text rather than the user-supplied name so the cache is shared even
    // when callers reuse the same canonical SQL across worker threads.
    bool need_prepare = false;
    {
        std::lock_guard plk(prep_mtx_);
        auto pit = prep_cache_.find(sql);
        if (pit == prep_cache_.end()) {
            pit = prep_cache_.emplace(std::string(sql), PrepInfo{}).first;
        }
        auto& info = pit->second;
        if (info.param_types.empty() && !param_types.empty()) {
            info.param_types.assign(param_types.begin(), param_types.end());
        }
        auto it = info.last_epoch.find(c->fd());
        if (it == info.last_epoch.end() || it->second != c->epoch()) {
            need_prepare = true;
            info.last_epoch[c->fd()] = c->epoch();
        }
    }

    if (need_prepare) {
        c->prepare(stmt_name, sql, param_types);
    }

    StmtHandle h;
    h.name = std::string(stmt_name);
    h.param_types.assign(param_types.begin(), param_types.end());
    h.epoch = c->epoch();
    return c->exec_prepared(h, params);
}

void PgPool::health_loop() {
    using namespace std::chrono;
    auto period = seconds(cfg_.health_check_s);
    while (!stop_.load(std::memory_order_acquire)) {
        std::unique_lock lk(mtx_);
        if (cv_.wait_for(lk, period, [this]{ return stop_.load(std::memory_order_acquire); }))
            return;

        // Snapshot idle conns and ping them off-mutex.
        std::deque<std::unique_ptr<PgConnection>> drained;
        drained.swap(idle_);
        lk.unlock();

        std::deque<std::unique_ptr<PgConnection>> survivors;
        for (auto& c : drained) {
            auto r = c->exec("SELECT 1");
            if (r.ok() && c->is_ready()) {
                survivors.push_back(std::move(c));
            } else {
                live_.fetch_sub(1, std::memory_order_relaxed);
            }
        }
        lk.lock();
        for (auto& c : survivors) idle_.push_back(std::move(c));

        // Top-up below min_conns. Backoff handled by sleep loop above.
        while (live_.load(std::memory_order_relaxed) < cfg_.min_conns) {
            lk.unlock();
            auto c = create_one();
            lk.lock();
            if (!c) break;
            idle_.push_back(std::move(c));
            live_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

} // namespace spido_pg
