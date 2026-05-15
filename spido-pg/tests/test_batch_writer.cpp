#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "spido_pg/batch_writer.h"
#include "spido_pg/pool.h"

#include <chrono>
#include <cstdlib>
#include <thread>

using namespace spido_pg;
using namespace std::chrono_literals;

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
    c.min_conns = 1; c.max_conns = 2;
    c.health_check_s = 0;
    return c;
}
bool have_pg() { return std::getenv("SPIDO_PG_TEST_SOCKET") != nullptr; }

#define SKIP_IF_NO_PG() do { if (!have_pg()) { \
    MESSAGE("SPIDO_PG_TEST_* not set — skipping"); return; } } while (0)

} // namespace

TEST_CASE("BatchWriter: flush fires on batch_size") {
    SKIP_IF_NO_PG();
    PgPool pool(env_cfg());
    // Set up a temp table.
    pool.exec("DROP TABLE IF EXISTS bw_test");
    pool.exec("CREATE TABLE bw_test (id int4 primary key, label text)");

    BatchWriterConfig bwc;
    BatchTableConfig tc;
    tc.columns = {"id", "label"};
    tc.primary_key = "id";
    tc.batch_size = 10;
    tc.flush_interval_ms = 100000; // basically off — we want batch_size to drive
    bwc.tables["bw_test"] = tc;

    BatchWriter bw(pool, bwc);
    for (int i = 0; i < 10; ++i) {
        bw.enqueue("/bw", "bw_test", { PgParam::i4(i), PgParam::text("x") });
    }
    // Wait a moment for flusher to drain.
    for (int i = 0; i < 50 && bw.stats("bw_test").flushed < 10; ++i)
        std::this_thread::sleep_for(20ms);
    CHECK(bw.stats("bw_test").flushed >= 10);

    pool.exec("DROP TABLE bw_test");
}

TEST_CASE("BatchWriter: flush fires on flush_interval_ms") {
    SKIP_IF_NO_PG();
    PgPool pool(env_cfg());
    pool.exec("DROP TABLE IF EXISTS bw_test");
    pool.exec("CREATE TABLE bw_test (id int4 primary key, label text)");

    BatchWriterConfig bwc;
    BatchTableConfig tc;
    tc.columns = {"id", "label"};
    tc.primary_key = "id";
    tc.batch_size = 1000;            // big — won't be hit
    tc.flush_interval_ms = 50;       // short
    bwc.tables["bw_test"] = tc;

    BatchWriter bw(pool, bwc);
    bw.enqueue("/bw", "bw_test", { PgParam::i4(1), PgParam::text("a") });
    bw.enqueue("/bw", "bw_test", { PgParam::i4(2), PgParam::text("b") });

    for (int i = 0; i < 20 && bw.stats("bw_test").flushed < 2; ++i)
        std::this_thread::sleep_for(20ms);
    CHECK(bw.stats("bw_test").flushed >= 2);

    pool.exec("DROP TABLE bw_test");
}

TEST_CASE("BatchWriter: dynamic priority allocates more slots to busy endpoint") {
    SKIP_IF_NO_PG();
    PgPool pool(env_cfg());
    pool.exec("DROP TABLE IF EXISTS bw_prio");
    pool.exec("CREATE TABLE bw_prio (id int4 primary key, label text)");

    BatchWriterConfig bwc;
    BatchTableConfig tc;
    tc.columns = {"id", "label"};
    tc.primary_key = "id";
    tc.batch_size = 1000;
    tc.flush_interval_ms = 100;
    bwc.tables["bw_prio"] = tc;
    bwc.controller_tick_ms = 50;     // fast tick so the test doesn't drag

    BatchWriter bw(pool, bwc);

    // /hot gets ~10x the rate of /cold for a window.
    int hot_id = 0;
    int cold_id = 100000;
    for (int round = 0; round < 6; ++round) {
        for (int i = 0; i < 100; ++i) {
            bw.enqueue("/hot", "bw_prio",
                       { PgParam::i4(hot_id++), PgParam::text("h") });
        }
        bw.enqueue("/cold", "bw_prio",
                   { PgParam::i4(cold_id++), PgParam::text("c") });
        // Sleep so the controller ticks observe the rate.
        std::this_thread::sleep_for(60ms);
    }

    auto loads = bw.endpoint_loads();
    REQUIRE(loads.size() == 2);

    uint32_t hot_slots = 0, cold_slots = 0;
    for (const auto& e : loads) {
        if (e.endpoint == "/hot")  hot_slots  = e.write_slots;
        if (e.endpoint == "/cold") cold_slots = e.write_slots;
    }
    MESSAGE("hot=" << hot_slots << " cold=" << cold_slots);
    CHECK(hot_slots > cold_slots);

    pool.exec("DROP TABLE bw_prio");
}

