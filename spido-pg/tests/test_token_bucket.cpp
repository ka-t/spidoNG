#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "spido_pg/token_bucket.h"

#include <chrono>
#include <thread>

using namespace spido_pg;
using namespace std::chrono_literals;

TEST_CASE("TokenBucket: zero capacity returns false") {
    TokenBucket b;                  // default: rate=0, capacity=0
    CHECK_FALSE(b.try_acquire());
    CHECK_FALSE(b.try_acquire(0.5));
}

TEST_CASE("TokenBucket: capacity is the starting budget after reset") {
    TokenBucket b(1000.0, 5.0);
    // No new tokens have accrued (clock hasn't moved much), but capacity
    // worth IS allowed by reset() — verify by draining.
    int got = 0;
    for (int i = 0; i < 100; ++i) if (b.try_acquire()) ++got;
    // We expect to drain capacity (~5) plus a few accrued during the
    // ~µs the loop took at 1000 tok/s.
    CHECK(got >= 5);
    CHECK(got < 50);
}

TEST_CASE("TokenBucket: refills at configured rate over time") {
    TokenBucket b(1000.0, 1.0);
    // Drain.
    while (b.try_acquire()) {}
    CHECK_FALSE(b.try_acquire());
    // Wait ~5 ms: at 1000 tok/s we get ~5 tokens — capped at capacity=1.
    std::this_thread::sleep_for(5ms);
    CHECK(b.try_acquire());
}

TEST_CASE("TokenBucket: reset() preserves token level up to new capacity") {
    TokenBucket b(100.0, 100.0);
    while (b.try_acquire(10.0)) {}     // drain
    b.reset(200.0, 200.0);             // capacity went up
    // No instant refill, but live level was preserved (~0); waiting
    // ~10 ms at 200/s gives ~2 tokens.
    std::this_thread::sleep_for(15ms);
    CHECK(b.try_acquire(1.0));
}

TEST_CASE("TokenBucket: bulk cost respected") {
    TokenBucket b(1.0, 10.0);
    CHECK(b.try_acquire(5.0));        // 10 - 5 = 5 left
    CHECK(b.try_acquire(5.0));        //  5 - 5 = 0 left
    CHECK_FALSE(b.try_acquire(1.0));  // empty now
}
