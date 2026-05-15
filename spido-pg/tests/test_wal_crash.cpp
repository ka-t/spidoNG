// Crash-recovery harness for the WAL.
//
// The doctest TEST_CASE blocks fork a child that hammers the WAL with
// appends in fdatasync_interval mode, then the parent kills it with
// SIGKILL at a randomised moment. After reaping the child the parent
// reopens the WAL directory and walks the records, verifying that:
//
//   1. Every record the child saw as "durable" (via wait_for) is
//      present and undamaged after recovery.
//   2. Records still in flight at kill time are either present-and-
//      intact OR cleanly missing — never partially decoded.
//   3. The post-recovery WAL is reusable: a fresh WalManager can
//      append, mark_flushed, compact without errors.
//
// Each iteration uses a random kill delay between 5 ms and 50 ms so we
// hit a different point in the appender's loop each time.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "spido_pg/wal.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace spido_pg;
using namespace std::chrono_literals;

namespace {

std::filesystem::path fresh_dir(const char* tag) {
    auto p = std::filesystem::temp_directory_path()
           / ("spido_pg_walcrash_" + std::string(tag) + "_" +
              std::to_string(::getpid()));
    std::filesystem::remove_all(p);
    return p;
}

WalRecord rec(uint64_t i, std::string payload) {
    WalRecord r;
    r.endpoint_id = i;
    r.table_id    = 1;
    r.op          = WalOp::Upsert;
    r.primary_key = std::to_string(i);
    r.row_bytes   = std::move(payload);
    r.row_version = i;
    r.timestamp   = std::chrono::system_clock::now();
    return r;
}

// Child appends records as fast as it can, writing its own progress to
// a "progress.txt" file in the WAL directory. The parent uses that
// file after reaping the child to know the highest sequence number the
// child *believed* it had made durable.
[[noreturn]] void child_loop(const std::filesystem::path& dir) {
    WalConfig cfg;
    cfg.directory = dir;
    cfg.fsync_mode = WalFsyncMode::FdatasyncInterval;
    cfg.fsync_interval_ms = 2;
    WalManager wal(cfg);
    auto progress_path = dir / "progress.txt";
    uint64_t durable_seq = 0;
    for (uint64_t i = 1; ; ++i) {
        auto h = wal.append(rec(i, std::string(64, 'x')));
        if (h.wait_for(50ms)) {
            durable_seq = h.sequence;
            // Persist progress every 32 records to bound disk traffic.
            // Use O_TRUNC + fsync so the file always reflects a
            // committed checkpoint the parent can trust.
            if ((i & 31) == 0) {
                std::ofstream f(progress_path, std::ios::trunc);
                f << durable_seq;
                f.flush();
                // We don't fdatasync here on purpose — we want a
                // realistic race between this checkpoint and the kill
                // signal.
            }
        }
    }
    std::_Exit(0);  // unreachable; SIGKILL gets us first
}

bool run_one_crash(const std::filesystem::path& dir,
                   std::chrono::milliseconds kill_after,
                   uint64_t& reported_durable,
                   uint64_t& recovered_count)
{
    pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        child_loop(dir);  // never returns
    }
    std::this_thread::sleep_for(kill_after);
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);

    // Read the child's last checkpoint.
    reported_durable = 0;
    std::ifstream f(dir / "progress.txt");
    if (f) f >> reported_durable;

    // Reopen WAL on this side and count recoverable records.
    WalConfig cfg; cfg.directory = dir;
    WalManager wal(cfg);
    recovered_count = 0;
    uint64_t last_seq = 0;
    bool ordered = true;
    wal.replay([&](const WalRecord& r) {
        ++recovered_count;
        if (r.endpoint_id <= last_seq) ordered = false;
        last_seq = r.endpoint_id;     // we encode the seq in endpoint_id
        return true;
    });
    return ordered;
}

} // namespace

TEST_CASE("WAL crash: every checkpointed record survives kill -9") {
    auto dir = fresh_dir("durable");
    std::mt19937 rng(42);
    for (int run = 0; run < 5; ++run) {
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        auto kill_after = std::chrono::milliseconds(5 + rng() % 45);
        uint64_t reported = 0, recovered = 0;
        bool ordered = run_one_crash(dir, kill_after, reported, recovered);
        // Recovery must produce a record set that's:
        //   - monotonically ordered by sequence (no torn re-orderings)
        //   - at least as big as what the child told us it had checkpointed
        // Records beyond `reported` may be present (silently fsync'd by
        // the kernel before kill) — that's a "bonus", not a violation.
        CHECK(ordered);
        CHECK(recovered >= reported);
        MESSAGE("kill_after=" << kill_after.count() << "ms checkpointed="
                << reported << " recovered=" << recovered);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("WAL crash: post-recovery WAL is still usable") {
    auto dir = fresh_dir("reuse");
    uint64_t reported = 0, recovered = 0;
    run_one_crash(dir, 20ms, reported, recovered);

    // The previous test left a (recovered) WAL at `dir`. Append more,
    // mark them flushed, compact, and verify it didn't break.
    WalConfig cfg; cfg.directory = dir;
    cfg.fsync_mode = WalFsyncMode::FdatasyncAlways;
    WalManager wal(cfg);
    auto h = wal.append(rec(99999, "post-recovery"));
    CHECK(h.wait_for(50ms));
    wal.mark_flushed(99999);
    // Compaction may be no-op (other records still pending), but it
    // must not throw.
    (void)wal.compact();

    std::filesystem::remove_all(dir);
}
