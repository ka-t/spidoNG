// Integration tests against a real PostgreSQL instance. Skipped if the
// environment vars SPIDO_PG_TEST_SOCKET / _USER / _DBNAME aren't set, so
// running `make test` on a CI box without PG doesn't hard-fail.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "spido_pg/connection.h"

#include <cstdlib>
#include <cstring>
#include <string>

using namespace spido_pg;

namespace {

struct PgEnv {
    std::string socket;
    std::string user;
    std::string password;
    std::string dbname;
    bool        present = false;

    PgEnv() {
        const char* s = std::getenv("SPIDO_PG_TEST_SOCKET");
        const char* u = std::getenv("SPIDO_PG_TEST_USER");
        const char* d = std::getenv("SPIDO_PG_TEST_DBNAME");
        const char* p = std::getenv("SPIDO_PG_TEST_PASSWORD");
        if (s && u && d) {
            socket = s; user = u; dbname = d;
            password = p ? p : "";
            present = true;
        }
    }
};
const PgEnv& env() { static PgEnv e; return e; }

#define SKIP_IF_NO_PG()                                                    \
    do { if (!env().present) {                                             \
        MESSAGE("SPIDO_PG_TEST_SOCKET/USER/DBNAME not set — skipping");    \
        return;                                                            \
    } } while (0)

} // namespace

TEST_CASE("PgConnection: connect and SELECT 1") {
    SKIP_IF_NO_PG();
    PgConnection c;
    REQUIRE(c.connect(env().socket, env().user, env().password, env().dbname));
    auto r = c.exec("SELECT 1::int4");
    REQUIRE(r.ok());
    REQUIRE(r.rows.size() == 1);
    auto v = r.i4(0, 0);
    REQUIRE(v.has_value());
    CHECK(*v == 1);
}

TEST_CASE("PgConnection: prepared statement binary roundtrip") {
    SKIP_IF_NO_PG();
    PgConnection c;
    REQUIRE(c.connect(env().socket, env().user, env().password, env().dbname));

    std::vector<Oid> types = { Oid::Int4, Oid::Int8, Oid::Text, Oid::Bool, Oid::Float8 };
    auto h = c.prepare("rt_test",
        "SELECT $1::int4, $2::int8, $3::text, $4::bool, $5::float8",
        types);
    REQUIRE(!h.name.empty());

    std::vector<PgParam> ps = {
        PgParam::i4(0x12345678),
        PgParam::i8(0x1122334455667788LL),
        PgParam::text("héllo"),
        PgParam::b(true),
        PgParam::f8(3.141592653589793),
    };
    auto r = c.exec_prepared(h, ps);
    REQUIRE(r.ok());
    REQUIRE(r.rows.size() == 1);

    CHECK(r.i4(0, 0).value_or(0) == 0x12345678);
    CHECK(r.i8(0, 1).value_or(0) == 0x1122334455667788LL);
    auto tv = r.text(0, 2); REQUIRE(tv.has_value()); CHECK(std::string(*tv) == "héllo");
    CHECK(r.b(0, 3).value_or(false) == true);
    CHECK(r.f8(0, 4).value_or(0.0) == doctest::Approx(3.141592653589793));
}

TEST_CASE("PgConnection: reconnect after close()") {
    SKIP_IF_NO_PG();
    PgConnection c;
    REQUIRE(c.connect(env().socket, env().user, env().password, env().dbname));
    auto r1 = c.exec("SELECT 1");
    REQUIRE(r1.ok());
    c.close();
    CHECK_FALSE(c.is_ready());
    REQUIRE(c.connect(env().socket, env().user, env().password, env().dbname));
    auto r2 = c.exec("SELECT 2");
    REQUIRE(r2.ok());
}

TEST_CASE("PgConnection: NULL in DataRow") {
    SKIP_IF_NO_PG();
    PgConnection c;
    REQUIRE(c.connect(env().socket, env().user, env().password, env().dbname));
    auto r = c.exec("SELECT NULL::int4, 7::int4");
    REQUIRE(r.ok());
    REQUIRE(r.rows.size() == 1);
    CHECK_FALSE(r.i4(0, 0).has_value());
    CHECK(r.i4(0, 1).value_or(0) == 7);
}
