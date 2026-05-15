// Walking demo of the spido_pg::Db API. Build with -DSPIDO_PG_WITH_EXAMPLES=ON.
//
// Set SPIDO_PG_TEST_SOCKET / _USER / _DBNAME (and optionally _PASSWORD) in
// the environment; the example refuses to start without them.

#include <spido_pg/db.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace spido_pg;
using namespace std::chrono_literals;

namespace {
const char* env_or_die(const char* k) {
    const char* v = std::getenv(k);
    if (!v) { std::fprintf(stderr, "%s not set\n", k); std::exit(2); }
    return v;
}
} // namespace

int main() {
    DbConfig cfg;
    cfg.pool.socket_path = env_or_die("SPIDO_PG_TEST_SOCKET");
    cfg.pool.user        = env_or_die("SPIDO_PG_TEST_USER");
    cfg.pool.dbname      = env_or_die("SPIDO_PG_TEST_DBNAME");
    if (const char* p = std::getenv("SPIDO_PG_TEST_PASSWORD")) cfg.pool.password = p;
    cfg.pool.min_conns = 2;
    cfg.pool.max_conns = 4;

    // Set up an example_events table for the batch demo.
    BatchTableConfig tc;
    tc.columns = {"ts", "payload"};
    tc.primary_key = "ts";
    tc.batch_size = 100;
    tc.flush_interval_ms = 50;
    cfg.batch.tables["example_events"] = tc;

    Db db(cfg);

    {
        auto r = db.query("DROP TABLE IF EXISTS example_events");
        if (!r.ok()) { std::fprintf(stderr, "drop: %s\n", r.error_message.c_str()); return 1; }
        r = db.query("CREATE TABLE example_events (ts int8 primary key, payload text)");
        if (!r.ok()) { std::fprintf(stderr, "create: %s\n", r.error_message.c_str()); return 1; }
    }

    // 1. Direct query.
    {
        std::fprintf(stderr, "[1] direct query\n");
        auto r = db.query("SELECT 1::int4 AS one, 'hi'::text AS greeting");
        if (!r.ok()) { std::fprintf(stderr, "  error: %s\n", r.error_message.c_str()); return 1; }
        std::fprintf(stderr, "  one=%d greeting=%s\n",
                     r.i4(0,0).value_or(-1),
                     std::string(r.text(0,1).value_or("?")).c_str());
    }

    // 2. Cached query.
    {
        std::fprintf(stderr, "[2] cached query\n");
        auto before = db.cache().stats();
        for (int i = 0; i < 3; ++i) {
            auto r = db.query_cached("SELECT 42::int4",
                                     {}, 30s, "example_events");
            (void)r;
        }
        auto after = db.cache().stats();
        std::fprintf(stderr, "  l1_hits +%llu, l1_misses +%llu, l2_hits +%llu, l2_misses +%llu\n",
                     (unsigned long long)(after.l1_hits - before.l1_hits),
                     (unsigned long long)(after.l1_misses - before.l1_misses),
                     (unsigned long long)(after.l2_hits - before.l2_hits),
                     (unsigned long long)(after.l2_misses - before.l2_misses));
    }

    // 3. Batch write.
    {
        std::fprintf(stderr, "[3] batch write (1000 rows)\n");
        for (int i = 0; i < 1000; ++i) {
            db.write("/events", "example_events",
                     { PgParam::i8(i), PgParam::text("evt") });
        }
        db.batch().flush_now("example_events");
        auto r = db.query("SELECT COUNT(*)::int8 FROM example_events");
        std::fprintf(stderr, "  row count: %lld\n",
                     (long long)r.i8(0, 0).value_or(-1));
    }

    // 4. Invalidation via NOTIFY.
    {
        std::fprintf(stderr, "[4] NOTIFY → cache invalidation\n");
        // Warm the cache.
        db.query_cached("SELECT 1", {}, 60s, "example_events");
        auto stats0 = db.cache().stats();
        db.query("NOTIFY spido_pg_invalidate, 'example_events'");
        // Notifier runs on a background thread; give it a moment.
        std::this_thread::sleep_for(500ms);
        auto stats1 = db.cache().stats();
        std::fprintf(stderr, "  invalidations: %llu -> %llu\n",
                     (unsigned long long)stats0.invalidations,
                     (unsigned long long)stats1.invalidations);
    }

    db.query("DROP TABLE example_events");
    std::fprintf(stderr, "done.\n");
    return 0;
}
