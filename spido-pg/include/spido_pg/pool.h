#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "spido_pg/connection.h"
#include "spido_pg/string_map.h"

namespace spido_pg {

struct PoolConfig {
    std::string socket_path = "/var/run/postgresql/.s.PGSQL.5432";
    std::string user;
    std::string password;
    std::string dbname;

    uint32_t    min_conns         = 0;   // 0 = std::thread::hardware_concurrency()
    uint32_t    max_conns         = 0;   // 0 = nproc * 4
    uint32_t    idle_timeout_s    = 60;
    uint32_t    connect_timeout_s = 5;

    // PG-side keepalive ping interval. Set to 0 to disable.
    uint32_t    health_check_s    = 30;
};

class PgPool;

// RAII checkout. Returns to the pool on destruction. Moveable, not copyable.
class PgConn {
public:
    PgConn() = default;
    PgConn(PgPool* pool, std::unique_ptr<PgConnection> c) noexcept
        : pool_(pool), conn_(std::move(c)) {}
    ~PgConn();

    PgConn(const PgConn&)            = delete;
    PgConn& operator=(const PgConn&) = delete;
    PgConn(PgConn&& o) noexcept;
    PgConn& operator=(PgConn&& o) noexcept;

    PgConnection*       operator->()       noexcept { return conn_.get(); }
    const PgConnection* operator->() const noexcept { return conn_.get(); }
    PgConnection&       operator*()        noexcept { return *conn_; }
    explicit operator bool() const noexcept         { return static_cast<bool>(conn_); }

    // Release without returning to the pool — caller commits to destroying
    // the conn (e.g., after a fatal protocol error).
    std::unique_ptr<PgConnection> release() noexcept { pool_ = nullptr; return std::move(conn_); }

private:
    PgPool*                        pool_ = nullptr;
    std::unique_ptr<PgConnection>  conn_;
};

class PgPool {
public:
    explicit PgPool(PoolConfig cfg);
    ~PgPool();

    PgPool(const PgPool&)            = delete;
    PgPool& operator=(const PgPool&) = delete;

    // Blocks until a connection is available, or returns an empty PgConn on
    // hard failure (e.g. PG unreachable past connect_timeout_s).
    PgConn   checkout();

    // Internal — called by ~PgConn. Not part of the public API but exposed
    // because PgConn is in the same translation unit.
    void     checkin(std::unique_ptr<PgConnection> c) noexcept;

    // Convenience wrappers that borrow → exec → return in one call.
    PgResult exec(std::string_view sql);
    PgResult exec_prepared(std::string_view stmt_name,
                           std::string_view sql,
                           std::span<const Oid> param_types,
                           std::span<const PgParam> params);

    const PoolConfig& config() const noexcept { return cfg_; }
    uint32_t          live_conns() const noexcept { return live_.load(std::memory_order_relaxed); }
    uint32_t          idle_conns() const noexcept;

private:
    std::unique_ptr<PgConnection> create_one();
    void                          health_loop();

    PoolConfig                                  cfg_;
    std::mutex                                  mtx_;
    std::condition_variable                     cv_;
    std::deque<std::unique_ptr<PgConnection>>   idle_;
    std::atomic<uint32_t>                       live_{0};
    std::atomic<bool>                           stop_{false};

    // Per-statement prepare cache, keyed by SQL. We re-prepare lazily when
    // a borrowed connection's epoch doesn't match the cached one.
    struct PrepInfo {
        std::vector<Oid>                                 param_types;
        std::unordered_map<int /*conn fd*/, uint64_t>    last_epoch;
    };
    std::mutex                              prep_mtx_;
    StringMap<PrepInfo>                     prep_cache_;

    std::thread                             health_thread_;
};

} // namespace spido_pg
