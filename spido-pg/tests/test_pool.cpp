#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "spido_pg/pool.h"

#include <atomic>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace spido_pg;

namespace {

PoolConfig env_cfg() {
    PoolConfig c;
    const char* s = std::getenv("SPIDO_PG_TEST_SOCKET");
    const char* u = std::getenv("SPIDO_PG_TEST_USER");
    const char* d = std::getenv("SPIDO_PG_TEST_DBNAME");
    const char* p = std::getenv("SPIDO_PG_TEST_PASSWORD");
    if (s) c.socket_path = s;
    if (u) c.user        = u;
    if (d) c.dbname      = d;
    if (p) c.password    = p;
    c.min_conns = 2; c.max_conns = 8;
    c.health_check_s = 0;
    return c;
}
bool have_pg() { return std::getenv("SPIDO_PG_TEST_SOCKET") != nullptr; }

#define SKIP_IF_NO_PG() do { if (!have_pg()) { \
    MESSAGE("SPIDO_PG_TEST_* not set — skipping"); return; } } while (0)

} // namespace

TEST_CASE("PgPool: concurrent checkout/checkin") {
    SKIP_IF_NO_PG();
    PgPool pool(env_cfg());

    constexpr int kThreads = 16;
    constexpr int kOpsPerThread = 200;
    std::atomic<int> ok{0};

    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&]{
            for (int i = 0; i < kOpsPerThread; ++i) {
                auto c = pool.checkout();
                REQUIRE(c);
                auto r = c->exec("SELECT 1");
                if (r.ok() && r.rows.size() == 1) ok.fetch_add(1);
            }
        });
    }
    for (auto& th : ts) th.join();
    CHECK(ok.load() == kThreads * kOpsPerThread);
    CHECK(pool.live_conns() <= pool.config().max_conns);
}

TEST_CASE("PgPool: grows up to max_conns under load") {
    SKIP_IF_NO_PG();
    auto cfg = env_cfg();
    cfg.min_conns = 1;
    cfg.max_conns = 4;
    PgPool pool(cfg);

    // Hold 4 connections concurrently — should reach max_conns.
    std::vector<PgConn> holders;
    for (int i = 0; i < 4; ++i) {
        auto c = pool.checkout();
        REQUIRE(c);
        holders.push_back(std::move(c));
    }
    CHECK(pool.live_conns() == 4);
}
