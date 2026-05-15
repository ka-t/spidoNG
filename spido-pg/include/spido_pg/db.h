#pragma once

#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "spido_pg/batch_writer.h"
#include "spido_pg/cache.h"
#include "spido_pg/connection.h"
#include "spido_pg/endpoint_registry.h"
#include "spido_pg/entity_cache.h"
#include "spido_pg/notify.h"
#include "spido_pg/pool.h"
#include "spido_pg/pressure_controller.h"
#include "spido_pg/token_bucket.h"
#include "spido_pg/wal.h"

namespace spido_pg {

struct DbConfig {
    PoolConfig         pool;
    CacheConfig        cache;
    BatchWriterConfig  batch;
    PressureConfig     pressure;
    EntityCacheConfig  entity;
    WalConfig          wal;

    // When true, Db spawns a PgNotify subscribed to "spido_pg_invalidate"
    // and routes payloads to QueryCache::invalidate_table. Tables that emit
    // notifications via NOTIFY/triggers will then transparently invalidate
    // dependent cache entries.
    bool               enable_invalidation_listener = true;

    // When true, the pressure controller runs in the background. Disable
    // for tests / tools that don't want the extra thread.
    bool               enable_pressure_controller = true;

    // Default TTL for query_cached() callers that pass zero.
    std::chrono::seconds default_ttl{5};
};

// One-stop facade.
//
//   Db db(cfg);
//   auto r = db.query("SELECT 1");
//   auto r = db.query_cached("SELECT * FROM users WHERE id=$1",
//                            {PgParam::i4(42)}, 30s, "users");
//   db.write("events", {PgParam::i8(...), PgParam::text(...)});
//
// Lifetime: pool, cache, batch writer, notify listener are all owned and
// shut down in destructor order.
class Db {
public:
    explicit Db(DbConfig cfg);
    ~Db();

    Db(const Db&)            = delete;
    Db& operator=(const Db&) = delete;

    PgResult query(std::string_view sql);
    PgResult query(std::string_view sql, std::span<const PgParam> params);

    PgResult query_cached(std::string_view sql,
                          std::span<const PgParam> params,
                          std::chrono::seconds ttl,
                          std::string_view table_tag);

    // Async write — enqueued in the batch buffer keyed by `endpoint` (HTTP
    // path or any string the dynamic-priority controller should track).
    // Returns immediately. The cache is marked dirty for `table` until
    // the underlying flush ACKs, so reads can't observe pre-write state.
    void write(std::string_view endpoint,
               std::string_view table,
               std::vector<PgParam> row);
    // Blocks until the row has been flushed (or DLQ'd on hard failure).
    void write_sync(std::string_view table, std::vector<PgParam> row);

    PgPool&               pool()         noexcept { return *pool_;         }
    QueryCache&           cache()        noexcept { return *cache_;        }
    BatchWriter&          batch()        noexcept { return *batch_;        }
    EndpointRegistry&     endpoints()    noexcept { return *endpoints_;    }
    PgPressureController& pressure()     noexcept { return *pressure_;     }
    EntityCache&          entity_cache() noexcept { return *entity_cache_; }
    WalManager&           wal()          noexcept { return *wal_;          }

    const DbConfig& config() const noexcept { return cfg_; }

    // ram_entity_first read path: look up a single row in the entity
    // cache; on miss, fall through to PG, populate, and return.
    // table_id is the numeric handle from the generated policy; pk is
    // the raw payload that goes into a PG prepared statement.
    EntityRowPtr get_entity(TableId table, std::string_view pk,
                            std::string_view sql_on_miss,
                            std::span<const PgParam> params);

private:
    DbConfig                                  cfg_;
    std::unique_ptr<PgPool>                   pool_;
    std::unique_ptr<QueryCache>               cache_;
    std::unique_ptr<EntityCache>              entity_cache_;
    std::unique_ptr<EndpointRegistry>         endpoints_;
    std::unique_ptr<PgPressureController>     pressure_;
    std::unique_ptr<WalManager>               wal_;
    std::unique_ptr<BatchWriter>              batch_;
    std::unique_ptr<PgNotify>                 notify_;
};

} // namespace spido_pg
