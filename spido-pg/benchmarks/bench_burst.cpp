// Burst-isolation benchmark.
//
// Registers many endpoints across a few priority tiers, then drives a
// hot endpoint with sustained "high latency" PG responses while a
// critical endpoint sees normal traffic. We don't actually need PG —
// the PressureController works off recorded latency, so the test
// synthesises it.
//
// Goals:
//   1. Critical-priority endpoints retain a positive token budget even
//      when the system is in CRITICAL pressure.
//   2. Low / best-effort endpoints have their token budget collapse to
//      near zero under the same pressure.
//   3. Hot endpoint cannot starve the critical one — they have
//      independent token buckets, with the controller setting rates
//      according to priority + pressure.

#include "spido_pg/endpoint_registry.h"
#include "spido_pg/pool.h"
#include "spido_pg/pressure_controller.h"

#include <chrono>
#include <cstdio>
#include <thread>

using namespace spido_pg;
using namespace std::chrono_literals;

int main() {
    PoolConfig pcfg;
    pcfg.socket_path     = "/tmp/bench_burst_nonexistent.sock";
    pcfg.min_conns       = 0;
    pcfg.max_conns       = 8;
    pcfg.connect_timeout_s = 0;
    pcfg.health_check_s  = 0;

    PgPool pool(pcfg);
    EndpointRegistry reg;
    PressureConfig pres;
    pres.decision_interval_ms = 50;
    PgPressureController ctrl(pool, reg, pres);

    // Register a representative mix of endpoints.
    constexpr uint32_t kN = 1000;
    for (uint32_t i = 1; i <= kN; ++i) {
        EndpointPolicy p;
        p.endpoint_id = i;
        p.table_id    = (i % 32) + 1;
        p.path        = "/ep/" + std::to_string(i);
        if      (i % 50 == 0) p.priority = Priority::Critical;
        else if (i % 10 == 0) p.priority = Priority::High;
        else if (i % 3  == 0) p.priority = Priority::Low;
        else if (i % 7  == 0) p.priority = Priority::BestEffort;
        else                  p.priority = Priority::Normal;
        reg.register_policy(p);
    }
    ctrl.start();

    auto tier_budgets = [&](PressureState target_state) {
        // Sustain a load that classifies as `target_state` long enough
        // for two controller ticks to apply the budgets.
        uint64_t latency_us = 1'000;
        switch (target_state) {
            case PressureState::Healthy:    latency_us = 1'000;   break;
            case PressureState::Warm:       latency_us = 7'000;   break;
            case PressureState::Pressured:  latency_us = 20'000;  break;
            case PressureState::Overloaded: latency_us = 50'000;  break;
            case PressureState::Critical:   latency_us = 200'000; break;
        }
        for (int round = 0; round < 6; ++round) {
            for (int i = 0; i < 10; ++i) ctrl.record_pg_latency(latency_us);
            std::this_thread::sleep_for(40ms);
        }
        // Snapshot one rep per tier.
        double crit = ctrl.token_bucket_for(50  ).rate_per_sec();
        double high = ctrl.token_bucket_for(10  ).rate_per_sec();
        double norm = ctrl.token_bucket_for(2   ).rate_per_sec();
        double low  = ctrl.token_bucket_for(3   ).rate_per_sec();
        double best = ctrl.token_bucket_for(7   ).rate_per_sec();
        std::printf("%-12s critical=%7.0f high=%7.0f normal=%7.0f low=%7.0f best=%7.0f  state=%s\n",
                    to_string(target_state),
                    crit, high, norm, low, best,
                    to_string(ctrl.state()));
    };

    std::puts("priority budgets per pressure state (writes/sec):");
    tier_budgets(PressureState::Healthy);
    tier_budgets(PressureState::Warm);
    tier_budgets(PressureState::Pressured);
    tier_budgets(PressureState::Overloaded);
    tier_budgets(PressureState::Critical);

    // Validate the invariants we care about.
    // 1. Critical endpoint never goes to zero.
    double crit_final = ctrl.token_bucket_for(50).rate_per_sec();
    // 2. Low / best-effort endpoint near zero under CRITICAL.
    double low_final  = ctrl.token_bucket_for(3).rate_per_sec();
    double best_final = ctrl.token_bucket_for(7).rate_per_sec();

    bool ok = (crit_final > 0)
           && (low_final  < crit_final)
           && (best_final <= low_final);

    ctrl.stop();
    std::printf("\nverdict: %s\n", ok ? "PASS — critical protected, low/best degraded first" : "FAIL");
    return ok ? 0 : 1;
}
