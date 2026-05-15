#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "spido_pg/wal.h"

#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

using namespace spido_pg;
using namespace std::chrono_literals;

namespace {

std::filesystem::path fresh_wal_dir(const char* tag) {
    auto p = std::filesystem::temp_directory_path()
           / ("spido_pg_wal_" + std::string(tag) + "_" +
              std::to_string(::getpid()));
    std::filesystem::remove_all(p);
    return p;
}

WalRecord make_rec(uint64_t ep, std::string pk, std::string row) {
    WalRecord r;
    r.endpoint_id = ep;
    r.table_id    = 1;
    r.op          = WalOp::Upsert;
    r.primary_key = std::move(pk);
    r.row_bytes   = std::move(row);
    r.row_version = 1;
    r.timestamp   = std::chrono::system_clock::now();
    return r;
}

} // namespace

TEST_CASE("WAL: append + replay round-trip") {
    auto dir = fresh_wal_dir("rt");
    {
        WalConfig cfg; cfg.directory = dir;
        cfg.fsync_mode = WalFsyncMode::FdatasyncAlways;
        WalManager wal(cfg);
        wal.append(make_rec(1, "k1", "row-A"));
        wal.append(make_rec(2, "k2", "row-B"));
        CHECK(wal.total_appended() == 2);
    }
    {
        WalConfig cfg; cfg.directory = dir;
        WalManager wal(cfg);
        std::vector<std::string> seen;
        wal.replay([&](const WalRecord& r) {
            seen.push_back(r.row_bytes);
            return true;
        });
        REQUIRE(seen.size() == 2);
        CHECK(seen[0] == "row-A");
        CHECK(seen[1] == "row-B");
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("WAL: group commit eventually fsyncs queued waiters") {
    auto dir = fresh_wal_dir("grp");
    WalConfig cfg; cfg.directory = dir;
    cfg.fsync_mode = WalFsyncMode::FdatasyncInterval;
    cfg.fsync_interval_ms = 5;
    WalManager wal(cfg);

    // Append a batch of records — they all flip durable together when the
    // fsync thread wakes up. The interval is an UPPER bound on the
    // batching window; with a waiter queued the cv may wake sooner.
    std::vector<WalDurabilityHandle> handles;
    for (int i = 0; i < 100; ++i)
        handles.push_back(wal.append(make_rec(1, "k" + std::to_string(i), "v")));
    // Within 50 ms every record must be durable.
    for (const auto& h : handles) CHECK(h.wait_for(50ms));

    std::filesystem::remove_all(dir);
}

TEST_CASE("WAL: corrupted trailing record stops replay safely") {
    auto dir = fresh_wal_dir("corrupt");
    {
        WalConfig cfg; cfg.directory = dir;
        cfg.fsync_mode = WalFsyncMode::FdatasyncAlways;
        WalManager wal(cfg);
        wal.append(make_rec(1, "k1", "good-1"));
        wal.append(make_rec(2, "k2", "good-2"));
    }
    // Append garbage bytes to the file's tail.
    auto path = dir / "wal.log";
    {
        FILE* f = std::fopen(path.c_str(), "ab");
        REQUIRE(f != nullptr);
        const char junk[] = "\xde\xad\xbe\xef\x00\x00\x00\x00";
        std::fwrite(junk, 1, sizeof junk, f);
        std::fclose(f);
    }
    WalConfig cfg; cfg.directory = dir;
    WalManager wal(cfg);
    std::vector<std::string> seen;
    wal.replay([&](const WalRecord& r) {
        seen.push_back(r.row_bytes);
        return true;
    });
    // Two good records still visible; trailing junk silently dropped.
    CHECK(seen.size() == 2);

    std::filesystem::remove_all(dir);
}

TEST_CASE("WAL: compaction reclaims fully-flushed prefix") {
    auto dir = fresh_wal_dir("compact");
    WalConfig cfg; cfg.directory = dir;
    cfg.fsync_mode = WalFsyncMode::FdatasyncAlways;
    WalManager wal(cfg);
    for (int i = 0; i < 100; ++i) {
        wal.append(make_rec(1, "k" + std::to_string(i),
                            std::string(200, 'x')));
    }
    CHECK(wal.pending_bytes() > 10000);
    wal.mark_flushed(100);
    size_t freed = wal.compact();
    CHECK(freed > 0);
    CHECK(wal.pending_bytes() == 0);

    std::filesystem::remove_all(dir);
}

TEST_CASE("WAL: replay yields records in append order") {
    auto dir = fresh_wal_dir("order");
    {
        WalConfig cfg; cfg.directory = dir;
        cfg.fsync_mode = WalFsyncMode::FdatasyncAlways;
        WalManager wal(cfg);
        for (int i = 0; i < 20; ++i) {
            wal.append(make_rec(static_cast<uint64_t>(i+1),
                                "k" + std::to_string(i),
                                "v" + std::to_string(i)));
        }
    }
    WalConfig cfg; cfg.directory = dir;
    WalManager wal(cfg);
    std::vector<uint64_t> eps;
    wal.replay([&](const WalRecord& r) {
        eps.push_back(r.endpoint_id);
        return true;
    });
    REQUIRE(eps.size() == 20);
    for (size_t i = 0; i < eps.size(); ++i) CHECK(eps[i] == i + 1);

    std::filesystem::remove_all(dir);
}
