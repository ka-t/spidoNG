#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "spido_pg/pressure_controller.h"
#include "spido_pg/pool.h"
#include "spido_pg/endpoint_registry.h"

#include <chrono>
#include <cstdlib>
#include <thread>

using namespace spido_pg;
using namespace std::chrono_literals;

namespace {

PoolConfig dummy_pool_cfg() {
    PoolConfig c;
    // Point at a socket that definitely won't accept — we never actually
    // connect; we just need PgPool's constructor to compute min/max.
    c.socket_path  = "/tmp/spido_pg_test_nonexistent.sock";
    c.user         = "x";
    c.dbname       = "x";
    c.min_conns    = 0;     // skip eager fill
    c.max_conns    = 4;
    c.health_check_s = 0;   // no background pings
    c.connect_timeout_s = 0;
    return c;
}

} // namespace

TEST_CASE("PgPressureController: starts in HEALTHY before any signal") {
    PgPool pool(dummy_pool_cfg());
    EndpointRegistry reg;
    PressureConfig pc; pc.decision_interval_ms = 50;
    PgPressureController ctrl(pool, reg, pc);
    CHECK(ctrl.state() == PressureState::Healthy);
}

TEST_CASE("PgPressureController: classifies CRITICAL on high p95 latency") {
    PgPool pool(dummy_pool_cfg());
    EndpointRegistry reg;
    PressureConfig pc;
    pc.decision_interval_ms = 50;
    pc.db_p95_critical_ms   = 100.0;
    PgPressureController ctrl(pool, reg, pc);
    ctrl.start();

    // Drive sustained high latency across multiple ticks. Sticky max
    // ensures we don't relax back to HEALTHY between ticks.
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 5; ++i) ctrl.record_pg_latency(200'000);
        std::this_thread::sleep_for(40ms);
    }
    CHECK(ctrl.state() == PressureState::Critical);
    ctrl.stop();
}

TEST_CASE("PgPressureController: token bucket budgets shrink under pressure") {
    PgPool pool(dummy_pool_cfg());
    EndpointRegistry reg;
    PressureConfig pc; pc.decision_interval_ms = 50;
    PgPressureController ctrl(pool, reg, pc);

    EndpointPolicy p; p.endpoint_id = 1; p.priority = Priority::Low;
    reg.register_policy(p);
    ctrl.start();

    // Wait one tick so HEALTHY budget is applied.
    std::this_thread::sleep_for(80ms);
    double healthy_rate = ctrl.token_bucket_for(1).rate_per_sec();
    CHECK(healthy_rate > 0);

    // Drive CRITICAL via sustained high latency across several ticks.
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 5; ++i) ctrl.record_pg_latency(500'000);
        std::this_thread::sleep_for(40ms);
    }
    double crit_rate = ctrl.token_bucket_for(1).rate_per_sec();
    // Low-priority endpoint should have zero or near-zero budget at CRITICAL.
    CHECK(crit_rate < healthy_rate);
    ctrl.stop();
}

TEST_CASE("PgPressureController: error rate alone can drive PRESSURED") {
    PgPool pool(dummy_pool_cfg());
    EndpointRegistry reg;
    PressureConfig pc;
    pc.decision_interval_ms = 40;
    pc.error_rate_pressured = 0.10;
    PgPressureController ctrl(pool, reg, pc);
    ctrl.start();

    // Sustain a 30% error rate across multiple ticks.
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 7; ++i) ctrl.record_pg_latency(1000);
        for (int i = 0; i < 3; ++i) ctrl.record_pg_error();
        std::this_thread::sleep_for(50ms);
    }
    auto s = ctrl.state();
    CHECK((s == PressureState::Pressured || s == PressureState::Overloaded
        || s == PressureState::Critical));
    ctrl.stop();
}
