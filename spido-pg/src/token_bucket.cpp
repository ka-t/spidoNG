#include "spido_pg/token_bucket.h"

#include <cstring>

namespace spido_pg {

uint64_t TokenBucket::pack(double d) noexcept {
    uint64_t u;
    std::memcpy(&u, &d, sizeof u);
    return u;
}

double TokenBucket::unpack(uint64_t u) noexcept {
    double d;
    std::memcpy(&d, &u, sizeof d);
    return d;
}

void TokenBucket::reset(double rate_per_sec, double capacity) noexcept {
    if (capacity < 0)     capacity = 0;
    if (rate_per_sec < 0) rate_per_sec = 0;

    double old_cap = capacity_.load(std::memory_order_relaxed);
    rate_.store(rate_per_sec,    std::memory_order_relaxed);
    capacity_.store(capacity,    std::memory_order_relaxed);

    // Token-level policy on reset:
    //   - Fresh bucket (old_cap == 0): start full at the new capacity.
    //     This is the burst-friendly default — a brand-new endpoint can
    //     immediately serve up to capacity requests without waiting for
    //     the refill clock to catch up.
    //   - Existing bucket: preserve the live level, clamped DOWN to the
    //     new capacity. We don't grant freebies on retune.
    uint64_t cur = tokens_packed_.load(std::memory_order_relaxed);
    double cur_d = unpack(cur);
    if (old_cap == 0) {
        cur_d = capacity;
    } else if (cur_d > capacity) {
        cur_d = capacity;
    }
    tokens_packed_.store(pack(cur_d), std::memory_order_relaxed);
    last_refill_us_.store(now_us(), std::memory_order_relaxed);
}

void TokenBucket::refill_inplace(double& tokens, int64_t now) const noexcept {
    int64_t last = last_refill_us_.load(std::memory_order_relaxed);
    if (now <= last) return;
    double rate = rate_.load(std::memory_order_relaxed);
    if (rate <= 0) return;
    double cap  = capacity_.load(std::memory_order_relaxed);
    double add  = static_cast<double>(now - last) * rate / 1'000'000.0;
    tokens += add;
    if (tokens > cap) tokens = cap;
}

bool TokenBucket::try_acquire(double cost) noexcept {
    if (cost <= 0) return true;
    double cap = capacity_.load(std::memory_order_relaxed);
    if (cap <= 0) {
        // capacity=0 means "no budget allocated yet" — happens at startup
        // before the pressure controller has run its first tick. Block
        // until reset() is called by returning false; the caller will
        // fall to overload behaviour (typically Sync→pool which has its
        // own checkout queue).
        return false;
    }
    constexpr int kMaxRetries = 8;
    for (int i = 0; i < kMaxRetries; ++i) {
        int64_t  now   = now_us();
        uint64_t cur_u = tokens_packed_.load(std::memory_order_acquire);
        double   cur   = unpack(cur_u);
        refill_inplace(cur, now);

        if (cur < cost) {
            // Persist the refilled (but-not-consumed) value so other
            // callers see the recent fill, and update last_refill_us_.
            // Best effort: if CAS fails, another writer beat us to it
            // with their own refill — both views are equally valid.
            uint64_t new_u = pack(cur);
            tokens_packed_.compare_exchange_weak(
                cur_u, new_u, std::memory_order_release, std::memory_order_relaxed);
            last_refill_us_.store(now, std::memory_order_relaxed);
            return false;
        }

        double   after   = cur - cost;
        uint64_t after_u = pack(after);
        if (tokens_packed_.compare_exchange_weak(
                cur_u, after_u,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            last_refill_us_.store(now, std::memory_order_relaxed);
            return true;
        }
        // CAS failed → another acquirer/refiller raced us; retry.
    }
    // Hit the retry cap. Backoff is the caller's problem — for our
    // workload (writers behind a per-endpoint TB hit by a single worker
    // thread at a time in the common case) we never get this far.
    return false;
}

double TokenBucket::current_tokens() const noexcept {
    uint64_t u = tokens_packed_.load(std::memory_order_relaxed);
    double cur = unpack(u);
    refill_inplace(cur, now_us());
    return cur;
}

} // namespace spido_pg
