#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "spido_pg/entity_cache.h"

#include <chrono>
#include <thread>

using namespace spido_pg;
using namespace std::chrono_literals;

namespace {
EntityRow row_of(std::string data, uint64_t v = 1) {
    EntityRow r; r.data = std::move(data); r.version = v; return r;
}
} // namespace

TEST_CASE("EntityCache: put + get round-trip") {
    EntityCache c;
    c.put(1, "k1", row_of("hello", 1), /*dirty=*/false);
    auto got = c.get(1, "k1");
    REQUIRE(got);
    CHECK(got->data == "hello");
    CHECK(got->version == 1);
}

TEST_CASE("EntityCache: miss returns nullptr") {
    EntityCache c;
    CHECK_FALSE(c.get(1, "absent"));
}

TEST_CASE("EntityCache: same-pk overwrites with newer version") {
    EntityCache c;
    c.put(1, "k", row_of("v1", 1), false);
    c.put(1, "k", row_of("v2", 2), false);
    auto got = c.get(1, "k");
    REQUIRE(got);
    CHECK(got->data == "v2");
    CHECK(got->version == 2);
}

TEST_CASE("EntityCache: dirty entries survive eviction pressure") {
    EntityCacheConfig cfg;
    cfg.max_bytes = 2048;          // very small
    cfg.sweep_interval_ms = 50;
    cfg.slots_per_shard = 64;
    cfg.shards = 4;
    EntityCache c(cfg);

    // Mark a single hot entry dirty.
    c.put(1, "hot", row_of(std::string(100, 'H'), 1), /*dirty=*/true);

    // Flood with clean entries to push past quota.
    for (int i = 0; i < 200; ++i) {
        c.put(2, "cold" + std::to_string(i),
              row_of(std::string(80, 'c'), 1),
              /*dirty=*/false);
    }
    std::this_thread::sleep_for(200ms);
    auto got = c.get(1, "hot");
    REQUIRE(got);
    CHECK(got->data.size() == 100);
}

TEST_CASE("EntityCache: tombstone makes get return nullptr but pins slot") {
    EntityCache c;
    c.put(1, "k", row_of("alive", 1), /*dirty=*/false);
    c.erase(1, "k", 2);
    CHECK_FALSE(c.get(1, "k"));
    auto st = c.stats();
    CHECK(st.tombstones > 0);
}

TEST_CASE("EntityCache: NOTIFY with older version is ignored") {
    EntityCache c;
    c.put(1, "k", row_of("local-v5", 5), /*dirty=*/false);
    c.notify_remote_version(1, "k", /*remote=*/3);   // older
    auto got = c.get(1, "k");
    REQUIRE(got);
    CHECK(got->data == "local-v5");
}

TEST_CASE("EntityCache: NOTIFY with newer version evicts so next read refills") {
    EntityCache c;
    c.put(1, "k", row_of("local-v5", 5), /*dirty=*/false);
    c.notify_remote_version(1, "k", /*remote=*/9);
    CHECK_FALSE(c.get(1, "k"));     // evicted, next get would hit PG
}

TEST_CASE("EntityCache: NOTIFY cannot evict dirty local") {
    EntityCache c;
    c.put(1, "k", row_of("local-dirty-v5", 5), /*dirty=*/true);
    c.notify_remote_version(1, "k", 99);
    auto got = c.get(1, "k");
    REQUIRE(got);
    CHECK(got->version == 5);
}

TEST_CASE("EntityCache: mark_flushed clears dirty so eviction can reclaim") {
    EntityCache c;
    c.put(1, "k", row_of("payload", 3), /*dirty=*/true);
    c.mark_flushed(1, "k", 3);
    auto st = c.stats();
    // Entry still present (clean now) — get returns it.
    auto got = c.get(1, "k");
    REQUIRE(got);
    CHECK(got->version == 3);
    CHECK(st.dirty == 0);
}

TEST_CASE("EntityCache: mark_flushed for tombstone drops the slot") {
    EntityCache c;
    c.put(1, "k", row_of("alive", 1), /*dirty=*/false);
    c.erase(1, "k", 2);
    c.mark_flushed(1, "k", 2);
    CHECK_FALSE(c.get(1, "k"));
}

TEST_CASE("EntityCache: pin protects from sweep eviction") {
    EntityCacheConfig cfg;
    cfg.max_bytes = 1024;
    cfg.sweep_interval_ms = 30;
    cfg.shards = 2; cfg.slots_per_shard = 32;
    EntityCache c(cfg);

    c.put(1, "vip", row_of(std::string(64, 'V'), 1), false);
    c.pin(1, "vip");
    for (int i = 0; i < 64; ++i) {
        c.put(2, "junk" + std::to_string(i),
              row_of(std::string(64, 'j'), 1), false);
    }
    std::this_thread::sleep_for(100ms);
    auto got = c.get(1, "vip");
    REQUIRE(got);
    CHECK(got->data.size() == 64);
}
