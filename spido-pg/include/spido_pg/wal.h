#pragma once

// Write-ahead log for durable async writes. Every operation that an
// endpoint policy classifies as "durable" (consistency_level
// eventual_durable or stricter, write_mode async_durable/batch_durable)
// must hit the WAL before we tell the caller "200 OK". The batch writer
// then drains the WAL into PostgreSQL on its own schedule; if the
// process dies before that drain completes, recovery on next startup
// replays the un-flushed records to PG.
//
// Faz 2 scope:
//   - single growing file ("wal-XXXXXXXX.log")
//   - append-only with CRC32C-checksummed records
//   - group commit driven by fdatasync_interval_ms
//   - replay: open + scan + dispatch a callback per record
//   - compaction: drop the prefix of records whose offsets are < the
//     last-known-flushed checkpoint
// Deferred to Faz 4:
//   - segment rotation by size
//   - compression
//   - encryption
//   - WAL replication

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace spido_pg {

enum class WalOp : uint8_t {
    Insert      = 1,
    Update      = 2,
    Upsert      = 3,
    Delete      = 4,
    EventAppend = 5,
    Tombstone   = 6,
};

enum class WalFsyncMode : uint8_t {
    OsBuffered      = 0,    // no explicit flush; OS chooses
    FdatasyncInterval = 1,  // group-commit at fixed interval (default)
    FdatasyncAlways = 2,    // fdatasync after every append (slow, safest)
    FsyncAlways     = 3,    // full fsync after every append
};

struct WalConfig {
    bool         enabled        = true;
    std::filesystem::path directory = "./wal";
    uint64_t     max_file_size_mb = 256;   // honoured in Faz 4 (rotation)
    WalFsyncMode fsync_mode      = WalFsyncMode::FdatasyncInterval;
    uint32_t     fsync_interval_ms = 2;    // 2 ms default — Postgres-style
    bool         checksum         = true;
    bool         use_io_uring     = false; // Faz 4
};

// One serialised write. The caller (Db / BatchWriter) is responsible for
// constructing this from the typed RowData; the WAL stores the bytes
// verbatim. Recovery dispatches a deserialiser through a user-supplied
// callback so the WAL itself stays type-agnostic.
struct WalRecord {
    uint64_t     endpoint_id   = 0;
    uint64_t     table_id      = 0;
    WalOp        op            = WalOp::Insert;
    std::string  primary_key;          // raw bytes of pk param
    std::string  row_bytes;            // pre-serialised row payload
    uint64_t     row_version   = 0;
    std::string  idempotency_key;      // optional, empty = none
    std::chrono:: system_clock::time_point timestamp{};
};

// On-disk record layout (after the 8-byte file magic):
//   uint32  total_size_incl_self   (network order)
//   uint32  crc32c (covers fields below + payload)
//   uint64  sequence              monotonic per-WalManager
//   uint64  endpoint_id
//   uint64  table_id
//   uint8   op
//   uint64  row_version
//   int64   timestamp_us_since_epoch
//   uint32  pk_len, pk_len bytes
//   uint32  idem_len, idem_len bytes
//   uint32  row_len, row_len bytes
// All multi-byte integers are little-endian on disk to match host x86.

// Result handle returned by append(). Callers that need durability
// before they ack the client wait on it; pure best-effort callers ignore
// it. The "durable_at" is the byte offset in the WAL once fdatasync has
// guaranteed the record is on stable storage.
struct WalDurabilityHandle {
    uint64_t                                            sequence  = 0;
    uint64_t                                            offset    = 0;
    std::shared_ptr<std::atomic<bool>>                  durable;

    // Block until the record is durable, or timeout. Returns true if
    // durable, false on timeout (caller can keep waiting or downgrade
    // the response).
    bool wait_for(std::chrono::milliseconds timeout) const;
};

class WalManager {
public:
    explicit WalManager(WalConfig cfg);
    ~WalManager();

    WalManager(const WalManager&)            = delete;
    WalManager& operator=(const WalManager&) = delete;

    // Append a record. Returns a handle the caller can wait on if it
    // wants durability semantics. Append itself is fast — actual fsync
    // happens in the group-commit thread.
    WalDurabilityHandle append(const WalRecord& rec);

    // Mark a sequence (or earlier) as flushed to PG. Compaction uses
    // this as a high-water mark: records with sequence <= last_flushed
    // and offset < current_compaction_point are safe to drop on the
    // next compaction pass.
    void mark_flushed(uint64_t sequence_inclusive) noexcept;

    // Trigger a synchronous compaction pass. Returns the number of
    // bytes reclaimed. Safe to call from a background thread; we hold
    // an internal mutex so callers can't race with the appender.
    size_t compact();

    // Replay un-flushed records through a callback. The callback is
    // invoked in increasing sequence order; it returns true to mark
    // the record as flushed (recovery succeeded), false to leave it
    // pending (recovery should re-try later).
    using ReplayFn = std::function<bool(const WalRecord&)>;
    void replay(ReplayFn fn);

    // Metrics.
    uint64_t pending_records() const noexcept {
        return appended_seq_.load(std::memory_order_relaxed)
             - flushed_seq_.load(std::memory_order_relaxed);
    }
    uint64_t pending_bytes() const noexcept {
        return pending_bytes_.load(std::memory_order_relaxed);
    }
    uint64_t total_appended() const noexcept {
        return appended_seq_.load(std::memory_order_relaxed);
    }

private:
    void   open_or_create();
    void   write_record_unlocked(const WalRecord& rec, uint64_t seq,
                                 uint64_t& out_offset);
    void   fsync_loop();
    void   do_fdatasync_unlocked();

    static uint32_t crc32c(const void* data, size_t len) noexcept;

    WalConfig          cfg_;
    int                fd_ = -1;
    std::filesystem::path path_;

    std::mutex                     write_mtx_;
    std::condition_variable        fsync_cv_;
    uint64_t                       current_offset_ = 0;

    std::atomic<uint64_t>          appended_seq_{0};
    std::atomic<uint64_t>          flushed_seq_{0};
    std::atomic<uint64_t>          pending_bytes_{0};

    // Waiters keyed by offset they need to be ≤ durable_offset before
    // their handles can flip. fdatasync_loop wakes them.
    struct Waiter {
        uint64_t                                offset;
        std::shared_ptr<std::atomic<bool>>      durable;
    };
    std::vector<Waiter>            waiters_;
    std::atomic<uint64_t>          durable_offset_{0};

    std::thread                    fsync_thread_;
    std::atomic<bool>              stop_{false};
};

} // namespace spido_pg
