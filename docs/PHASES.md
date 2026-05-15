# spido-pg — Production Roadmap

This file is the agreement between us about scope. Each phase is one
working session; we don't move on until the current phase compiles
clean (`-Wall -Wextra -Werror -O3`), tests pass, and we both think it's
done. Anything not in the current phase is explicitly **deferred** —
mention "Faz X" in a TODO/comment and move on, don't half-build it.

Current state (baseline before Faz 1):
- libspido_pg.a builds clean
- PG wire protocol v3 over io_uring (MD5 auth only; SCRAM in Faz 4)
- PgPool with health check + prepared statement cache
- L1 hash-keyed (~70 ns) + L2 lock-free probe cache (~780 ns)
- Dirty bit + flush-before-evict semantics
- BatchWriter with 250ms dynamic priority controller + PG load EMA
- LISTEN/NOTIFY → cache invalidate listener
- Python generator with full CRUD body parsing (POST/PUT/DELETE)
- 21 cache assertions + 8 generator pytest cases passing

---

## Faz 1 — Endpoint awareness + pressure-driven traffic shaping

**Goal:** Make every write/read aware of which endpoint it came from,
let one bad endpoint not eat the whole pool, react to PG pressure by
retuning batch sizes and admission.

### New components
- `include/spido_pg/endpoint_registry.h` + `src/endpoint_registry.cpp`
  - `EndpointId` = `uint32_t` (numeric, generator-assigned)
  - `EndpointPolicy` — full struct from prompt's spec
  - `EndpointRuntimeState` — atomic counters for rps, latency, queue
    depth, cache hits/misses, errors. Per-endpoint, no shard-wide
    locks; use atomics + periodic aggregation.
  - `EndpointRegistry` — register by id, lookup by id, iterate all,
    top-K hot tracking.
- `include/spido_pg/token_bucket.h` + `src/token_bucket.cpp`
  - Per-endpoint token bucket with refill rate driven by pressure.
  - `try_acquire()` returns bool, never blocks.
- `include/spido_pg/pressure_controller.h` + `src/pressure_controller.cpp`
  - `PressureState` enum: HEALTHY, WARM, PRESSURED, OVERLOADED, CRITICAL
  - Observes: PG p95 latency, pool saturation, batch queue depth,
    error rate. EWMA + sliding window.
  - Background thread, configurable `decision_interval_ms` (default 250).
  - Reclassifies state, updates token bucket refill rates, retunes
    BatchWriter scale.
- `DataModel { State, Event }` in `BatchWriter`:
  - State: coalesce by primary key, emit UPSERT with
    `WHERE table.version <= EXCLUDED.version`.
  - Event: preserve all, plain multi-row INSERT.

### Wiring
- `DbConfig` grows: `EndpointRegistry`, `PressureController`,
  `TokenBucket`s.
- `Db::write(endpoint, table, row)` now consults endpoint policy →
  picks write mode → optionally rate-limits via token bucket →
  routes to BatchWriter with correct data_model.
- BatchWriter consumes pressure state in its existing 250ms tick.

### Generator changes
- `config.json` resource entries grow new fields:
  `data_model`, `priority`, `consistency_level`, `read_mode`,
  `write_mode`, `overload_behavior`, etc. (Sensible defaults if
  missing.)
- Emit `generated_policies.h` with a static array of
  `EndpointPolicy`, indexed by numeric `EndpointId`.
- Generated handlers look up policy via `endpoint_id` and call into
  the right write/read path.

### Tests
- `tests/test_endpoint_registry.cpp` — register / lookup / top-K
- `tests/test_token_bucket.cpp` — refill / drain / saturation
- `tests/test_pressure_controller.cpp` — state transitions under
  synthetic latency, batch retune on state change
- `tests/test_batch_writer.cpp` — state coalescing test case
  (UPSERT path), event preservation test case
- `tests/test_generator.py` — policy emission, numeric id stability

### Explicitly deferred to later phases
- WAL (Faz 2)
- EntityCache hotset, preload, NOTIFY reconciliation (Faz 3)
- SCRAM auth (Faz 4)
- Memory arenas / slab allocator (Faz 4 if needed)
- 10k endpoint burst benchmark (Faz 4)
- TSan clean (Faz 4)
- Debug HTTP endpoints (Faz 4)
- OpenAPI emission (Faz 4)

### Done when
- New components compile clean, `-Wall -Wextra -Werror`
- All existing tests still pass
- New tests pass offline (PG-required tests skip cleanly)
- Generator emits policy struct and generated service builds against
  the new headers

---

## Faz 2 — WAL + durable async writes

