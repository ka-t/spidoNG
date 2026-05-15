#include "spido_pg/db.h"

#include <condition_variable>
#include <mutex>

namespace spido_pg {

Db::Db(DbConfig cfg) : cfg_(std::move(cfg)) {
    pool_         = std::make_unique<PgPool>(cfg_.pool);
    cache_        = std::make_unique<QueryCache>(cfg_.cache);
    entity_cache_ = std::make_unique<EntityCache>(cfg_.entity);
    endpoints_    = std::make_unique<EndpointRegistry>();
    pressure_     = std::make_unique<PgPressureController>(*pool_, *endpoints_, cfg_.pressure);
    wal_          = std::make_unique<WalManager>(cfg_.wal);
    batch_        = std::make_unique<BatchWriter>(*pool_, cfg_.batch);

    // Wire the dirty-bit pipeline. enqueue() flips the bit; the flush
    // observer clears it once PG ack's. Between those two points the
    // cache refuses to evict entries tagged with this table.
    QueryCache* cache_ref = cache_.get();
    batch_->set_flush_observer([cache_ref](std::string_view t, size_t rows, bool /*ok*/) {
        for (size_t i = 0; i < rows; ++i) cache_ref->mark_clean(t);
        // On a terminal failure (rows landed in the DLQ) we still clear the
        // dirty marker — the alternative is leaking it forever, which would
        // pin cache entries that are now divergent from PG anyway. Tests
        // can subscribe to BatchWriter::stats().failed to detect this.
    });

    // Hook the pressure controller into the batch writer. The controller
    // also pulls batch queue depth via record_batch_queue_depth() — we
    // call it lazily inside the controller's own loop, but the setter
    // here gives it a write channel into the batch scale.
    BatchWriter* batch_ref = batch_.get();
    pressure_->set_batch_scale_setter([batch_ref](double s) {
        batch_ref->set_pressure_scale(s);
    });

    if (cfg_.enable_pressure_controller) pressure_->start();

    if (cfg_.enable_invalidation_listener) {
        notify_ = std::make_unique<PgNotify>(cfg_.pool.socket_path,
                                             cfg_.pool.user,
                                             cfg_.pool.password,
                                             cfg_.pool.dbname);
        notify_->listen("spido_pg_invalidate", [this](std::string_view payload){
            // Payload format:
            //   "<table>"                  → invalidate all keys tagged with that table
            //   "<table>|<pk>|<version>"   → version-aware single-row invalidate
            // We split on '|' so the trigger can send the more
            // informative form. The simple form is the fallback.
            std::string_view table = payload;
            std::string_view pk;
            uint64_t version = 0;
            auto bar1 = payload.find('|');
            if (bar1 != std::string_view::npos) {
                table = payload.substr(0, bar1);
                auto rest = payload.substr(bar1 + 1);
                auto bar2 = rest.find('|');
                if (bar2 != std::string_view::npos) {
                    pk = rest.substr(0, bar2);
                    auto vstr = rest.substr(bar2 + 1);
                    try { version = std::stoull(std::string(vstr)); }
                    catch (...) { version = 0; }
                } else {
                    pk = rest;
                }
            }
            if (table.empty()) {
                for (const auto& [t, _] : cfg_.batch.tables) cache_->invalidate_table(t);
                return;
            }
            // QueryCache invalidation is always safe — entries are
            // immutable and old data is just stale.
            cache_->invalidate_table(table);

            // EntityCache reconciliation is version-aware: we never
            // overwrite newer LOCAL state with an older remote
            // notification. If we don't have a version or pk, do a
            // best-effort eviction by skipping (the next read will
            // refresh through PG).
            if (!pk.empty() && version > 0) {
                // We need a TableId mapping from string. The simplest
                // path is to ask the registry for any endpoint whose
                // table_name matches; tracking it explicitly is Faz 4.
                for (auto id : endpoints_->all_ids()) {
                    const auto* p = endpoints_->policy(id);
                    if (p && p->table_name == table) {
                        entity_cache_->notify_remote_version(p->table_id, pk, version);
                        break;
                    }
                }
            }
        });
        notify_->start();
    }
}

Db::~Db() {
    // Tear down in reverse-dependency order: notify (background recv)
    // before pressure (background tick that touches the registry),
    // batch before pool (batch's flusher still wants pool checkouts),
    // entity_cache + wal before pool too (they coordinate via flush
    // observers; tearing pool first would crash a flush in flight).
    if (notify_)   notify_->stop();
    if (pressure_) pressure_->stop();
    batch_.reset();
    wal_.reset();
    pressure_.reset();
    endpoints_.reset();
    entity_cache_.reset();
    cache_.reset();
    pool_.reset();
}

EntityRowPtr Db::get_entity(TableId table, std::string_view pk,
                            std::string_view sql_on_miss,
                            std::span<const PgParam> params)
{
    if (auto hit = entity_cache_->get(table, pk)) return hit;
    PgResult r = query(sql_on_miss, params);
    if (!r.ok() || r.rows.empty()) return nullptr;

    // Serialise the first row to text-bytes for the entity cache. We
    // store the raw column cells joined by NULs — a real codec is
    // generator-emitted in Faz 4; this minimal version lets the cache
    // return SOMETHING typed for callers that don't yet have decoders.
    EntityRow er;
    er.version = 1;
    for (size_t c = 0; c < r.fields.size(); ++c) {
        if (!r.rows[0].nulls[c]) er.data.append(r.rows[0].cells[c]);
        er.data.push_back('\0');
    }
    entity_cache_->put(table, pk, er, /*dirty=*/false);
    return entity_cache_->get(table, pk);
}

PgResult Db::query(std::string_view sql) {
    return pool_->exec(sql);
}

PgResult Db::query(std::string_view sql, std::span<const PgParam> params) {
    if (params.empty()) return pool_->exec(sql);
    std::vector<Oid> types;
    types.reserve(params.size());
    for (const auto& p : params) types.push_back(p.type);
    // Prepared-statement name is derived from the SQL itself so the same
    // canonical query reuses the same Parse across worker threads.
    std::string stmt_name = "q_" + std::to_string(std::hash<std::string_view>{}(sql));
    return pool_->exec_prepared(stmt_name, sql,
                                std::span<const Oid>(types),
                                params);
}

PgResult Db::query_cached(std::string_view sql,
                          std::span<const PgParam> params,
                          std::chrono::seconds ttl,
                          std::string_view table_tag)
{
    if (ttl.count() == 0) ttl = cfg_.default_ttl;
    auto key = make_cache_key(sql, params);
    // Cache returns shared_ptr<const PgResult>; the public Db API is
    // by-value so we copy once at the boundary. Callers that want to
    // skip the copy (request handlers reading specific fields) can use
    // cache().get() directly and access via the const reference.
    if (auto hit = cache_->get(key)) return *hit;

    PgResult r = query(sql, params);
    if (r.ok()) cache_->put(key, r, ttl, table_tag);
    return r;
}

void Db::write(std::string_view endpoint,
               std::string_view table,
               std::vector<PgParam> row)
{
    cache_->mark_dirty(table);
    batch_->enqueue(endpoint, table, std::move(row));
}

void Db::write_sync(std::string_view table, std::vector<PgParam> row) {
    cache_->mark_dirty(table);
    batch_->enqueue(table, table, std::move(row));
    batch_->flush_now(table);
}

} // namespace spido_pg
