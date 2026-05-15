#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "spido_pg/endpoint_registry.h"

using namespace spido_pg;

TEST_CASE("EndpointRegistry: register and look up policy by id") {
    EndpointRegistry reg;

    EndpointPolicy p;
    p.endpoint_id = 1;
    p.table_id    = 7;
    p.path        = "/users/:id";
    p.method      = "GET";
    p.priority    = Priority::High;
    p.data_model  = DataModel::State;
    reg.register_policy(p);

    auto* fetched = reg.policy(1);
    REQUIRE(fetched != nullptr);
    CHECK(fetched->endpoint_id == 1);
    CHECK(fetched->table_id    == 7);
    CHECK(fetched->path        == "/users/:id");
    CHECK(fetched->priority    == Priority::High);

    CHECK(reg.policy(999) == nullptr);   // unregistered
    CHECK(reg.policy(0)   == nullptr);   // reserved "unknown"
}

TEST_CASE("EndpointRegistry: re-register replaces in place") {
    EndpointRegistry reg;
    EndpointPolicy p; p.endpoint_id = 5; p.path = "/a";
    reg.register_policy(p);
    p.path = "/b";
    reg.register_policy(p);

    CHECK(reg.policy(5)->path == "/b");
    CHECK(reg.count() == 1);
}

TEST_CASE("EndpointRegistry: runtime state allocated lazily, atomic counters") {
    EndpointRegistry reg;
    auto& s = reg.state(42);          // unregistered id → lazy create
    CHECK(s.endpoint_id == 42);
    CHECK(s.total_requests.load() == 0);

    s.record_read(100);
    s.record_read(200);
    s.record_write(50);

    CHECK(s.total_requests.load() == 3);
    CHECK(s.reads.load()  == 2);
    CHECK(s.writes.load() == 1);
    CHECK(s.latency_us_max.load() == 200);
}

TEST_CASE("EndpointRegistry: top-K writers sorts by write_rps") {
    EndpointRegistry reg;
    for (uint32_t id = 1; id <= 10; ++id) {
        EndpointPolicy p; p.endpoint_id = id; reg.register_policy(p);
        reg.state(id).write_rps.store(static_cast<double>(id));
    }
    auto top = reg.top_k_writers(3);
    REQUIRE(top.size() == 3);
    CHECK(top[0].id == 10);
    CHECK(top[1].id == 9);
    CHECK(top[2].id == 8);
}

TEST_CASE("EndpointRegistry: out-of-range ids handled gracefully") {
    EndpointRegistry reg;
    EndpointPolicy p; p.endpoint_id = EndpointRegistry::kMaxEndpoints + 1;
    reg.register_policy(p);                    // silently ignored
    CHECK(reg.count() == 0);

    auto& fallback = reg.state(0);             // kEndpointUnknown
    CHECK(&fallback != nullptr);               // returns singleton
    fallback.record_read(10);                  // no crash
}
