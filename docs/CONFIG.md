# Config Reference

SpidoNG'de tek bir `config.json` dosyası servisinin **tamamını** tanımlar:
hangi tabloya hangi yoldan erişilir, kim ne yapabilir, hangi alanlar
doğrulanır, cache nasıl davranır, hangi endpoint'lere push tetikleyici
bağlanır. Bu döküman tüm alanları örnekleriyle açıklar.

Hızlı geçit:
- [Top-level şema](#top-level-şema) — `service`, `database`, `cache`, `auth`, ...
- [Cookbook](#cookbook---tipik-senaryolar) — "şunu yapmak istiyorum" → "şunu yaz"
- [Resource alanları](#resource-alanları) — `resources[]` içindeki her şey
- [Validasyon kuralları](#validasyon-kuralları)
- [Minimum config](#minimum-viable-config) — en küçük çalışan örnek

---

## Top-level şema

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

| Alan | Tip | Default | Açıklama |
|---|---|---|---|
| `name` | string | `"service"` | C++ project ismi, binary adı, openapi `info.title` |
| `port` | int | `8080` | HTTP listen port |

### `database`

| Alan | Tip | Default | Açıklama |
|---|---|---|---|
| `socket_path` | string | `/var/run/postgresql/.s.PGSQL.5432` | PG Unix socket path |
| `user` | string | `postgres` | PG kullanıcısı |
| `password` | string | `""` | PG şifresi (SCRAM/MD5 için) |
| `dbname` | string | `postgres` | Database adı |
| `min_conns` | int | `0` (= `nproc`) | Minimum pool boyutu |
| `max_conns` | int | `0` (= `nproc * 4`) | Maksimum pool boyutu |

### `cache`

| Alan | Tip | Default | Açıklama |
|---|---|---|---|
| `enabled` | bool | `true` | L1/L2 cache aktif mi |
| `max_bytes` | int | `268435456` (256 MB) | Toplam cache kapasitesi |
| `default_ttl_s` | int | `5` | Resource'ta `cache_ttl_s` override yoksa kullanılır |

### `auth`

**İki mod var.** Default `"header"` (geriye dönük uyumlu):

#### Header mode (varsayılan)
```json
"auth": {"type": "header"}
```
Reverse proxy (nginx, Cloudflare Access, vs.) `X-User-Id` ve `X-User-Roles`
header'larını set ediyor varsayar. Generator bu header'ları okur. Sunucu
JWT doğrulamaz.

#### JWT mode (mobil/SPA için önerilen)
```json
"auth": {
  "type": "jwt",
  "algo": "HS256",
  "secret": "your-256-bit-secret",
  "user_claim": "sub",
  "roles_claim": "roles",
  "leeway_s": 60,
  "issuer": "myapp.com",          // optional, JWT iss claim eşleşmesi
  "audience": "mobile",            // optional, aud claim eşleşmesi
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

- **`secret`** zorunlu. `${JWT_SECRET}` formatında env var değildir
  (henüz) — şu an plaintext. Production'da generated main.cpp'yi env
  okuyacak şekilde değiştir veya bir sonraki sürümü bekle.
- **`refresh.enabled: true`** olunca generator otomatik olarak:
  - `/auth/login`, `/auth/refresh`, `/auth/logout` endpoint'leri üretir
  - `migrations/100_pgcrypto.sql` + `migrations/101_refresh_tokens_*.sql` üretir
  - Login'de `pgcrypto.crypt()` ile bcrypt password verify

### `files` (S3-uyumlu presigned uploads)

```json
"files": {
  "enabled": true,
  "region": "us-east-1",
  "bucket": "myapp-uploads",
  "access_key": "AKIA...",
  "secret_key": "secretEXAMPLE",
  "endpoint": "",                  // optional, MinIO/R2 için "https://...:9000"
  "url_ttl_s": 900,
  "metadata_table": "files",
  "max_size_mb": 50
}
```

- **`auth.type=jwt` zorunlu** (upload eden kullanıcının kim olduğu JWT'den okunur).
- AWS S3, Cloudflare R2, MinIO, DO Spaces — hepsi S3-uyumlu, sadece
  `endpoint` farklı.
- Generator `POST /files/upload-url` (presigned PUT URL) + `POST /files/:id/confirm`
  endpoint'leri üretir. Mobil app S3'e doğrudan yükler.

### `push` (FCM/APNs queue + device registry)

```json
"push": {
  "enabled": true,
  "queue_table": "push_queue",
  "device_token_table": "device_tokens"
}
```

- **`auth.type=jwt` zorunlu**.
- Generator `POST /push/register-device` (token + platform kayıt) + DELETE'i üretir.
- Asıl FCM/APNs dispatch'i sen yapmıyorsun — `push_queue` tablosuna SQL
  satırı yazıyorsun (hook'larda), harici bir worker (Node/Python/Go)
  bu tabloyu poll edip dispatch ediyor. Şema dökümante:

```sql
INSERT INTO push_queue (user_id, title, body, payload)
VALUES ('42', 'Yeni mesaj', 'Birisi yorum yaptı', '{}'::jsonb);
```

Bu satırı hook'tan da basabilirsin (aşağıda detay).

---

## Cookbook - tipik senaryolar

### 1) Public read API (e-ticaret ürün kataloğu)

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

Ne yapar: `GET /products?category=clothing&min_price=50&sort=-price&page=2`
şeklinde queriable, 60s cache'li, ETag'li listeleme. Hiç auth yok.

### 2) Per-user notes app (mobil)

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

Ne yapar: Her kullanıcı sadece kendi notlarını görür/yazar.
`/auth/login` + `/auth/refresh` + `/auth/logout` otomatik. JWT'deki
`sub` claim'i `user_id` ile eşleşmek zorunda. Title 200 karakterden
fazlaysa 400 döner.

### 3) Admin paneli + public API (rol bazlı)

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

Davranış:
- **Admin**: tüm user'ları listeleyebilir, herkesin profilini güncelleyebilir.
- **User**: sadece kendi profilini GET/PUT yapabilir (`bypass_ownership` yok).
- Listeleme + kullanıcı silme sadece admin'e açık.

### 4) IoT cihaz telemetrisi (yüksek yazma)

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

Davranış: POST'lar batched (memory-only, hızlı), GET ile sorgu+istatistik
endpoint'leri (`/telemetry/stats`). Saniyede 10k+ event çekebilir.

### 5) Audit log otomatik tutma (after_insert hook)

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
      "after_update": "INSERT INTO push_queue (user_id, title, body) VALUES ($2, 'Sipariş güncellendi', 'Siparişiniz hakkında bilgi')"
    }
  }]
}
```

Hook'larda `$1` = pk değeri (yeni satırın id'si), `$2` = user_id (JWT'den).
- `after_insert`: her yeni sipariş audit tablosuna kayıt
- `after_update`: kullanıcıya push notification queue'sa basılır

### 6) Idempotent POST (mobil 4G güvenliği)

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

Davranış: Mobil app `Idempotency-Key: <uuid>` header'ı ile POST atar.
Aynı key tekrar gelirse server **aynı response'u replay** eder, yeni
satır oluşturmaz. Şebeke kesintilerinde idealdir.

---

## Resource alanları

### Zorunlu alanlar

| Alan | Tip | Açıklama |
|---|---|---|
| `path` | string | URL yolu, örn. `/users`. Generator `path/:id`'yi otomatik üretir. |
| `table` | string | PG tablo adı |
| `primary_key` | string | PK kolon adı (default `"id"`) |
| `columns` | string[] | Tablo kolonları sırayla (INSERT/SELECT projeksiyonu) |
| `methods` | string[] | `["GET", "POST", "PUT", "DELETE"]` arasından seçim |

### Filtreleme — `filters`

Query string'den gelen parametreleri WHERE clause'a çevirir. Generator
**whitelist** uygular: config'te tanımlı olmayan filter denenince 400.

```json
"filters": {
  "param_name": {"column": "kolon_adi", "op": "operator"}
}
```

Desteklenen op'lar:

| Op | SQL | Örnek query string |
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
| `is_null` | `IS NULL` | `?archived=1` (değer önemsiz, varlığı yeter) |
| `not_null` | `IS NOT NULL` | `?confirmed=1` |

### Sıralama — `sort`

```json
"sort": {
  "allowed": ["price", "name", "created_at"],
  "default": "-created_at"
}
```

- `allowed`: query'den `?sort=` ile gelebilecek kolonlar. Whitelist dışı = 400.
- `default`: `?sort=` yoksa kullanılan ORDER BY. `-` öneki DESC.

Query örnekleri:
- `?sort=name` → ORDER BY name ASC
- `?sort=-price` → ORDER BY price DESC

### Sayfalama — `pagination`

**Offset mode (default)**:
```json
"pagination": {"default": 20, "max": 100, "include_total": true}
```
- `?page=2&page_size=20` → `LIMIT 20 OFFSET 20`
- `include_total: true` → `COUNT(*) OVER ()` ile total + meta yazısı:
  ```json
  {"data": [...], "meta": {"total": 1234, "page": 2, "page_size": 20, "has_next": true}}
  ```

**Cursor mode** (büyük tablolar, sonsuz scroll için):
```json
"pagination": {"mode": "cursor", "default": 20, "max": 100}
```
- `?cursor=<last_pk>&page_size=20` → `WHERE pk > cursor ORDER BY pk ASC LIMIT 20`
- Response `meta.next_cursor` ile yeni cursor verir.
- Cursor mode `field_masking` ile uyuşmaz (pk her response'ta gerekli).

### İlişkiler — `relations`

PG-tarafı `json_agg` ile tek query'de embed eder:

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

Üretilen SQL:
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
- `"auto"`: her zaman embed
- `"on_demand"`: sadece `?include=posts` parametresi geldiğinde embed (henüz tamamlanmadı — şu an `auto` ile aynı davranır)

### Sahiplik — `ownership`

```json
"ownership": {"column": "user_id"}
```

- Her SELECT'e otomatik `WHERE user_id = <caller_user_id>` eklenir
- POST'ta body'deki `user_id` kullanıcıdan değil, **JWT'den** alınır (client değiştiremez)
- PUT/DELETE'te aynı satır sahibi mi diye kontrol
- `auth.type=jwt` ise user_id JWT claim'inden
- `auth.type=header` ise `ownership.header` (default `X-User-Id`) header'ından

### Roller ve izinler — `permissions`

```json
"permissions": {
  "list":   {"roles": ["admin", "user"], "bypass_ownership": ["admin"]},
  "create": ["admin"],
  "update": ["user", "admin"],
  "delete": ["admin"]
}
```

İki form var:
- **Kısa form** `["role1", "role2"]` = `{"roles": [...], "bypass_ownership": []}`
- **Uzun form** explicit bypass listesi ile

`bypass_ownership`: bu rollere sahip kullanıcılar `WHERE owner_id = ...`
filtresini atlar (admin tüm satırları görsün).

Op anahtarları: `list`, `get`, `create`, `update`, `delete`, `bulk`,
`count`, `stats`. Bir op için `permissions` tanımlanmamışsa o op için
rol gate yok (ama ownership hâlâ uygulanır).

### Field validation — `validations`

POST/PUT öncesi gövdeyi doğrular. Hata varsa 400 + `{errors: [...]}`.

```json
"validations": {
  "email":    {"type": "email", "required": true},
  "username": {"type": "text",  "required": true, "min_length": 3, "max_length": 32},
  "age":      {"type": "int",   "min": 0, "max": 150},
  "role":     {"type": "text",  "enum": ["user", "admin", "moderator"]},
  "rating":   {"type": "int",   "required": true, "min": 1, "max": 5}
}
```

Tipler: `text`, `int`, `bigint`, `float`, `bool`, `email`, `uuid`.

- `required: true` sadece POST'ta zorunlu, PUT'ta partial update.
- `min/max`: sadece sayısal tipler
- `min_length/max_length`: sadece string tipler
- `enum`: sadece string tipler

### Yumuşak silme — `soft_delete`

```json
"soft_delete": "deleted_at"
```

- Tüm SELECT'lere `AND deleted_at IS NULL` eklenir
- DELETE → `UPDATE table SET deleted_at = now() WHERE ...` olarak çalışır
- Mobil sync için (`?since=...` filter + tombstoned rows) ideal

### Alan seçimi — `field_masking`

```json
"field_masking": true
```

`?fields=id,name` → SELECT projeksiyonu daraltılır. Whitelist config'teki
columns. Cursor pagination ile birlikte kullanılmaz.

### Cache başına ayar — `cache_ttl_s` ve `etag`

```json
"cache_ttl_s": 60,
"etag": true
```

- `cache_ttl_s`: query_cached TTL süresi (saniye)
- `etag: true`: response gövdesinin FNV-1a hash'i `ETag` header'ına basılır;
  client `If-None-Match` ile gönderirse 304 döner.

### Aggregations — `aggregations`

```json
"aggregations": {
  "count": true,
  "stats": {"columns": ["price", "rating"], "ops": ["min", "max", "avg", "sum", "count"]}
}
```

- `count: true` → `GET /<path>/count` endpoint'i (mevcut filtreleri uygular)
- `stats: {...}` → `GET /<path>/stats` endpoint'i, response:
  ```json
  {"price": {"min": 5.0, "max": 999.0, "avg": 124.5, "sum": ..., "count": 1234}}
  ```

### Toplu yazma — `bulk`

```json
"bulk": {"enabled": true, "max_size": 1000}
```

`POST /<path>/bulk` endpoint'i tek istekte array body kabul eder. All-or-nothing:
herhangi bir satır validation'dan geçemezse hiçbiri yazılmaz.

### Idempotency — `idempotency`

```json
"idempotency": {"enabled": true, "header": "Idempotency-Key", "table": "idempotency_log"}
```

POST'larda. Client `Idempotency-Key: <uuid>` gönderir; aynı key tekrar
gelirse stored response replay (yeni satır oluşturulmaz). Generator
`idempotency_log` migration'ını otomatik üretir.

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

Hook SQL'inde parametreler:
- `$1` = primary key değeri (insert öncesi boş)
- `$2` = user_id (JWT'den veya ownership header'ından)

Davranış:
- **before_***: SQL fail olursa request iptal, 500 döner.
- **after_***: SQL fail olursa log + devam (response zaten gönderildi).
- **transactional: true** (sadece POST için): BEGIN..COMMIT içinde tüm
  hook + main op çalışır. Herhangi biri fail → ROLLBACK.

### Yazma modu — `write_mode` ve `data_model`

```json
"data_model": "state" | "event",
"write_mode": "sync" | "async_durable" | "batch_durable" | "async_memory" | "batch_memory" | "disabled",
"batch_write": true,
"batch_size": 500,
"flush_interval_ms": 10
```

- **`data_model: "state"`**: aynı pk'li yeni write coalesce edilir, UPSERT WHERE version eşliğinde.
- **`data_model: "event"`**: her satır korunur, plain multi-row INSERT.
- **`write_mode: "batch_memory"`** (en hızlı): BatchWriter'a kuyruğa atılır, periyodik flush. **Crash'te kayıp** mümkün.
- **`write_mode: "batch_durable"`**: BatchWriter + WAL. Crash-safe, hafif gecikme.
- **`write_mode: "sync"`** (default): Her POST tek INSERT, immediate.

### Pressure controller — `priority` ve `overload_behavior`

```json
"priority": "critical" | "high" | "normal" | "low" | "best_effort",
"overload_behavior": "return_503" | "return_429" | "return_202" | "drop_best_effort" | ...
```

PG yavaşlayınca veya pool dolunca:
- `critical`: token bucket'a en yüksek priority, neredeyse hiç reject yok
- `best_effort`: ilk reject edilen
- `overload_behavior`: reject olunca client ne görür (503/429 vs. async ack)

---

## Validasyon kuralları

Generator config'i load ederken bazı kontroller yapar; ihlal varsa 400 ile
çıkar (compile öncesi yakalanır):

| Kural | Hata mesajı |
|---|---|
| `path` veya `table` eksik | `resource missing 'path' or 'table'` |
| `columns` boş | `resource has empty 'columns'` |
| `filters.<x>.column` columns'ta yok | `filter '<x>' references column ... not in resource columns` |
| `filters.<x>.op` desteklenmeyen op | `op '<bad>' not in {eq, neq, gt, gte, ...}` |
| `sort.allowed` kolonları yok | `sort.allowed contains '<x>' which is not in resource columns` |
| `relations.<r>` eksik alan (table/fk/columns) | `relation '<r>' missing 'table'/'fk'/'columns'` |
| `ownership.column` yok | `ownership.column required` |
| `validations.<x>.type` desteklenmiyor | `type 'potato' not in {text, int, bigint, float, ...}` |
| `validations.<x>.min` text tipinde | `min/max requires a numeric type, got 'text'` |
| `cursor` mode + `field_masking` | `cursor is not compatible with field_masking` |
| `files.enabled` + `auth.type != jwt` | `files.enabled requires auth.type='jwt'` |
| `push.enabled` + `auth.type != jwt` | `push.enabled requires auth.type='jwt'` |
| `auth.refresh.enabled` + `auth.type != jwt` | refresh ancak JWT modda anlamlıdır |
| `hooks.<event>` bilinmiyor | `unknown event, allowed: before_/after_ × insert/update/delete` |

---

## Minimum viable config

Sadece bir endpoint:

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

Ne çalışır:
- `GET /items?page=1&page_size=20` (default pagination, hiç filter yok)
- `GET /items/:id`
- `POST /items` body `{"name": "..."}` → INSERT
- `GET /_spido_pg/health` (built-in)
- `GET /_spido_pg/metrics` (built-in)

5 satır config. Üretilen kod ~400 satır. Bu noktadan başlayıp adım adım
yukarıdaki bölümlerden özellik ekleyebilirsin.

---

## Adım adım config inşa etme

1. **Tablonu tasarla**: PG'de `CREATE TABLE` (resource başına). Generator
   schema yaratmıyor — tablolar zaten orada olmalı.
2. **Minimum config**: yukarıdaki örnekten kopyala, path/table/columns
   doldur, generate et, çalıştır.
3. **Filter ekle**: query'den hangi parametreler gelmeli? Her birini
   `filters` map'ine ekle.
4. **Sort + pagination**: kullanıcı hangi kolonlara göre sıralayabilir?
   `sort.allowed` whitelist'i. `pagination.include_total: true` mobil
   "X'ten Y" ekranı için.
5. **Auth gerekli mi?** JWT mode'a geç. `secret` belirle.
6. **Per-user data?** `ownership` ekle. Generator otomatik filtre uygular.
7. **Roller var mı?** `permissions` ekle. Admin için `bypass_ownership`.
8. **Validation**: required alanlar, regex'ler, enum'lar.
9. **Hooks**: audit log, push notification, side effect SQL.
10. **Performance**: cache_ttl_s, etag, write_mode (yüksek yazma için
    batch_memory), priority.

Her adımda regenerate + rebuild + test et. Generator deterministic — aynı
config aynı kodu üretir.

---

## Mevcut örnekler

Repo'da `examples/` altında çalışan config'ler var:

| Dosya | Senaryo |
|---|---|
| `examples/ecommerce/config.json` | Tam e-ticaret: products + orders (idempotency) + reviews, JWT, refresh, aggregations |
| `examples/myservice.json` | İlk default config — users / firmware / events resource'ları |

Yeni başlıyorsan **`examples/ecommerce/config.json`**'i baz alıp kendine
göre kırp en hızlı yol.

---

## Daha derine

- **Mimari**: [`docs/PHASES.md`](PHASES.md) — spido-pg'nin iç tasarımı (cache, batch, WAL, entity cache, pressure controller).
- **Faz 4 roadmap**: SCRAM auth, OpenAPI export, transaction wrapping advanced cases.
- **Benchmark detay**: README'deki "Performance" bölümü.

İlk config'in çalışmazsa: generator output'taki hata mesajı genelde
yeterli açıklayıcı. Yine de takılırsan issue aç, repro adımları + config + hata.
