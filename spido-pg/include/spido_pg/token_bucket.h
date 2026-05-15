#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace spido_pg {

// Lock-free token bucket. Each call to try_acquire() lazily refills based
// on wall clock — there is no background thread driving it. Refill rate
// and capacity are atomic so the pressure controller can retune them
// without coordinating with callers.
//
// Refill model: tokens accrue at `rate_per_sec` up to `capacity`. Each
// try_acquire(n) does a CAS to consume n tokens if available; on
// failure, returns false without sleeping. Callers are expected to fall
// through to the endpoint's overload behavior (stale cache, 202, 429,
// etc.) rather than block — non-blocking is the whole point.
//
// Limitations we accept for Faz 1:
//   - Tokens are modelled as a floating-point double inside an atomic,
//     packed/unpacked via memcpy. Lock-free on x86 (atomic<uint64_t> is
//     lock-free).
//   - Clock resolution is microseconds via steady_clock; that gives us
//     more than enough headroom for the refill rates we care about
//     (up to ~1M tokens/sec).
//   - The CAS loop bounds the worst-case retries; under heavy contention
//     a caller may see a few CAS failures but never spins indefinitely.
class TokenBucket {
public:
    TokenBucket() noexcept { reset(0.0, 0.0); }
    TokenBucket(double rate_per_sec, double capacity) noexcept {
        reset(rate_per_sec, capacity);
    }

    // Reconfigure both rate and capacity atomically. Existing token level
    // is preserved (clamped to new capacity). Called by the pressure
    // controller every tick.
    void reset(double rate_per_sec, double capacity) noexcept;

    // Try to consume `cost` tokens. Returns true on success, false if the
    // bucket doesn't have enough — caller must take the slow path.
    // `cost` defaults to 1; bulk operations (a batch flush) can pass
    // higher costs to throttle proportional to their PG impact.
    bool try_acquire(double cost = 1.0) noexcept;

    // Read-only snapshot for metrics. Not exact under contention.
    double current_tokens()  const noexcept;
    double rate_per_sec()    const noexcept { return rate_.load(std::memory_order_relaxed); }
    double capacity()        const noexcept { return capacity_.load(std::memory_order_relaxed); }

private:
    // Tokens packed into a uint64_t for atomic CAS. We store the double's
    // bit pattern. Endianness doesn't matter — we always pack/unpack with
    // memcpy through the same atomic.
    static uint64_t pack(double d) noexcept;
    static double   unpack(uint64_t u) noexcept;

    static int64_t now_us() noexcept {
        using namespace std::chrono;
        return duration_cast<microseconds>(
            steady_clock::now().time_since_epoch()).count();
    }

    void refill_inplace(double& tokens, int64_t now_us_val) const noexcept;

    std::atomic<uint64_t> tokens_packed_{0};
    std::atomic<int64_t>  last_refill_us_{0};
    std::atomic<double>   rate_{0};       // tokens per second
    std::atomic<double>   capacity_{0};
};

} // namespace spido_pg
