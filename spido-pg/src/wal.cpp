#include "spido_pg/wal.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

namespace spido_pg {

namespace {

constexpr char     kWalFileName[] = "wal.log";
constexpr uint64_t kFileMagic    = 0x53504944'4F50'4710ULL; // "SPIDOPG" + version 10

// Castagnoli polynomial CRC32C. We don't need a SIMD implementation for
// Faz 2 — the WAL is dominated by fdatasync, not CRC compute. ~400 MB/s
// on a typical desktop is fine.
constexpr uint32_t kCrcPoly = 0x82F63B78u;

inline uint32_t crc32c_byte(uint32_t crc, uint8_t b) noexcept {
    crc ^= b;
    for (int i = 0; i < 8; ++i)
        crc = (crc >> 1) ^ (kCrcPoly & -(int32_t)(crc & 1));
    return crc;
}

template <typename T>
void put_le(std::string& out, T v) {
    for (size_t i = 0; i < sizeof(T); ++i) out.push_back(static_cast<char>((v >> (i*8)) & 0xff));
}
template <typename T>
T get_le(const uint8_t* p) {
    T v = 0;
    for (size_t i = 0; i < sizeof(T); ++i) v |= static_cast<T>(p[i]) << (i*8);
    return v;
}

} // namespace

uint32_t WalManager::crc32c(const void* data, size_t len) noexcept {
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) crc = crc32c_byte(crc, p[i]);
    return crc ^ 0xFFFFFFFFu;
}

WalManager::WalManager(WalConfig cfg) : cfg_(std::move(cfg)) {
    if (!cfg_.enabled) return;
    open_or_create();
    if (cfg_.fsync_mode == WalFsyncMode::FdatasyncInterval) {
        fsync_thread_ = std::thread([this]{ fsync_loop(); });
    }
}

WalManager::~WalManager() {
    stop_.store(true, std::memory_order_release);
    fsync_cv_.notify_all();
    if (fsync_thread_.joinable()) fsync_thread_.join();
    if (fd_ >= 0) {
        // Final fdatasync so anything appended after the last group commit
        // tick still lands on disk before we exit.
        ::fdatasync(fd_);
        ::close(fd_);
        fd_ = -1;
    }
}

void WalManager::open_or_create() {
    std::error_code ec;
    std::filesystem::create_directories(cfg_.directory, ec);
    path_ = cfg_.directory / kWalFileName;

    fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd_ < 0) {
        throw std::runtime_error(std::string("wal: open failed: ") + std::strerror(errno));
    }
    struct stat st{};
    if (::fstat(fd_, &st) < 0) {
        ::close(fd_); fd_ = -1;
        throw std::runtime_error("wal: fstat failed");
    }
    if (st.st_size == 0) {
        // Fresh file: write magic header.
        uint64_t magic = kFileMagic;
        if (::write(fd_, &magic, sizeof magic) != sizeof magic) {
            ::close(fd_); fd_ = -1;
            throw std::runtime_error("wal: failed to write magic");
        }
        current_offset_ = sizeof magic;
    } else {
        // Existing file. Validate magic; if mismatch, treat as corrupt
        // and refuse to start — better than silently losing data.
        uint64_t magic = 0;
        if (::pread(fd_, &magic, sizeof magic, 0) != static_cast<ssize_t>(sizeof magic) ||
            magic != kFileMagic) {
            ::close(fd_); fd_ = -1;
            throw std::runtime_error("wal: magic mismatch — refusing to open");
        }
        current_offset_ = static_cast<uint64_t>(st.st_size);
    }
    ::lseek(fd_, current_offset_, SEEK_SET);
    durable_offset_.store(current_offset_, std::memory_order_relaxed);
}