**Goal:** Acknowledged writes survive a crash. `batch_durable` and
`async_durable` actually mean what they say.

### New components
- `include/spido_pg/wal.h` + `src/wal.cpp`
  - WAL file format: magic, version, record_size, checksum (CRC32C),
    endpoint_id, table_id, op, primary_key, row, timestamp,
    row_version, idempotency_key, prev_offset.
  - `WalManager`:
    - `append(record) → wal_offset`
    - Group commit with `fdatasync_interval_ms` (default 2ms)
    - Per-record durability promise via `wait_for_durable(offset)`
    - Background flusher coordinates with BatchWriter to mark
      records flushed after PG ack
    - Compaction: drop records older than last-flushed checkpoint
    - Recovery: scan files on startup, validate checksums, stop on
      corrupted trailing record, coalesce state ops by (table, pk),
      preserve event ops, replay through BatchWriter
- BatchWriter learns about WAL: on flush success, marks WAL records
  flushed; on retry exhaust, the record stays in WAL pending DLQ
  resolution.

### Tests
- `tests/test_wal.cpp` — append, checksum, corrupted-tail recovery,
  group commit timing, replay, compaction.
- Crash simulation: fork a child that appends, kill -9, parent
  reopens and verifies recovery.

### Explicitly deferred
- WAL segment rotation by size (start with one growing file)
- WAL compression
- WAL encryption

### Done when
- Durable mode writes survive `kill -9` + restart
- ASan clean
- Recovery replays only un-flushed records
- WAL compaction frees disk

---

## Faz 3 — EntityCache + ram_entity_first

**Goal:** Hot ID-keyed reads served from RAM. State table writes go
to RAM first, PG eventually.

### New components
- `include/spido_pg/entity_cache.h` + `src/entity_cache.cpp`
  - `EntityCacheEntry`: row data, version, db_version, dirty,
    flushing, valid, stale, tombstone, pinned, wal_offset.
  - Keyed by `(table_id, primary_key)` — primary key may be int8 or
    text variant.
  - Hotset: pin first N rows per table, dynamic promotion of top-K
    accessed.
  - Preload at startup via `preload_query`.
  - State vs event semantics:
    - State: same-pk write replaces, never coalesce already-flushed
      rows of different versions out of order
    - Event: append-only queue per table_id
  - NOTIFY reconciliation: payload `{table, pk, op, version}` →
    invalidate iff incoming version > local; never overwrite newer
    local dirty state.

### Wiring
- `Db::get_entity(table, key, policy)` — RAM lookup first, PG miss
  fallback through PgPool.
- `Db::write_*` updates entity cache + WAL + batch.

### Generator changes
- `read_mode: ram_entity_first` actually does what it says: handler
  calls `db.get_entity(...)` instead of `query_cached(...)`.
- Generator emits typed row decoders/encoders per resource.

### Tests
- `tests/test_entity_cache.cpp` — coalescing, preservation, dirty
  retention, hotset promotion, version-ordered reconciliation,
  tombstone behavior.

### Done when
- GET by id can be served from RAM with zero PG round-trip
- Stress test: 1k writes/sec to same pk don't multiply PG writes
- NOTIFY doesn't roll back local newer state

---

## Faz 4 — Hardening, ops, polish

### Tasks
- SCRAM-SHA-256 auth in `connection.cpp` (current MD5 stays as fallback)
- Debug HTTP endpoints:
  `/health`, `/metrics`, `/debug/{endpoints,cache,entity-cache,batch,wal,pressure}`
- Generator: numeric endpoint/table IDs throughout, prepared SQL
  constants emitted as `static constexpr`, OpenAPI emission
- `benchmarks/`:
  - `bench_burst.cpp` — 10k endpoints + hot endpoint isolation
  - `bench_wal.cpp`, `bench_entity_cache.cpp`, `bench_pool.cpp`
- TSan clean pass (or documented annotations for known-safe races)
- Memory: switch entity cache and WAL record allocation to a slab
  allocator if benchmarks show malloc churn
- Object pools for protocol message buffers in connection.cpp

### Done when
- TSan clean OR every suppression has a written justification
- 10k endpoint burst test demonstrates pressure controller working
- Critical-priority endpoint p99 latency stays bounded under load
- Best_effort endpoint degrades first, not critical

---

## Working agreement

- One phase per session unless we agree otherwise.
- If a phase is going long, we split it and write the cut into this
  file before the session ends.
- `-Werror` stays on; if a warning blocks us we fix it, not silence
  it.
- New tests written in the same session as the new code.
- Existing tests must continue to pass — no regressions.
