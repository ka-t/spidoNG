#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace spido {

// SPSC ring with fixed-capacity, fixed-line-width records. One producer
// (the worker thread), one consumer (the access-log writer thread). The
// hot path formats one line straight into a record slot — no malloc,
// no system call.
class AccessLogRing {
public:
    static constexpr size_t kLineMax = 384;  // typical combined-log line
    explicit AccessLogRing(size_t entries);

    char*  reserve() noexcept;     // returns slot ptr or nullptr if full
    void   commit() noexcept;      // call after writing terminator into slot

    // Consumer side. `peek_at(i)` returns the entry `i` slots ahead of
    // the current tail without advancing — letting the writer batch many
    // entries into one writev. `consume(n)` advances tail by n.
    const char* peek_at(size_t i, size_t& len) noexcept;
    size_t      pending() const noexcept;
    void        consume(size_t n) noexcept;

private:
    size_t capacity_mask_;
    std::vector<char>            buf_;   // capacity_ * kLineMax bytes
    std::vector<uint16_t>        lens_;  // line length per slot
    alignas(64) std::atomic<size_t> head_{0};  // producer-write
    alignas(64) std::atomic<size_t> tail_{0};  // consumer-read
};

// Background drain thread. One AccessLogger owns N rings (one per worker).
class AccessLogger {
public:
    static std::unique_ptr<AccessLogger>
    create(const std::string& path, size_t workers, bool drop_on_full);

    ~AccessLogger();
    AccessLogger(const AccessLogger&)            = delete;
    AccessLogger& operator=(const AccessLogger&) = delete;

    AccessLogRing& ring(size_t worker_idx) { return *rings_[worker_idx]; }
    bool drop_on_full() const noexcept { return drop_on_full_; }
    void stop();

private:
    AccessLogger() = default;
    void writer_loop();

    int                             fd_ = -1;
    bool                            drop_on_full_ = true;
    std::atomic<bool>               stop_{false};
    // unique_ptr because AccessLogRing has atomic members → not movable
    // and vector::reserve requires move-constructibility.
    std::vector<std::unique_ptr<AccessLogRing>> rings_;
    std::thread                     thread_;
};

} // namespace spido