void WalManager::write_record_unlocked(const WalRecord& rec, uint64_t seq,
                                       uint64_t& out_offset)
{
    // Serialise inner payload (everything except the 8-byte header) so we
    // can checksum it as a contiguous range and emit the total_size + crc
    // at the top.
    std::string body;
    body.reserve(64 + rec.primary_key.size() + rec.row_bytes.size()
                 + rec.idempotency_key.size());
    put_le<uint64_t>(body, seq);
    put_le<uint64_t>(body, rec.endpoint_id);
    put_le<uint64_t>(body, rec.table_id);
    body.push_back(static_cast<char>(rec.op));
    put_le<uint64_t>(body, rec.row_version);
    auto ts_us = std::chrono::duration_cast<std::chrono::microseconds>(
                   rec.timestamp.time_since_epoch()).count();
    put_le<int64_t>(body, ts_us);
    put_le<uint32_t>(body, static_cast<uint32_t>(rec.primary_key.size()));
    body.append(rec.primary_key);
    put_le<uint32_t>(body, static_cast<uint32_t>(rec.idempotency_key.size()));
    body.append(rec.idempotency_key);
    put_le<uint32_t>(body, static_cast<uint32_t>(rec.row_bytes.size()));
    body.append(rec.row_bytes);

    uint32_t crc = cfg_.checksum
        ? crc32c(body.data(), body.size())
        : 0u;

    // Header: total_size (incl self) | crc
    uint32_t total_size = static_cast<uint32_t>(8 + body.size());
    std::string header;
    header.reserve(8);
    put_le<uint32_t>(header, total_size);
    put_le<uint32_t>(header, crc);

    // writev so the kernel sees a single atomic append. POSIX guarantees
    // a single write() ≤ PIPE_BUF on a regular file is atomic; for larger
    // writes, atomicity isn't guaranteed but we hold write_mtx_ around
    // every append so concurrent appenders never interleave bytes.
    iovec iov[2] = {
        {const_cast<char*>(header.data()), header.size()},
        {const_cast<char*>(body.data()),   body.size()},
    };
    ssize_t want = static_cast<ssize_t>(header.size() + body.size());
    ssize_t got  = ::writev(fd_, iov, 2);
    if (got != want) {
        throw std::runtime_error(
            std::string("wal: short write: ") + std::strerror(errno));
    }

    out_offset = current_offset_;
    current_offset_ += static_cast<uint64_t>(got);
    pending_bytes_.fetch_add(static_cast<uint64_t>(got), std::memory_order_relaxed);
}

WalDurabilityHandle WalManager::append(const WalRecord& rec) {
    if (!cfg_.enabled) {
        WalDurabilityHandle h;
        h.durable = std::make_shared<std::atomic<bool>>(true);  // pretend
        return h;
    }

    uint64_t seq = appended_seq_.fetch_add(1, std::memory_order_relaxed) + 1;
    uint64_t offset = 0;
    WalDurabilityHandle h;
    h.sequence = seq;
    h.durable  = std::make_shared<std::atomic<bool>>(false);

    {
        std::lock_guard lk(write_mtx_);
        write_record_unlocked(rec, seq, offset);
        h.offset = current_offset_;   // durable once durable_offset_ >= this

        if (cfg_.fsync_mode == WalFsyncMode::FsyncAlways) {
            ::fsync(fd_);
            durable_offset_.store(current_offset_, std::memory_order_release);
            h.durable->store(true, std::memory_order_release);
        } else if (cfg_.fsync_mode == WalFsyncMode::FdatasyncAlways) {
            ::fdatasync(fd_);
            durable_offset_.store(current_offset_, std::memory_order_release);
            h.durable->store(true, std::memory_order_release);
        } else {
            // Group commit / os-buffered: queue the waiter; the fsync
            // thread (or, for os-buffered, a periodic flush by the
            // kernel) will mark it durable later.
            waiters_.push_back({h.offset, h.durable});
        }
    }
    fsync_cv_.notify_one();
    return h;
}

void WalManager::do_fdatasync_unlocked() {
    // Called while holding write_mtx_. fdatasync the file, then flip every
    // waiter whose offset is ≤ the now-durable current_offset_.
    uint64_t durable_at = current_offset_;
    ::fdatasync(fd_);
    durable_offset_.store(durable_at, std::memory_order_release);
    auto it = waiters_.begin();
    while (it != waiters_.end()) {
        if (it->offset <= durable_at) {
            it->durable->store(true, std::memory_order_release);
            it = waiters_.erase(it);
        } else {
            ++it;
        }
    }
}

void WalManager::fsync_loop() {
    using namespace std::chrono;
    auto interval = milliseconds(std::max<uint32_t>(1, cfg_.fsync_interval_ms));
    while (!stop_.load(std::memory_order_acquire)) {
        std::unique_lock lk(write_mtx_);
        // Wait until either: stop, or a waiter has been queued, or the
        // interval elapses. Either way we then fdatasync.
        fsync_cv_.wait_for(lk, interval, [this]{
            return stop_.load(std::memory_order_acquire) || !waiters_.empty();
        });
        if (stop_.load(std::memory_order_acquire) && waiters_.empty()) return;
        do_fdatasync_unlocked();
    }
}

