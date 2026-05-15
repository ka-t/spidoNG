# Config Reference

A single `config.json` defines the **entire** service: which tables are
exposed at which paths, who can read/write them, which fields are
validated, how cache and back-pressure behave, which lifecycle hooks
fire. This document covers every field with examples.

Quick links:
- [Top-level schema](#top-level-schema) — `service`, `database`, `cache`, `auth`, ...
- [Cookbook](#cookbook---typical-scenarios) — "I want X" → "write Y"
- [Resource fields](#resource-fields) — everything inside `resources[]`
- [Validation rules](#validation-rules) — what gets rejected at config-load time
- [Minimum viable config](#minimum-viable-config) — smallest working example

---

## Top-level schema

```json
{
  "service":   {"name": "...", "port": 8080},
  "database":  {"socket_path": "...", "user": "...", "dbname": "...", "min_conns": 8, "max_conns": 32},
  "cache":     {"enabled": true, "max_bytes": 134217728, "default_ttl_s": 30},
  "auth":      {"type": "header" | "jwt", ...},
  "files":     {"enabled": false, ...},
  "push":      {"enabled": false, ...},
  "resources": [ ... ],
  "proxy":     []
}
```

### `service`

| Field | Type | Default | Description |
|---|---|---|---|
| `name` | string | `"service"` | C++ project name, binary name, openapi `info.title` |
| `port` | int | `8080` | HTTP listen port |

### `database`

| Field | Type | Default | Description |
|---|---|---|---|
| `socket_path` | string | `/var/run/postgresql/.s.PGSQL.5432` | PG Unix socket path |
| `user` | string | `postgres` | PG user |
| `password` | string | `""` | PG password (for SCRAM/MD5) |
| `dbname` | string | `postgres` | Database name |
| `min_conns` | int | `0` (= `nproc`) | Minimum pool size |
| `max_conns` | int | `0` (= `nproc * 4`) | Maximum pool size |

### `cache`

| Field | Type | Default | Description |
|---|---|---|---|
| `enabled` | bool | `true` | L1/L2 cache active |
| `max_bytes` | int | `268435456` (256 MB) | Total cache capacity |
| `default_ttl_s` | int | `5` | Used when a resource doesn't override `cache_ttl_s` |

### `auth`

**Two modes are supported.** Default is `"header"` (backwards compatible):

#### Header mode (default)
```json
"auth": {"type": "header"}
```
Assumes a reverse proxy (nginx, Cloudflare Access, etc.) is setting
`X-User-Id` and `X-User-Roles` headers. The generator reads from those.
The server itself does not verify JWTs.

#### JWT mode (recommended for mobile/SPA)
```json
"auth": {
  "type": "jwt",
  "algo": "HS256",
  "secret": "your-256-bit-secret",
  "user_claim": "sub",
  "roles_claim": "roles",
  "leeway_s": 60,
  "issuer": "myapp.com",          // optional, matches JWT iss claim
  "audience": "mobile",            // optional, matches aud claim
  "refresh": {
    "enabled": true,
    "access_ttl_s": 900,
    "refresh_ttl_s": 2592000,
    "users_table": "users",
    "email_column": "email",
    "password_column": "password_hash",
    "user_id_column": "id",
    "refresh_table": "refresh_tokens"
  }
}
```

- **`secret`** is required. Note: in the current version it's embedded
  in the generated `main.cpp` as plaintext. For production, edit the
  generated code to read from an environment variable, or wait for the
  upcoming `${ENV_VAR}` substitution feature.
- **`refresh.enabled: true`** makes the generator automatically:
  - emit `/auth/login`, `/auth/refresh`, `/auth/logout` endpoints
  - emit `migrations/100_pgcrypto.sql` + `migrations/101_refresh_tokens_*.sql`
  - use `pgcrypto.crypt()` for bcrypt password verification on login

### `files` (S3-compatible presigned uploads)

```json
"files": {
  "enabled": true,
  "region": "us-east-1",
  "bucket": "myapp-uploads",
  "access_key": "AKIA...",
  "secret_key": "secretEXAMPLE",
  "endpoint": "",                  // optional; "https://...:9000" for MinIO/R2
  "url_ttl_s": 900,
  "metadata_table": "files",
  "max_size_mb": 50
}
```

- **Requires `auth.type=jwt`** (the uploader is identified from the JWT).
- AWS S3, Cloudflare R2, MinIO, DO Spaces — all S3-compatible. Just
  change `endpoint`.
- The generator emits `POST /files/upload-url` (presigned PUT URL) and
  `POST /files/:id/confirm`. The mobile app uploads directly to S3.

### `push` (FCM/APNs queue + device registry)

```json
"push": {
  "enabled": true,
  "queue_table": "push_queue",
  "device_token_table": "device_tokens"
}
```

- **Requires `auth.type=jwt`**.
- The generator emits `POST /push/register-device` (register token +
  platform) and a DELETE counterpart.
- The actual FCM/APNs dispatch is **not** done by your server — you
  write a row to `push_queue` (typically from a hook), and a separate
  worker (Node/Python/Go) polls the table and sends the push. Schema
  is documented:

```sql
INSERT INTO push_queue (user_id, title, body, payload)
VALUES ('42', 'New message', 'Someone commented', '{}'::jsonb);
```

You can run that statement from a hook (see below).

---

## Cookbook - typical scenarios

### 1) Public read API (e-commerce catalog)

```json
{
  "resources": [{
    "path": "/products",
    "table": "products",
    "primary_key": "id",
    "columns": ["id", "name", "description", "price", "stock", "category"],
    "methods": ["GET"],
    "filters": {
      "category":  {"column": "category", "op": "eq"},
      "min_price": {"column": "price",    "op": "gte"},
      "search":    {"column": "name",     "op": "contains"}
    },
    "sort":       {"allowed": ["price", "name"], "default": "price"},
    "pagination": {"default": 20, "max": 100, "include_total": true},
    "cache_ttl_s": 60,
    "etag": true
  }]
}
```

What you get: `GET /products?category=clothing&min_price=50&sort=-price&page=2`
style queryable listing, 60s cache, ETag support. No auth.

### 2) Per-user notes app (mobile)

```json
{
  "auth": {"type": "jwt", "secret": "...", "refresh": {"enabled": true}},
  "resources": [{
    "path": "/notes",
    "table": "notes",
    "primary_key": "id",
    "columns": ["id", "user_id", "title", "body", "created_at"],
    "methods": ["GET", "POST", "PUT", "DELETE"],
    "ownership": {"column": "user_id"},
    "validations": {
      "title": {"type": "text", "required": true, "max_length": 200},
      "body":  {"type": "text", "max_length": 100000}
    },
    "pagination": {"default": 50, "max": 200}
  }]
}
```

What you get: each user sees / writes only their own notes.
`/auth/login` + `/auth/refresh` + `/auth/logout` are auto-emitted. The
JWT's `sub` claim must match `user_id`. Titles longer than 200 chars
return 400.

### 3) Admin panel + public API (role-based)

```json
{
  "auth": {"type": "jwt", "secret": "..."},
  "resources": [{
    "path": "/users",
    "table": "users",
    "primary_key": "id",
    "columns": ["id", "email", "name", "role"],
    "methods": ["GET", "POST", "PUT", "DELETE"],
    "ownership": {"column": "id"},
    "permissions": {
      "list":   {"roles": ["admin"]},
      "get":    {"roles": ["user", "admin"], "bypass_ownership": ["admin"]},
      "create": {"roles": ["admin"]},
      "update": {"roles": ["user", "admin"], "bypass_ownership": ["admin"]},
      "delete": {"roles": ["admin"]}
    }
  }]
}
```

Behavior:
- **Admin** can list all users and update any profile.
- **User** can only GET/PUT their own profile (no `bypass_ownership`).
- Listing + deleting users is admin-only.

### 4) IoT device telemetry (high write rate)

```json
{
  "resources": [{
    "path": "/telemetry",
    "table": "telemetry_events",
    "primary_key": "id",
    "columns": ["id", "device_id", "metric", "value", "ts"],
    "methods": ["POST", "GET"],
    "data_model": "event",
    "write_mode": "batch_memory",
    "batch_size": 2000,
    "flush_interval_ms": 50,
    "filters": {
      "device_id": {"column": "device_id", "op": "eq"},
      "since":     {"column": "ts",        "op": "gte"}
    },
    "pagination": {"default": 100, "max": 5000},
    "aggregations": {
      "stats": {"columns": ["value"], "ops": ["avg", "min", "max"]}
    }
  }]
}
```

Behavior: POSTs are batched (memory-only, fast), GET supports filtering
and a `/telemetry/stats` aggregation endpoint. Can sustain 10k+ events/s.

### 5) Automatic audit log via after_insert hook

```json
{
  "push": {"enabled": true},
  "resources": [{
    "path": "/orders",
    "table": "orders",
    "primary_key": "id",
    "columns": ["id", "user_id", "total", "status"],
    "methods": ["POST", "PUT"],
    "ownership": {"column": "user_id"},
    "hooks": {
      "after_insert": "INSERT INTO audit (table_name, action, row_id, user_id) VALUES ('orders', 'create', $1, $2)",
      "after_update": "INSERT INTO push_queue (user_id, title, body) VALUES ($2, 'Order updated', 'See your order status')"
    }
  }]
}
```

In hook SQL, `$1` = primary key of the new/updated row, `$2` = user_id
(from JWT).
- `after_insert`: every new order writes to the audit table
- `after_update`: pushes a notification to the user via push_queue

### 6) Idempotent POST (mobile 4G safety)

```json
{
  "resources": [{
    "path": "/payments",
    "table": "payments",
    "primary_key": "id",
    "columns": ["id", "user_id", "amount", "status"],
    "methods": ["POST"],
    "ownership": {"column": "user_id"},
    "idempotency": {"enabled": true},
    "validations": {
      "amount": {"type": "float", "required": true, "min": 1}
    }
  }]
}
```

Behavior: the mobile app sends `Idempotency-Key: <uuid>` with the POST.
If the same key arrives again, the server **replays the original
response** instead of creating a duplicate row. Ideal for flaky network
conditions.

---

## Resource fields

### Required fields

| Field | Type | Description |
|---|---|---|
| `path` | string | URL path, e.g. `/users`. The generator auto-creates `/path/:id`. |
| `table` | string | PG table name |
| `primary_key` | string | PK column name (default `"id"`) |
| `columns` | string[] | Table columns in order (INSERT/SELECT projection) |
| `methods` | string[] | Subset of `["GET", "POST", "PUT", "DELETE"]` |

### Filtering — `filters`

Maps query string parameters to WHERE clauses. The generator enforces a
**whitelist**: a filter param not declared in config returns 400.

```json
"filters": {
  "param_name": {"column": "column_name", "op": "operator"}
}
```

Supported ops:

| Op | SQL | Example query string |
|---|---|---|
| `eq` | `=` | `?status=paid` |
| `neq` | `<>` | `?status=cancelled` |
| `gt`, `gte`, `lt`, `lte` | `>`, `>=`, `<`, `<=` | `?min_age=18` |
| `like` | `LIKE` | `?prefix=foo%25` (% encoded) |
| `ilike` | `ILIKE` | case-insensitive LIKE |
| `contains` | `ILIKE '%v%'` | `?search=phone` |
| `starts_with` | `ILIKE 'v%'` | `?prefix=Apple` |
| `ends_with` | `ILIKE '%v'` | `?suffix=Pro` |
| `in` | `= ANY()` | `?status=paid,shipped` |
| `not_in` | `<> ALL()` | `?status=cancelled,refunded` |
| `is_null` | `IS NULL` | `?archived=1` (value ignored; presence triggers) |
| `not_null` | `IS NOT NULL` | `?confirmed=1` |

### Sorting — `sort`

```json
"sort": {
  "allowed": ["price", "name", "created_at"],
  "default": "-created_at"
}
```

- `allowed`: columns the client may request via `?sort=`. Anything else
  returns 400.
- `default`: the ORDER BY used when `?sort=` is absent. Prefix `-` for DESC.

Query examples:
- `?sort=name` → ORDER BY name ASC
- `?sort=-price` → ORDER BY price DESC

### Pagination — `pagination`

**Offset mode (default)**:
```json
"pagination": {"default": 20, "max": 100, "include_total": true}
```
- `?page=2&page_size=20` → `LIMIT 20 OFFSET 20`
- With `include_total: true` the server emits a `COUNT(*) OVER ()`
  window and wraps the response:
  ```json
  {"data": [...], "meta": {"total": 1234, "page": 2, "page_size": 20, "has_next": true}}
  ```

**Cursor mode** (large tables, infinite scroll):
```json
"pagination": {"mode": "cursor", "default": 20, "max": 100}
```
- `?cursor=<last_pk>&page_size=20` → `WHERE pk > cursor ORDER BY pk ASC LIMIT 20`
- The response's `meta.next_cursor` is the new cursor.
- Cursor mode is not compatible with `field_masking` (the pk must be
  emitted in every response).

### Relations — `relations`

Embeds related rows via PG-side `json_agg` in the same query:

```json
"relations": {
  "posts": {
    "table": "posts",
    "fk": "user_id",
    "columns": ["id", "title", "created_at"],
    "embed": "auto"
  }
}
```

Generated SQL:
```sql
SELECT users.id, users.name,
       coalesce(json_agg(json_build_object('id', posts.id, 'title', posts.title, ...))
                FILTER (WHERE posts.id IS NOT NULL), '[]'::json) AS posts
FROM users LEFT JOIN posts ON posts.user_id = users.id
GROUP BY users.id
```

Response:
```json
[{"id": 1, "name": "Alice", "posts": [{"id": 1, "title": "..."}, ...]}, ...]
```

`embed`:
- `"auto"`: always embedded
- `"on_demand"`: only embedded when `?include=posts` is present (not
  yet fully implemented — currently behaves the same as `auto`)

### Ownership — `ownership`

```json
"ownership": {"column": "user_id"}
```

- Every SELECT gets an automatic `WHERE user_id = <caller_user_id>`
- On POST the `user_id` in the body is **overridden** with the value
  from the JWT (clients cannot impersonate)
- On PUT/DELETE the request only succeeds if the row's user_id matches
- With `auth.type=jwt`, user_id is taken from the JWT claim
- With `auth.type=header`, it's taken from `ownership.header` (default
  `X-User-Id`)

### Roles and permissions — `permissions`

```json
"permissions": {
  "list":   {"roles": ["admin", "user"], "bypass_ownership": ["admin"]},
  "create": ["admin"],
  "update": ["user", "admin"],
  "delete": ["admin"]
}
```

Two forms:
- **Short form** `["role1", "role2"]` = `{"roles": [...], "bypass_ownership": []}`
- **Long form** with an explicit bypass list

`bypass_ownership`: users with one of these roles skip the
`WHERE owner_id = ...` filter (e.g. admins seeing all rows).

Op keys: `list`, `get`, `create`, `update`, `delete`, `bulk`, `count`,
`stats`. Omitting `permissions` for an op means no role gate (but
ownership still applies if configured).

### Field validation — `validations`

Validates the body before POST/PUT. Failures return 400 with
`{errors: [...]}`.

```json
"validations": {
  "email":    {"type": "email", "required": true},
  "username": {"type": "text",  "required": true, "min_length": 3, "max_length": 32},
  "age":      {"type": "int",   "min": 0, "max": 150},
  "role":     {"type": "text",  "enum": ["user", "admin", "moderator"]},
  "rating":   {"type": "int",   "required": true, "min": 1, "max": 5}
}
```

Types: `text`, `int`, `bigint`, `float`, `bool`, `email`, `uuid`.

- `required: true` is enforced only on POST; PUT treats it as partial update
- `min/max`: numeric types only
- `min_length/max_length`: string types only
- `enum`: string types only

### Soft delete — `soft_delete`

```json
"soft_delete": "deleted_at"
```

- Every SELECT gains an automatic `AND deleted_at IS NULL`
- DELETE becomes `UPDATE table SET deleted_at = now() WHERE ...`
- Ideal for mobile sync (combine with a `?since=...` filter + tombstone rows)

### Field selection — `field_masking`

```json
"field_masking": true
```

`?fields=id,name` narrows the SELECT projection. The whitelist is the
resource's `columns`. Not compatible with cursor pagination.

### Per-resource cache config — `cache_ttl_s` and `etag`

```json
"cache_ttl_s": 60,
"etag": true
```

- `cache_ttl_s`: TTL for `query_cached` (seconds)
- `etag: true`: the response body's FNV-1a hash is written into the
  `ETag` header; clients that send a matching `If-None-Match` get 304.

### Aggregations — `aggregations`

```json
"aggregations": {
  "count": true,
  "stats": {"columns": ["price", "rating"], "ops": ["min", "max", "avg", "sum", "count"]}
}
```

- `count: true` → emits `GET /<path>/count` (applies current filters)
- `stats: {...}` → emits `GET /<path>/stats` with response:
  ```json
  {"price": {"min": 5.0, "max": 999.0, "avg": 124.5, "sum": ..., "count": 1234}}
  ```

### Bulk writes — `bulk`

```json
"bulk": {"enabled": true, "max_size": 1000}
```

Emits `POST /<path>/bulk` which accepts an array body. All-or-nothing:
if any row fails validation, none are written.

### Idempotency — `idempotency`

```json
"idempotency": {"enabled": true, "header": "Idempotency-Key", "table": "idempotency_log"}
```

For POSTs. Clients send `Idempotency-Key: <uuid>`; if the same key
arrives again, the **stored response is replayed** (no new row). The
generator auto-emits the `idempotency_log` migration.

### Lifecycle hooks — `hooks`

```json
"hooks": {
  "before_insert": "SELECT check_user_limit($2)",
  "after_insert":  "INSERT INTO audit VALUES ('orders', 'create', $1, $2, now())",
  "before_update": null,
  "after_update":  "INSERT INTO push_queue (user_id, title, body) VALUES ($2, 'Updated', '...')",
  "before_delete": "SELECT can_delete_order($1, $2)",
  "after_delete":  null,
  "transactional": false
}
```

Hook SQL parameters:
- `$1` = primary key value (empty before INSERT)
- `$2` = user_id (from JWT or ownership header)

Behavior:
- **before_***: SQL failure aborts the request with 500.
- **after_***: SQL failure is logged but the response was already sent.
- **transactional: true** (POST only): wraps all hooks + the main op in
  a single BEGIN..COMMIT. Any failure → ROLLBACK.

### Write mode — `write_mode` and `data_model`

```json
"data_model": "state" | "event",
"write_mode": "sync" | "async_durable" | "batch_durable" | "async_memory" | "batch_memory" | "disabled",
"batch_write": true,
"batch_size": 500,
"flush_interval_ms": 10
```

- **`data_model: "state"`**: same-pk writes coalesce; the BatchWriter
  emits an UPSERT with a `WHERE version <=` guard.
- **`data_model: "event"`**: every row is preserved (plain multi-row INSERT).
- **`write_mode: "batch_memory"`** (fastest): rows are enqueued in the
  BatchWriter and flushed periodically. **Crash-loss possible.**
- **`write_mode: "batch_durable"`**: BatchWriter + WAL. Crash-safe, mild latency.
- **`write_mode: "sync"`** (default): each POST is one INSERT, immediate.

### Pressure controller — `priority` and `overload_behavior`

```json
"priority": "critical" | "high" | "normal" | "low" | "best_effort",
"overload_behavior": "return_503" | "return_429" | "return_202" | "drop_best_effort" | ...
```

When PG slows down or the pool fills:
- `critical`: gets the highest token-bucket refill rate; almost never rejected
- `best_effort`: the first to get rejected
- `overload_behavior`: what the client sees when rejected (503/429 vs. async ack)

---

## Validation rules

The generator runs several checks at config-load time; violations cause
an early exit (caught before C++ compilation):

| Rule | Error message |
|---|---|
| Missing `path` or `table` | `resource missing 'path' or 'table'` |
| Empty `columns` | `resource has empty 'columns'` |
| `filters.<x>.column` not in columns | `filter '<x>' references column ... not in resource columns` |
| `filters.<x>.op` unsupported | `op '<bad>' not in {eq, neq, gt, gte, ...}` |
| `sort.allowed` references missing columns | `sort.allowed contains '<x>' which is not in resource columns` |
| `relations.<r>` missing field (table/fk/columns) | `relation '<r>' missing 'table'/'fk'/'columns'` |
| `ownership.column` missing | `ownership.column required` |
| `validations.<x>.type` unsupported | `type 'potato' not in {text, int, bigint, float, ...}` |
| `validations.<x>.min` on a string type | `min/max requires a numeric type, got 'text'` |
| `cursor` mode + `field_masking` | `cursor is not compatible with field_masking` |
| `files.enabled` + non-JWT auth | `files.enabled requires auth.type='jwt'` |
| `push.enabled` + non-JWT auth | `push.enabled requires auth.type='jwt'` |
| `auth.refresh.enabled` + non-JWT auth | refresh only makes sense in JWT mode |
| `hooks.<event>` unknown | `unknown event, allowed: before_/after_ × insert/update/delete` |

---

## Minimum viable config

A single endpoint:

```json
{
  "service":  {"name": "tiny", "port": 8080},
  "database": {"socket_path": "/var/run/postgresql/.s.PGSQL.5432",
               "user": "tiny", "dbname": "tiny"},
  "resources": [{
    "path": "/items",
    "table": "items",
    "columns": ["id", "name"],
    "methods": ["GET", "POST"]
  }]
}
```

What works:
- `GET /items?page=1&page_size=20` (default pagination, no filters)
- `GET /items/:id`
- `POST /items` with body `{"name": "..."}` → INSERT
- `GET /_spido_pg/health` (built-in)
- `GET /_spido_pg/metrics` (built-in)

Five lines of config, ~400 lines of generated code. From here you can
add features section by section from the reference above.

---

## Building a config step by step

1. **Design the table**: run `CREATE TABLE` in PG yourself (one per
   resource). The generator does not create schemas — tables must
   exist before the service starts.
2. **Start minimal**: copy the example above, fill in path/table/columns,
   generate, run.
3. **Add filters**: which query params should be supported? Add each
   to the `filters` map.
4. **Add sort + pagination**: which columns can be sorted by?
   `sort.allowed` is the whitelist. Enable `pagination.include_total`
   for mobile "X of Y" screens.
5. **Need auth?** Switch to JWT mode. Set a `secret`.
6. **Per-user data?** Add `ownership`. The generator auto-applies the filter.
7. **Roles?** Add `permissions`. Use `bypass_ownership` for admin views.
8. **Validation**: required fields, regex constraints, enums.
9. **Hooks**: audit logs, push notifications, side-effect SQL.
10. **Performance**: tune `cache_ttl_s`, `etag`, `write_mode` (use
    `batch_memory` for very high write rates), `priority`.

Regenerate + rebuild + test after each step. The generator is
deterministic — the same config produces the same code.

---

## Built-in examples

The repo includes working configs under `examples/`:

| File | Scenario |
|---|---|
| `examples/ecommerce/config.json` | Full e-commerce: products + orders (idempotency) + reviews, JWT, refresh, aggregations |
| `examples/myservice.json` | First default config — users / firmware / events resources |

Starting from `examples/ecommerce/config.json` and trimming it to your
needs is the fastest path.

---

## Going deeper

- **Architecture**: [`docs/PHASES.md`](PHASES.md) — internal design of
  spido-pg (cache, batch writer, WAL, entity cache, pressure controller).
- **Faz 4 roadmap**: SCRAM auth, OpenAPI export, advanced transaction wrapping.
- **Benchmark methodology**: the "Performance" section of the README.

If your first config doesn't generate correctly the error message is
usually self-explanatory. If you're stuck, open an issue with the
config + the error.