TEST_CASE("BatchWriter: state-model coalesces same-pk rows in a flush") {
    SKIP_IF_NO_PG();
    PgPool pool(env_cfg());
    pool.exec("DROP TABLE IF EXISTS bw_state");
    pool.exec("CREATE TABLE bw_state (id int4 primary key, label text)");

    BatchWriterConfig bwc;
    BatchTableConfig tc;
    tc.columns    = {"id", "label"};
    tc.primary_key = "id";
    tc.data_model  = DataModel::State;
    tc.batch_size  = 100;
    tc.flush_interval_ms = 100000;
    bwc.tables["bw_state"] = tc;

    {
        BatchWriter bw(pool, bwc);
        // Same pk 5 times with different labels — only the last should land.
        for (int i = 0; i < 5; ++i) {
            bw.enqueue("/s", "bw_state",
                       { PgParam::i4(1),
                         PgParam::text("v" + std::to_string(i)) });
        }
        bw.flush_now("bw_state");
    }

    auto r = pool.exec("SELECT label FROM bw_state WHERE id=1");
    REQUIRE(r.ok());
    REQUIRE(r.rows.size() == 1);
    auto v = r.text(0, 0);
    REQUIRE(v.has_value());
    CHECK(std::string(*v) == "v4");

    pool.exec("DROP TABLE bw_state");
}

TEST_CASE("BatchWriter: event-model preserves duplicate-pk rows") {
    SKIP_IF_NO_PG();
    PgPool pool(env_cfg());
    pool.exec("DROP TABLE IF EXISTS bw_event");
    pool.exec("CREATE TABLE bw_event (id bigserial primary key, kind text, payload text)");

    BatchWriterConfig bwc;
    BatchTableConfig tc;
    tc.columns    = {"kind", "payload"};
    tc.primary_key = "";          // no pk in row data — bigserial handles it
    tc.data_model  = DataModel::Event;
    tc.batch_size  = 100;
    tc.flush_interval_ms = 100000;
    bwc.tables["bw_event"] = tc;

    {
        BatchWriter bw(pool, bwc);
        for (int i = 0; i < 5; ++i) {
            bw.enqueue("/e", "bw_event",
                       { PgParam::text("login"), PgParam::text("p" + std::to_string(i)) });
        }
        bw.flush_now("bw_event");
    }

    auto r = pool.exec("SELECT COUNT(*)::int8 FROM bw_event");
    REQUIRE(r.ok());
    CHECK(r.i8(0, 0).value_or(0) == 5);

    pool.exec("DROP TABLE bw_event");
}

TEST_CASE("BatchWriter: upsert SQL has ON CONFLICT") {
    SKIP_IF_NO_PG();
    PgPool pool(env_cfg());
    pool.exec("DROP TABLE IF EXISTS bw_test");
    pool.exec("CREATE TABLE bw_test (id int4 primary key, label text)");

    BatchWriterConfig bwc;
    BatchTableConfig tc;
    tc.columns = {"id", "label"};
    tc.primary_key = "id";
    tc.batch_size = 2;
    tc.flush_interval_ms = 100000;
    tc.upsert = true;
    bwc.tables["bw_test"] = tc;

    {
        BatchWriter bw(pool, bwc);
        bw.enqueue("/bw", "bw_test", { PgParam::i4(1), PgParam::text("first") });
        bw.enqueue("/bw", "bw_test", { PgParam::i4(1), PgParam::text("second") });
        bw.flush_now("bw_test");
    }
    auto r = pool.exec("SELECT label FROM bw_test WHERE id=1");
    REQUIRE(r.ok());
    REQUIRE(r.rows.size() == 1);
    auto lv = r.text(0, 0);
    REQUIRE(lv.has_value());
    CHECK(std::string(*lv) == "second");

    pool.exec("DROP TABLE bw_test");
}