bool WalDurabilityHandle::wait_for(std::chrono::milliseconds timeout) const {
    using namespace std::chrono;
    auto deadline = steady_clock::now() + timeout;
    while (!durable->load(std::memory_order_acquire)) {
        if (steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(milliseconds(1));
    }
    return true;
}

void WalManager::mark_flushed(uint64_t seq_inclusive) noexcept {
    uint64_t cur = flushed_seq_.load(std::memory_order_relaxed);
    while (seq_inclusive > cur &&
           !flushed_seq_.compare_exchange_weak(cur, seq_inclusive,
                                               std::memory_order_relaxed)) {}
}

size_t WalManager::compact() {
    // Faz 2 minimal compaction: if every record so far has been flushed,
    // truncate the file back to just the magic header. A real
    // implementation rolls segments and unlinks older ones; that's Faz 4.
    std::lock_guard lk(write_mtx_);
    uint64_t flushed = flushed_seq_.load(std::memory_order_relaxed);
    uint64_t appended = appended_seq_.load(std::memory_order_relaxed);
    if (flushed < appended) return 0;
    uint64_t reclaimed = current_offset_ - sizeof(uint64_t);
    if (reclaimed == 0) return 0;
    if (::ftruncate(fd_, sizeof(uint64_t)) < 0) return 0;
    current_offset_ = sizeof(uint64_t);
    durable_offset_.store(current_offset_, std::memory_order_relaxed);
    pending_bytes_.store(0, std::memory_order_relaxed);
    return reclaimed;
}

void WalManager::replay(ReplayFn fn) {
    if (!cfg_.enabled) return;
    std::lock_guard lk(write_mtx_);

    // Scan from after the magic header to current_offset_. On a corrupted
    // trailing record (truncated write at process crash) we stop the
    // scan but don't fail recovery — the truncated record was never
    // acknowledged to a client so it's safe to ignore.
    uint64_t off = sizeof(uint64_t);
    while (off + 8 <= current_offset_) {
        uint8_t hdr[8];
        if (::pread(fd_, hdr, 8, static_cast<off_t>(off)) != 8) break;
        uint32_t total = get_le<uint32_t>(hdr);
        uint32_t crc   = get_le<uint32_t>(hdr + 4);
        if (total < 8 || off + total > current_offset_) {
            // Truncated tail — stop here. Faz 4 logs this for ops.
            break;
        }
        std::vector<uint8_t> body(total - 8);
        if (::pread(fd_, body.data(), body.size(),
                    static_cast<off_t>(off + 8)) != static_cast<ssize_t>(body.size())) {
            break;
        }
        if (cfg_.checksum) {
            uint32_t actual = crc32c(body.data(), body.size());
            if (actual != crc) break;
        }

        const uint8_t* p = body.data();
        const uint8_t* end = p + body.size();
        WalRecord rec;
        uint64_t seq = get_le<uint64_t>(p); p += 8;
        rec.endpoint_id = get_le<uint64_t>(p); p += 8;
        rec.table_id    = get_le<uint64_t>(p); p += 8;
        rec.op = static_cast<WalOp>(*p++);
        rec.row_version = get_le<uint64_t>(p); p += 8;
        int64_t ts_us   = get_le<int64_t>(p);  p += 8;
        rec.timestamp = std::chrono::system_clock::time_point(
                          std::chrono::microseconds(ts_us));

        auto read_str = [&](std::string& out) -> bool {
            if (p + 4 > end) return false;
            uint32_t len = get_le<uint32_t>(p); p += 4;
            if (p + len > end) return false;
            out.assign(reinterpret_cast<const char*>(p), len);
            p += len;
            return true;
        };
        if (!read_str(rec.primary_key))      break;
        if (!read_str(rec.idempotency_key))  break;
        if (!read_str(rec.row_bytes))        break;

        bool fn_ok = fn(rec);
        if (fn_ok) flushed_seq_.store(seq, std::memory_order_relaxed);
        appended_seq_.store(std::max(appended_seq_.load(std::memory_order_relaxed),
                                      seq),
                            std::memory_order_relaxed);
        off += total;
    }
}

} // namespace spido_pg
