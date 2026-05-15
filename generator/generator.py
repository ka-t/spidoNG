#!/usr/bin/env python3
"""
spido-pg code generator.

Reads a config.json describing a service (database, cache, REST resources,
proxy routes) and emits a ready-to-compile C++ project that wires libspido
(HTTP server) and libspido_pg (Postgres layer) together.

Usage:
    python generator.py config.json --output ./output
    python generator.py config.json --output ./output --build
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List

try:
    from jinja2 import Environment, FileSystemLoader, StrictUndefined
except ImportError:
    sys.stderr.write("error: jinja2 not installed. Run: pip install jinja2\n")
    sys.exit(2)


HERE = Path(__file__).resolve().parent


VALID_DATA_MODELS  = {"state", "event"}
VALID_PRIORITIES   = {"critical", "high", "normal", "low", "best_effort"}
VALID_READ_MODES   = {
    "direct_db", "cache_first", "cache_only_when_pressure",
    "stale_cache_allowed", "ram_entity_first",
}
VALID_WRITE_MODES  = {
    "sync", "async_durable", "batch_durable",
    "async_memory", "batch_memory", "disabled",
}
VALID_CONSISTENCY  = {
    "strict", "read_your_writes", "eventual_durable",
    "eventual_memory", "best_effort",
}
VALID_OVERLOAD     = {
    "delay", "queue", "return_202", "return_429", "return_503",
    "stale_cache", "drop_best_effort", "batch_only", "wal_only",
}

# Filter operators. Each is rendered at generation time into a C++ block
# that emits the SQL fragment + pushes the right PgParam. `takes_param=False`
# means the clause embeds no $N (e.g. IS NULL). `wrap` controls how the
# request value is transformed before being shipped as a parameter.
VALID_FILTER_OPS = {
    "eq", "neq", "gt", "gte", "lt", "lte",
    "like", "ilike", "contains", "starts_with", "ends_with",
    "in", "not_in",
    "is_null", "not_null",
}

FILTER_OP_INFO = {
    "eq":          {"sql": "=",          "takes_param": True,  "wrap": None},
    "neq":         {"sql": "<>",         "takes_param": True,  "wrap": None},
    "gt":          {"sql": ">",          "takes_param": True,  "wrap": None},
    "gte":         {"sql": ">=",         "takes_param": True,  "wrap": None},
    "lt":          {"sql": "<",          "takes_param": True,  "wrap": None},
    "lte":         {"sql": "<=",         "takes_param": True,  "wrap": None},
    "like":        {"sql": "LIKE",       "takes_param": True,  "wrap": None},
    "ilike":       {"sql": "ILIKE",      "takes_param": True,  "wrap": None},
    "contains":    {"sql": "ILIKE",      "takes_param": True,  "wrap": "contains"},
    "starts_with": {"sql": "ILIKE",      "takes_param": True,  "wrap": "starts_with"},
    "ends_with":   {"sql": "ILIKE",      "takes_param": True,  "wrap": "ends_with"},
    "in":          {"sql": "= ANY",      "takes_param": True,  "wrap": "in_array"},
    "not_in":      {"sql": "<> ALL",     "takes_param": True,  "wrap": "in_array"},
    "is_null":     {"sql": "IS NULL",    "takes_param": False, "wrap": None},
    "not_null":    {"sql": "IS NOT NULL","takes_param": False, "wrap": None},
}

# Named SQL blocks we recognize in an optional .sql override file.
SQL_BLOCK_NAMES = {"list", "get", "create", "update", "delete"}

# Field-validation types. Each maps to a parse-and-range-check block in the
# emitted validate_<table>() function.
VALID_FIELD_TYPES = {
    "text", "int", "bigint", "float", "bool", "email", "uuid",
}


def _validate_choice(name: str, value: str, choices: set, where: str) -> None:
    if value not in choices:
        raise SystemExit(
            f"config error: {where}: {name}={value!r} not in {sorted(choices)}")


def _qcol(table: str, col: str) -> str:
    """Qualify a column with its table name. Avoids ambiguity in JOINs."""
    return f"{table}.{col}"


def compile_resource_sql(r: Dict[str, Any]) -> None:
    """Precompute the static SQL fragments the template will splice in.

    Adds keys onto the resource dict in-place:
      select_list      — comma-separated qualified columns + relation aggs
      from_clause      — FROM table [LEFT JOIN ... for each relation]
      group_by_clause  — GROUP BY <pk> when relations are embedded, else empty
      filter_list      — list of compiled filter specs for the template
      sort_options     — list of {param, sql_col} the runtime accepts
      sort_default_sql — e.g. "users.created_at DESC"; empty if none
      insert_columns   — columns to INSERT (excludes auto-embedded ownership
                         column when ownership.set_on_insert is True)
      insert_pk_placeholder — N for ownership column's $N or None
    """
    table = r["table"]
    cols  = r["columns"]
    pk    = r["primary_key"]

    relations = r.get("relations", {})
    auto_relations = {n: d for n, d in relations.items() if d["embed"] == "auto"}
    on_demand_relations = {n: d for n, d in relations.items() if d["embed"] == "on_demand"}

    # ---- SELECT projection -------------------------------------------------
    base_cols = [_qcol(table, c) for c in cols]
    r["select_list_base"] = ", ".join(base_cols)

    rel_aggs = []
    for rname, rdef in auto_relations.items():
        rtable = rdef["table"]
        # json_agg(json_build_object('col', t.col, ...)) FILTER (WHERE t.pk IS NOT NULL)
        kvs = ", ".join(f"'{c}', {rtable}.{c}" for c in rdef["columns"])
        # Filter on relation table's first column being non-null — proxy for
        # "this side of the LEFT JOIN matched". The first listed column is
        # almost always the PK; if not, the user can override via sql_file.
        first_col = rdef["columns"][0]
        agg = (
            f"coalesce(json_agg(json_build_object({kvs})) "
            f"FILTER (WHERE {rtable}.{first_col} IS NOT NULL), '[]'::json) AS {rname}"
        )
        rel_aggs.append(agg)
    r["relation_aggs"] = ", ".join(rel_aggs)
    r["select_list"]   = ", ".join(base_cols + rel_aggs)

    # ---- FROM + JOINs ------------------------------------------------------
    from_sql = table
    for rname, rdef in auto_relations.items():
        rtable = rdef["table"]
        fk     = rdef["fk"]
        # rtable.fk = table.pk
        from_sql += f" LEFT JOIN {rtable} ON {rtable}.{fk} = {table}.{pk}"
    r["from_clause"] = from_sql

    # Optional-include relations: emitted as alternate SELECT/FROM fragments
    # the template can splice in when ?include=<name> is present.
    r["on_demand_relations"] = []
    for rname, rdef in on_demand_relations.items():
        rtable = rdef["table"]
        kvs = ", ".join(f"'{c}', {rtable}.{c}" for c in rdef["columns"])
        first_col = rdef["columns"][0]
        r["on_demand_relations"].append({
            "name": rname,
            "select_fragment": (
                f"coalesce(json_agg(json_build_object({kvs})) "
                f"FILTER (WHERE {rtable}.{first_col} IS NOT NULL), '[]'::json) AS {rname}"
            ),
            "join_fragment": (
                f" LEFT JOIN {rtable} ON {rtable}.{rdef['fk']} = {table}.{pk}"
            ),
        })

    # ---- GROUP BY ----------------------------------------------------------
    # When any aggregate is in the SELECT, PG needs a GROUP BY for non-agg
    # columns. Grouping by the primary key alone works because PG infers
    # functional dependence (column NOT NULL + PRIMARY KEY).
    if auto_relations or on_demand_relations:
        r["group_by_clause"] = f"GROUP BY {_qcol(table, pk)}"
    else:
        r["group_by_clause"] = ""

    # ---- Filter list (template iterates) ----------------------------------
    flist = []
    for fname, fdef in r["filters"].items():
        info = FILTER_OP_INFO[fdef["op"]]
        flist.append({
            "param":        fname,
            "column":       _qcol(table, fdef["column"]),
            "op":           fdef["op"],
            "sql_op":       info["sql"],
            "takes_param":  info["takes_param"],
            "wrap":         info["wrap"],
        })
    r["filter_list"] = flist

    # ---- Sort options ------------------------------------------------------
    sort = r["sort"]
    sopts = []
    for c in sort["allowed"]:
        bare = c[1:] if c.startswith("-") else c
        sopts.append({"param": bare, "sql_col": _qcol(table, bare)})
    r["sort_options"] = sopts
    if sort["default"]:
        bare = sort["default"][1:] if sort["default"].startswith("-") else sort["default"]
        direction = "DESC" if sort["default"].startswith("-") else "ASC"
        r["sort_default_sql"] = f"{_qcol(table, bare)} {direction}"
    else:
        r["sort_default_sql"] = ""

    # ---- Ownership fragment ------------------------------------------------
    own = r.get("ownership")
    if own:
        r["ownership_sql_col"] = _qcol(table, own["column"])
        # Index of the ownership column in cols_<table>. POST overrides this
        # cell with the header value so clients can't create rows for other
        # users no matter what they put in the body.
        r["ownership_col_index"] = cols.index(own["column"])
    else:
        r["ownership_sql_col"] = ""
        r["ownership_col_index"] = -1

    # ---- Soft-delete fragment ---------------------------------------------
    if r.get("soft_delete"):
        r["soft_delete_sql_col"] = _qcol(table, r["soft_delete"])
    else:
        r["soft_delete_sql_col"] = ""

    # ---- Hooks (None means no hook) --------------------------------------
    hooks = r.get("hooks", {})
    r["hook_transactional"] = bool(hooks.get("transactional", False))
    for hk in ("before_insert", "after_insert",
               "before_update", "after_update",
               "before_delete", "after_delete"):
        r[f"hook_{hk}"] = hooks.get(hk)

    # ---- Permission specs — None means no check ---------------------------
    # Each perms_<op> is either None (no check) or {roles, bypass_ownership}.
    perms = r.get("permissions", {})
    r["perms_header"] = perms.get("header", "X-User-Roles")
    for _op in ("list", "get", "create", "update", "delete",
                "bulk", "count", "stats"):
        r[f"perms_{_op}"] = perms.get(_op)

    # ---- Aggregation SQL fragments ---------------------------------------
    # The count/stats endpoints don't need the relation joins (they
    # operate on the main table only), so they bypass `from_clause` and
    # query the bare table.
    agg = r.get("aggregations", {})
    r["agg_count_enabled"] = bool(agg.get("count"))
    stats = agg.get("stats")
    if stats:
        parts = []
        for col in stats["columns"]:
            for op in stats["ops"]:
                parts.append(f"{op.upper()}({table}.{col}) AS {col}_{op}")
        r["agg_stats_enabled"]    = True
        r["agg_stats_sql_select"] = ", ".join(parts)
        r["agg_stats_columns"]    = stats["columns"]
        r["agg_stats_ops"]        = stats["ops"]
    else:
        r["agg_stats_enabled"]    = False
        r["agg_stats_sql_select"] = ""
        r["agg_stats_columns"]    = []
        r["agg_stats_ops"]        = []

    # ---- Validation rules — template iterates -----------------------------
    # Each entry is a dict with all keys the template branches on. Missing
    # keys are represented as None so Jinja's StrictUndefined doesn't fail.
    vrules = []
    for col, vdef in r.get("validations", {}).items():
        vrules.append({
            "column":     col,
            "type":       vdef.get("type", "text"),
            "required":   bool(vdef.get("required", False)),
            "min":        vdef.get("min"),
            "max":        vdef.get("max"),
            "min_length": vdef.get("min_length"),
            "max_length": vdef.get("max_length"),
            "enum":       vdef.get("enum"),
        })
    r["validation_rules"] = vrules
    r["has_validations"]  = bool(vrules)


def parse_sql_blocks(text: str) -> Dict[str, str]:
    """Split an override .sql file on `-- @name` markers.

    Recognised block names are in SQL_BLOCK_NAMES. Unknown blocks raise.
    Blocks may use `{{WHERE}}`, `{{ORDER}}`, `{{LIMIT}}`, `{{OWNERSHIP}}`,
    `{{SOFT_DELETE}}`, `{{COLUMNS}}`, `{{VALUES}}`, `{{SET}}` as placeholders
    that the runtime fills in.
    """
    blocks: Dict[str, str] = {}
    current = None
    buf: List[str] = []
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("-- @"):
            if current is not None:
                blocks[current] = "\n".join(buf).strip()
                buf = []
            name = stripped[4:].strip()
            if name not in SQL_BLOCK_NAMES:
                raise SystemExit(
                    f"sql override: unknown block '@{name}', "
                    f"expected one of {sorted(SQL_BLOCK_NAMES)}")
            current = name
        elif current is not None:
            buf.append(line)
    if current is not None:
        blocks[current] = "\n".join(buf).strip()
    return blocks


def load_config(path: Path) -> Dict[str, Any]:
    with path.open() as f:
        cfg = json.load(f)
    # Normalize / defaults.
    cfg.setdefault("service", {})
    cfg["service"].setdefault("name", "service")
    cfg["service"].setdefault("port", 8080)

    db = cfg.setdefault("database", {})
    db.setdefault("socket_path", "/var/run/postgresql/.s.PGSQL.5432")
    db.setdefault("user", "postgres")
    db.setdefault("password", "")
    db.setdefault("dbname", "postgres")
    db.setdefault("min_conns", 0)
    db.setdefault("max_conns", 0)

    cache = cfg.setdefault("cache", {})
    cache.setdefault("enabled", True)
    cache.setdefault("max_bytes", 256 * 1024 * 1024)
    cache.setdefault("default_ttl_s", 5)

    # ---- Top-level auth config (JWT) ----------------------------------
    # type="header" (default, X-User-Id/X-User-Roles headers from reverse
    # proxy) OR type="jwt" (Authorization: Bearer <token>, verified in-
    # process). When type=jwt, ownership/permissions automatically pull
    # from JWT claims instead of header values.
    auth = cfg.setdefault("auth", {})
    auth.setdefault("type", "header")
    if auth["type"] not in {"header", "jwt"}:
        raise SystemExit(
            f"config error: auth.type must be 'header' or 'jwt', got {auth['type']!r}")
    if auth["type"] == "jwt":
        auth.setdefault("algo", "HS256")
        if auth["algo"] not in {"HS256"}:
            raise SystemExit(
                f"config error: auth.algo {auth['algo']!r} not supported "
                f"(currently HS256 only)")
        if not auth.get("secret"):
            raise SystemExit("config error: auth.secret required when type=jwt")
        auth.setdefault("user_claim",  "sub")
        auth.setdefault("roles_claim", "roles")
        auth.setdefault("leeway_s",    60)
        auth.setdefault("issuer",      "")
        auth.setdefault("audience",    "")
        # Refresh token + login/logout endpoints — opt-in. Requires the
        # caller's users table to store passwords as pgcrypto bcrypt
        # hashes (the emitted migration installs the extension).
        refresh = auth.setdefault("refresh", {})
        refresh.setdefault("enabled", False)
        if not isinstance(refresh["enabled"], bool):
            raise SystemExit("config error: auth.refresh.enabled must be bool")
        refresh.setdefault("users_table",     "users")
        refresh.setdefault("email_column",    "email")
        refresh.setdefault("password_column", "password_hash")
        refresh.setdefault("user_id_column",  "id")
        refresh.setdefault("access_ttl_s",    900)
        refresh.setdefault("refresh_ttl_s",   30 * 24 * 3600)
        refresh.setdefault("refresh_table",   "refresh_tokens")
    else:
        # Placeholder defaults so templates can reference these unconditionally
        # without StrictUndefined errors. The branches that read them are
        # gated on auth.type == 'jwt' anyway.
        auth.setdefault("algo", "")
        auth.setdefault("secret", "")
        auth.setdefault("user_claim",  "")
        auth.setdefault("roles_claim", "")
        auth.setdefault("leeway_s",    60)
        auth.setdefault("issuer",      "")
        auth.setdefault("audience",    "")
        auth.setdefault("refresh", {"enabled": False})

    cfg.setdefault("resources", [])
    cfg.setdefault("proxy", [])

    # ---- Files (S3 presigned uploads) ----------------------------------
    # Off by default. When enabled, generator emits:
    #   POST /files/upload-url  — issues a presigned PUT URL + metadata row
    #   POST /files/{id}/confirm — marks upload as completed
    # plus migrations for the files metadata table.
    files = cfg.setdefault("files", {})
    files.setdefault("enabled", False)
    if not isinstance(files["enabled"], bool):
        raise SystemExit("config error: files.enabled must be bool")
    if files["enabled"]:
        for k in ("region", "bucket", "access_key", "secret_key"):
            if not files.get(k):
                raise SystemExit(f"config error: files.{k} required when files.enabled")
        files.setdefault("endpoint", "")       # blank → s3.<region>.amazonaws.com (virtual-hosted)
        files.setdefault("url_ttl_s", 900)
        files.setdefault("metadata_table", "files")
        files.setdefault("max_size_mb", 50)
        # Requires the JWT auth to be configured (so we know who's uploading)
        if cfg.get("auth", {}).get("type") != "jwt":
            raise SystemExit(
                "config error: files.enabled requires auth.type='jwt' so the "
                "upload endpoint can identify the uploader from the bearer token")

    # ---- Push notifications (queue + device registry) -----------------
    # The server only enqueues — an external worker polls push_queue and
    # dispatches to FCM/APNs. Keeps the server free of push-library deps.
    # Hooks can enqueue rows directly: INSERT INTO push_queue ... in any
    # after_insert / after_update hook SQL.
    push = cfg.setdefault("push", {})
    push.setdefault("enabled", False)
    if not isinstance(push["enabled"], bool):
        raise SystemExit("config error: push.enabled must be bool")
    if push["enabled"]:
        push.setdefault("queue_table",        "push_queue")
        push.setdefault("device_token_table", "device_tokens")
        if cfg.get("auth", {}).get("type") != "jwt":
            raise SystemExit(
                "config error: push.enabled requires auth.type='jwt' so the "
                "/push/register-device endpoint can attach the token to a user")

    # Assign sequential numeric endpoint ids starting at 1. Endpoint id 0
    # is reserved by the runtime for "unknown". We also assign a table_id
    # per unique table name so hot-path lookups can be O(1) integer
    # indexing in Faz 4.
    table_ids: Dict[str, int] = {}
    next_table_id = 1
    next_endpoint_id = 1

    for r in cfg["resources"]:
        r.setdefault("methods", ["GET"])
        r.setdefault("primary_key", "id")
        r.setdefault("cache_ttl_s", cache["default_ttl_s"])
        r.setdefault("stale_while_revalidate_s", 0)
        r.setdefault("auth", False)
        r.setdefault("batch_write", False)
        r.setdefault("batch_size", 500)
        r.setdefault("flush_interval_ms", 10)
        r.setdefault("columns", [])

        # New Faz 1 policy fields with safe defaults that match the C++
        # EndpointPolicy struct defaults.
        r.setdefault("data_model", "state")
        r.setdefault("priority",   "normal")
        r.setdefault("read_mode",  "cache_first")
        r.setdefault("write_mode", "sync")
        r.setdefault("consistency_level", "strict")
        r.setdefault("overload_behavior", "return_503")
        r.setdefault("hotset_size", 0)
        r.setdefault("preload_query", "")
        r.setdefault("allow_coalescing", False)
        r.setdefault("allow_stale",      False)
        r.setdefault("allow_drop",       False)
        r.setdefault("allow_memory_only", False)
        r.setdefault("require_wal",      False)
        r.setdefault("require_pg_confirm", False)
        r.setdefault("pin_hotset",       False)
        r.setdefault("batch_size_min",   100)
        r.setdefault("batch_size_max",   5000)
        r.setdefault("flush_interval_min_ms", 5)
        r.setdefault("flush_interval_max_ms", 2000)
        r.setdefault("max_queue_depth",       100_000)

        # Hard requirements — the template can't emit a usable handler
        # without a path, table, and column list. Fail loud with a path
        # the user can fix, not at the C++ compile stage where the error
        # comes from a generated file they're not supposed to read.
        if "path" not in r or "table" not in r:
            raise SystemExit(
                f"config error: resource missing 'path' or 'table': {r!r}")
        if not r["columns"]:
            raise SystemExit(
                f"config error: resource '{r['table']}' has empty 'columns'. "
                f"Body parsing and SELECT projection both need this list.")
        if r["primary_key"] not in r["columns"]:
            sys.stderr.write(
                f"warning: primary_key '{r['primary_key']}' not in columns "
                f"for {r['table']}; SELECT by id will still issue the SQL.\n")

        _validate_choice("data_model",        r["data_model"],        VALID_DATA_MODELS,  r["table"])
        _validate_choice("priority",          r["priority"],          VALID_PRIORITIES,   r["table"])
        _validate_choice("read_mode",         r["read_mode"],         VALID_READ_MODES,   r["table"])
        _validate_choice("write_mode",        r["write_mode"],        VALID_WRITE_MODES,  r["table"])
        _validate_choice("consistency_level", r["consistency_level"], VALID_CONSISTENCY,  r["table"])
        _validate_choice("overload_behavior", r["overload_behavior"], VALID_OVERLOAD,     r["table"])

        # ---- New: filter / sort / pagination / relations / ownership ----
        # All optional; missing → no clauses emitted, defaults to the bare
        # "SELECT cols FROM table" we shipped before.
        r.setdefault("filters", {})
        for fname, fdef in r["filters"].items():
            if not isinstance(fdef, dict):
                raise SystemExit(
                    f"config error: {r['table']}: filter '{fname}' must be an object "
                    f"with 'column' and 'op' fields")
            fdef.setdefault("column", fname)
            if "op" not in fdef:
                raise SystemExit(
                    f"config error: {r['table']}: filter '{fname}' missing 'op'")
            if fdef["op"] not in VALID_FILTER_OPS:
                raise SystemExit(
                    f"config error: {r['table']}: filter '{fname}': op={fdef['op']!r} "
                    f"not in {sorted(VALID_FILTER_OPS)}")
            if fdef["column"] not in r["columns"]:
                raise SystemExit(
                    f"config error: {r['table']}: filter '{fname}' references column "
                    f"{fdef['column']!r} which is not in resource columns")

        sort = r.setdefault("sort", {})
        sort.setdefault("allowed", [])
        sort.setdefault("default", "")
        for c in sort["allowed"]:
            # Strip optional leading '-' so users can list it however they want.
            bare = c[1:] if c.startswith("-") else c
            if bare not in r["columns"]:
                raise SystemExit(
                    f"config error: {r['table']}: sort.allowed contains {c!r} which "
                    f"is not in resource columns")
        if sort["default"]:
            bare = sort["default"][1:] if sort["default"].startswith("-") else sort["default"]
            if bare not in r["columns"]:
                raise SystemExit(
                    f"config error: {r['table']}: sort.default={sort['default']!r} "
                    f"references a column not in resource columns")

        pag = r.setdefault("pagination", {})
        pag.setdefault("default", 20)
        pag.setdefault("max", 100)
        pag.setdefault("include_total", False)
        pag.setdefault("mode", "offset")
        if pag["max"] < pag["default"] or pag["default"] <= 0 or pag["max"] <= 0:
            raise SystemExit(
                f"config error: {r['table']}: pagination invalid: {pag!r}")
        if not isinstance(pag["include_total"], bool):
            raise SystemExit(
                f"config error: {r['table']}: pagination.include_total must be bool")
        if pag["mode"] not in {"offset", "cursor"}:
            raise SystemExit(
                f"config error: {r['table']}: pagination.mode must be 'offset' or 'cursor'")
        if pag["mode"] == "cursor" and r.get("field_masking"):
            raise SystemExit(
                f"config error: {r['table']}: pagination.mode='cursor' is not "
                f"compatible with field_masking (the cursor needs the primary "
                f"key in every response — disable one or the other)")

        # Relations: one entry per embedded/joinable child resource. The
        # foreign key is on the *child* table pointing back at this row's
        # primary key.
        rels = r.setdefault("relations", {})
        for rname, rdef in rels.items():
            if not isinstance(rdef, dict):
                raise SystemExit(
                    f"config error: {r['table']}: relation '{rname}' must be an object")
            for required in ("table", "fk", "columns"):
                if required not in rdef:
                    raise SystemExit(
                        f"config error: {r['table']}: relation '{rname}' missing "
                        f"'{required}'")
            if not rdef["columns"]:
                raise SystemExit(
                    f"config error: {r['table']}: relation '{rname}' has empty columns")
            rdef.setdefault("embed", "auto")
            if rdef["embed"] not in {"auto", "on_demand"}:
                raise SystemExit(
                    f"config error: {r['table']}: relation '{rname}' embed must be "
                    f"'auto' or 'on_demand'")

        r.setdefault("ownership", None)
        r.setdefault("soft_delete", None)
        r.setdefault("sql_file", None)
        r.setdefault("field_masking", False)
        if not isinstance(r["field_masking"], bool):
            raise SystemExit(
                f"config error: {r['table']}: field_masking must be a bool")

        r.setdefault("etag", False)
        if not isinstance(r["etag"], bool):
            raise SystemExit(
                f"config error: {r['table']}: etag must be a bool")

        # ---- Lifecycle hooks ----------------------------------------------
        # Each hook is raw SQL. The runtime substitutes $1 = pk_value (text;
        # empty for before_insert), $2 = user_id from ownership header
        # (empty if not configured). before_* runs before the main op;
        # failure aborts the request. after_* runs after, errors are
        # logged but the response already committed.
        # Only fires for direct (non-batched) handlers; batched writes
        # don't have a single deterministic exec point.
        hooks = r.setdefault("hooks", {})
        allowed_hooks = {
            "before_insert", "after_insert",
            "before_update", "after_update",
            "before_delete", "after_delete",
        }
        hooks.setdefault("transactional", False)
        if not isinstance(hooks["transactional"], bool):
            raise SystemExit(
                f"config error: {r['table']}.hooks.transactional must be bool")
        for k, v in hooks.items():
            if k == "transactional":
                continue
            if k not in allowed_hooks:
                raise SystemExit(
                    f"config error: {r['table']}.hooks.{k}: unknown event, "
                    f"allowed: {sorted(allowed_hooks)} or 'transactional'")
            if v is not None and not isinstance(v, str):
                raise SystemExit(
                    f"config error: {r['table']}.hooks.{k} must be SQL "
                    f"string or null")

        # ---- Roles / permissions ------------------------------------------
        # Per-operation allow-list. The handler reads a comma-separated
        # roles header and 403s the request if no entry matches.
        # Schema:
        #   permissions:
        #     header: "X-User-Roles"        # default
        #     list:   ["admin", "viewer"]
        #     create: ["admin", "user"]
        #     ...
        # Missing op key → no role check (existing behavior, ownership
        # filter still applies if configured).
        perms = r.setdefault("permissions", {})
        perms.setdefault("header", "X-User-Roles")
        allowed_perm_ops = {
            "list", "get", "create", "update", "delete",
            "bulk", "count", "stats",
        }
        for k, v in perms.items():
            if k == "header":
                if not isinstance(v, str) or not v:
                    raise SystemExit(
                        f"config error: {r['table']}.permissions.header "
                        f"must be a non-empty string")
                continue
            if k not in allowed_perm_ops:
                raise SystemExit(
                    f"config error: {r['table']}.permissions.{k}: unknown op, "
                    f"allowed: {sorted(allowed_perm_ops)}")
            if isinstance(v, list):
                if not all(isinstance(x, str) for x in v):
                    raise SystemExit(
                        f"config error: {r['table']}.permissions.{k} "
                        f"list must contain role strings")
                # Normalize to dict form internally.
                perms[k] = {"roles": v, "bypass_ownership": []}
            elif isinstance(v, dict):
                v.setdefault("roles", [])
                v.setdefault("bypass_ownership", [])
                for sub in ("roles", "bypass_ownership"):
                    if not isinstance(v[sub], list) or \
                       not all(isinstance(x, str) for x in v[sub]):
                        raise SystemExit(
                            f"config error: {r['table']}.permissions.{k}.{sub} "
                            f"must be a list of role strings")
            else:
                raise SystemExit(
                    f"config error: {r['table']}.permissions.{k} "
                    f"must be a list of role strings or {{roles, bypass_ownership}} object")

        # ---- Idempotency keys ----------------------------------------------
        # When enabled, POST handlers honour an Idempotency-Key header:
        # pre-claim the key in a PG table; if it was already claimed,
        # replay the stored status+body. Schema the user must create:
        #   CREATE TABLE idempotency_log (
        #     key text PRIMARY KEY, endpoint text NOT NULL,
        #     status int NOT NULL DEFAULT 0, body text NOT NULL DEFAULT '',
        #     created_at timestamptz NOT NULL DEFAULT now());
        idem = r.setdefault("idempotency", {})
        idem.setdefault("enabled", False)
        idem.setdefault("table",  "idempotency_log")
        idem.setdefault("header", "Idempotency-Key")
        if not isinstance(idem["enabled"], bool):
            raise SystemExit(
                f"config error: {r['table']}.idempotency.enabled must be bool")

        # ---- Bulk insert endpoint ----------------------------------------
        bulk = r.setdefault("bulk", {})
        bulk.setdefault("enabled", False)
        bulk.setdefault("max_size", 1000)
        if not isinstance(bulk["enabled"], bool):
            raise SystemExit(
                f"config error: {r['table']}.bulk.enabled must be bool")
        if not isinstance(bulk["max_size"], int) or bulk["max_size"] <= 0:
            raise SystemExit(
                f"config error: {r['table']}.bulk.max_size must be a positive int")

        # ---- Aggregation endpoints (count, stats) ----------------------
        agg = r.setdefault("aggregations", {})
        agg.setdefault("count", False)
        if not isinstance(agg["count"], bool):
            raise SystemExit(
                f"config error: {r['table']}.aggregations.count must be bool")
        agg.setdefault("stats", None)
        if agg["stats"] is not None:
            stats = agg["stats"]
            if not isinstance(stats, dict):
                raise SystemExit(
                    f"config error: {r['table']}.aggregations.stats must be object")
            scols = stats.get("columns")
            if not isinstance(scols, list) or not scols:
                raise SystemExit(
                    f"config error: {r['table']}.aggregations.stats.columns "
                    f"must be a non-empty list")
            for c in scols:
                if c not in r["columns"]:
                    raise SystemExit(
                        f"config error: {r['table']}.aggregations.stats: "
                        f"column {c!r} not in resource columns")
            stats.setdefault("ops", ["count", "min", "max", "avg", "sum"])
            allowed_ops = {"count", "min", "max", "avg", "sum"}
            for op in stats["ops"]:
                if op not in allowed_ops:
                    raise SystemExit(
                        f"config error: {r['table']}.aggregations.stats.ops: "
                        f"unknown op {op!r}, allowed: {sorted(allowed_ops)}")
        own = r["ownership"]
        if own is not None:
            if "column" not in own:
                raise SystemExit(
                    f"config error: {r['table']}: ownership.column required")
            if own["column"] not in r["columns"]:
                raise SystemExit(
                    f"config error: {r['table']}: ownership.column "
                    f"{own['column']!r} not in resource columns")
            own.setdefault("header", "X-User-Id")

        sd = r["soft_delete"]
        if sd is not None:
            if not isinstance(sd, str):
                raise SystemExit(
                    f"config error: {r['table']}: soft_delete must be a column name string")
            if sd not in r["columns"]:
                raise SystemExit(
                    f"config error: {r['table']}: soft_delete column "
                    f"{sd!r} not in resource columns")

        # ---- Field validation (per-column rules) ------------------------
        # Optional; missing → no validation. Rules:
        #   type:        text|int|bigint|float|bool|email|uuid (default: text)
        #   required:    bool (default: false); only enforced on POST (create)
        #   min/max:     numeric range (int/float/bigint)
        #   min_length/max_length: string length
        #   enum:        list of allowed strings
        vals = r.setdefault("validations", {})
        for vcol, vdef in vals.items():
            if not isinstance(vdef, dict):
                raise SystemExit(
                    f"config error: {r['table']}: validation '{vcol}' must be object")
            if vcol not in r["columns"]:
                raise SystemExit(
                    f"config error: {r['table']}: validation '{vcol}' is not "
                    f"a column in this resource")
            vdef.setdefault("type", "text")
            vdef.setdefault("required", False)
            if vdef["type"] not in VALID_FIELD_TYPES:
                raise SystemExit(
                    f"config error: {r['table']}.validations.{vcol}: type "
                    f"{vdef['type']!r} not in {sorted(VALID_FIELD_TYPES)}")
            # Type-coherence checks.
            numeric_types = {"int", "bigint", "float"}
            string_types  = {"text", "email", "uuid"}
            if "min" in vdef or "max" in vdef:
                if vdef["type"] not in numeric_types:
                    raise SystemExit(
                        f"config error: {r['table']}.validations.{vcol}: "
                        f"min/max requires a numeric type, got {vdef['type']!r}")
            if "min_length" in vdef or "max_length" in vdef:
                if vdef["type"] not in string_types:
                    raise SystemExit(
                        f"config error: {r['table']}.validations.{vcol}: "
                        f"min_length/max_length requires a string type, "
                        f"got {vdef['type']!r}")
            if "enum" in vdef:
                if vdef["type"] not in string_types:
                    raise SystemExit(
                        f"config error: {r['table']}.validations.{vcol}: "
                        f"enum requires a string type")
                if not isinstance(vdef["enum"], list) or not vdef["enum"]:
                    raise SystemExit(
                        f"config error: {r['table']}.validations.{vcol}: "
                        f"enum must be a non-empty list")

        sql_file = r["sql_file"]
        r["sql_blocks"] = {}
        if sql_file:
            p = Path(sql_file)
            if not p.is_absolute():
                # Resolve relative to the config file's directory.
                p = path.parent / p
            if not p.exists():
                raise SystemExit(
                    f"config error: {r['table']}: sql_file {sql_file!r} not found")
            r["sql_blocks"] = parse_sql_blocks(p.read_text())

        # JWT-mode auto-auth: when global auth.type=jwt and the resource
        # touches user-scoped data (ownership) or has role gates, mark
        # routes as require_auth=true so spido's verifier runs before
        # our handler. Explicit `auth: true` still works the same way.
        _perm_ops = {"list","get","create","update","delete","bulk","count","stats"}
        _has_perms = any(r.get("permissions", {}).get(op) for op in _perm_ops)
        r["needs_auth"] = bool(r["auth"]) or (
            cfg["auth"]["type"] == "jwt" and
            (bool(r.get("ownership")) or _has_perms)
        )

        # Numeric ids.
        r["endpoint_id"] = next_endpoint_id
        next_endpoint_id += 1
        if r["table"] not in table_ids:
            table_ids[r["table"]] = next_table_id
            next_table_id += 1
        r["table_id"] = table_ids[r["table"]]

        # Pre-compute SQL fragments the template will splice into the
        # generated handler. Keeping this in Python keeps the Jinja file
        # focused on layout, not SQL string-juggling.
        compile_resource_sql(r)

    for p in cfg["proxy"]:
        p.setdefault("auth", False)
        p.setdefault("timeout_ms", 1000)

    return cfg


def render(env: Environment, name: str, ctx: Dict[str, Any]) -> str:
    return env.get_template(name).render(**ctx)


def write(target: Path, content: str) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(content)


def _openapi_type_for(column: str, validations: Dict[str, Any]) -> Dict[str, Any]:
    """Map a column's validation type to an OpenAPI schema fragment."""
    vdef = validations.get(column, {})
    t = vdef.get("type", "text")
    mapping = {
        "text":   {"type": "string"},
        "int":    {"type": "integer", "format": "int32"},
        "bigint": {"type": "integer", "format": "int64"},
        "float":  {"type": "number"},
        "bool":   {"type": "boolean"},
        "email":  {"type": "string", "format": "email"},
        "uuid":   {"type": "string", "format": "uuid"},
    }
    schema = dict(mapping.get(t, {"type": "string"}))
    if "min" in vdef:           schema["minimum"]   = vdef["min"]
    if "max" in vdef:           schema["maximum"]   = vdef["max"]
    if "min_length" in vdef:    schema["minLength"] = vdef["min_length"]
    if "max_length" in vdef:    schema["maxLength"] = vdef["max_length"]
    if "enum" in vdef:          schema["enum"]      = vdef["enum"]
    return schema


def _resource_schema(r: Dict[str, Any]) -> Dict[str, Any]:
    """Build the OpenAPI schema for a single resource row."""
    props: Dict[str, Any] = {}
    required: List[str] = []
    for col in r["columns"]:
        props[col] = _openapi_type_for(col, r.get("validations", {}))
        vdef = r.get("validations", {}).get(col, {})
        if vdef.get("required"):
            required.append(col)
    # Relations get embedded as arrays in list/get responses
    for rel_name, rel in r.get("relations", {}).items():
        if rel.get("embed") == "auto":
            rel_props = {c: {"type": "string"} for c in rel["columns"]}
            props[rel_name] = {
                "type": "array",
                "items": {"type": "object", "properties": rel_props},
            }
    schema: Dict[str, Any] = {"type": "object", "properties": props}
    if required:
        schema["required"] = required
    return schema


def _filter_param(fname: str, fdef: Dict[str, Any]) -> Dict[str, Any]:
    """OpenAPI parameter for a configured filter."""
    op = fdef["op"]
    desc = f"Filter by {fdef.get('column', fname)} ({op})"
    schema: Dict[str, Any] = {"type": "string"}
    if op in ("gt", "gte", "lt", "lte"):
        schema = {"type": "number"}
    elif op in ("is_null", "not_null"):
        desc += " — present-key sentinel; value is ignored"
    elif op in ("in", "not_in"):
        desc += " — comma-separated values"
    return {
        "name": fname, "in": "query", "required": False,
        "description": desc, "schema": schema,
    }


def _common_responses(has_perms: bool, has_ownership: bool) -> Dict[str, Any]:
    out = {
        "400": {"description": "validation error / invalid input"},
        "500": {"description": "database / server error"},
    }
    if has_ownership:
        out["401"] = {"description": "auth required (missing ownership header)"}
    if has_perms:
        out["403"] = {"description": "forbidden (role does not match)"}
    return out


def build_openapi(cfg: Dict[str, Any]) -> Dict[str, Any]:
    """Compose an OpenAPI 3.0 document from the resolved config.

    Every configured resource expands to up to 8 routes (list, by-id,
    create, update, delete, bulk, count, stats); each is documented with
    its parameters, request body schema and response shape inferred from
    the resource's columns + validations + ownership/permissions config.
    """
    paths: Dict[str, Any] = {}
    schemas: Dict[str, Any] = {}

    for r in cfg["resources"]:
        table   = r["table"]
        path    = r["path"]
        pk      = r["primary_key"]
        methods = r["methods"]
        schema_name = "".join(p.capitalize() for p in table.split("_"))
        schemas[schema_name] = _resource_schema(r)
        body_ref = {"$ref": f"#/components/schemas/{schema_name}"}

        ownership_hdr = r["ownership"]["header"] if r.get("ownership") else None
        perms_hdr     = r.get("perms_header", "X-User-Roles")
        has_perms     = any(r.get(f"perms_{op}") for op in
                            ("list","get","create","update","delete",
                             "bulk","count","stats"))

        def add_headers(perm_op: str) -> List[Dict[str, Any]]:
            hdrs: List[Dict[str, Any]] = []
            if ownership_hdr:
                hdrs.append({
                    "name": ownership_hdr, "in": "header", "required": True,
                    "schema": {"type": "string"},
                    "description": "Caller user id (used for row-level ownership filter)",
                })
            if r.get(f"perms_{perm_op}"):
                hdrs.append({
                    "name": perms_hdr, "in": "header", "required": True,
                    "schema": {"type": "string"},
                    "description": f"Comma-separated roles; required one of: "
                                   f"{r[f'perms_{perm_op}']}",
                })
            return hdrs

        # ---- GET list ----
        if "GET" in methods:
            params = add_headers("list")
            for fname, fdef in r.get("filters", {}).items():
                params.append(_filter_param(fname, fdef))
            if r.get("sort_options"):
                params.append({
                    "name": "sort", "in": "query", "required": False,
                    "schema": {"type": "string",
                               "enum": [s["param"] for s in r["sort_options"]] +
                                       [f"-{s['param']}" for s in r["sort_options"]]},
                    "description": "Sort field. Prefix with '-' for descending.",
                })
            params.append({
                "name": "page", "in": "query", "required": False,
                "schema": {"type": "integer", "minimum": 1, "default": 1},
            })
            params.append({
                "name": "page_size", "in": "query", "required": False,
                "schema": {"type": "integer", "minimum": 1,
                           "maximum": r["pagination"]["max"],
                           "default": r["pagination"]["default"]},
            })
            if r.get("field_masking"):
                params.append({
                    "name": "fields", "in": "query", "required": False,
                    "schema": {"type": "string"},
                    "description": "Comma-separated subset of columns to return. "
                                   f"Allowed: {r['columns']}",
                })
            if r["pagination"].get("include_total"):
                ok_schema = {
                    "type": "object",
                    "properties": {
                        "data": {"type": "array", "items": body_ref},
                        "meta": {
                            "type": "object",
                            "properties": {
                                "total":     {"type": "integer"},
                                "page":      {"type": "integer"},
                                "page_size": {"type": "integer"},
                                "has_next":  {"type": "boolean"},
                            },
                        },
                    },
                }
            else:
                ok_schema = {"type": "array", "items": body_ref}
            paths.setdefault(path, {})["get"] = {
                "summary": f"List {table}",
                "parameters": params,
                "responses": {
                    "200": {"description": "ok",
                            "content": {"application/json": {"schema": ok_schema}}},
                    **_common_responses(has_perms, bool(ownership_hdr)),
                },
            }
            # GET /:id
            id_path = f"{path}/{{id}}"
            paths.setdefault(id_path, {})["get"] = {
                "summary": f"Get one {table} by {pk}",
                "parameters": [
                    {"name": "id", "in": "path", "required": True,
                     "schema": {"type": "string"}},
                    *add_headers("get"),
                    *([{
                        "name": "fields", "in": "query", "required": False,
                        "schema": {"type": "string"},
                    }] if r.get("field_masking") else []),
                ],
                "responses": {
                    "200": {"description": "ok",
                            "content": {"application/json": {"schema": body_ref}}},
                    "404": {"description": "not found"},
                    **_common_responses(has_perms, bool(ownership_hdr)),
                },
            }

        # ---- POST ----
        if "POST" in methods:
            params = add_headers("create")
            if r["idempotency"].get("enabled"):
                params.append({
                    "name": r["idempotency"]["header"], "in": "header",
                    "required": False, "schema": {"type": "string"},
                    "description": "Idempotency key — replays a stored response if "
                                   "the same key has already succeeded.",
                })
            paths.setdefault(path, {})["post"] = {
                "summary": f"Create a {table}",
                "parameters": params,
                "requestBody": {
                    "required": True,
                    "content": {"application/json": {"schema": body_ref}},
                },
                "responses": {
                    "201": {"description": "created",
                            "content": {"application/json": {"schema": body_ref}}},
                    "202": {"description": "queued (batched/durable write modes)"},
                    "409": {"description": "duplicate idempotency key"},
                    **_common_responses(has_perms, bool(ownership_hdr)),
                },
            }
            if r.get("bulk", {}).get("enabled"):
                paths.setdefault(f"{path}/bulk", {})["post"] = {
                    "summary": f"Bulk-create {table} (max {r['bulk']['max_size']} rows)",
                    "parameters": add_headers("bulk"),
                    "requestBody": {
                        "required": True,
                        "content": {"application/json": {
                            "schema": {"type": "array", "items": body_ref},
                        }},
                    },
                    "responses": {
                        "201": {"description": "all inserted"},
                        "202": {"description": "queued (batched write modes)"},
                        "413": {"description": "too many items"},
                        **_common_responses(has_perms, bool(ownership_hdr)),
                    },
                }

        if "PUT" in methods:
            paths.setdefault(f"{path}/{{id}}", {})["put"] = {
                "summary": f"Update one {table} (partial)",
                "parameters": [
                    {"name": "id", "in": "path", "required": True,
                     "schema": {"type": "string"}},
                    *add_headers("update"),
                ],
                "requestBody": {
                    "required": True,
                    "content": {"application/json": {"schema": body_ref}},
                },
                "responses": {
                    "200": {"description": "ok"},
                    "404": {"description": "not found"},
                    **_common_responses(has_perms, bool(ownership_hdr)),
                },
            }
        if "DELETE" in methods:
            paths.setdefault(f"{path}/{{id}}", {})["delete"] = {
                "summary": f"Delete one {table}" +
                           (f" (soft: SET {r['soft_delete']})" if r.get("soft_delete") else ""),
                "parameters": [
                    {"name": "id", "in": "path", "required": True,
                     "schema": {"type": "string"}},
                    *add_headers("delete"),
                ],
                "responses": {
                    "204": {"description": "deleted"},
                    "404": {"description": "not found"},
                    **_common_responses(has_perms, bool(ownership_hdr)),
                },
            }

        # ---- Aggregations ----
        if r.get("agg_count_enabled"):
            paths.setdefault(f"{path}/count", {})["get"] = {
                "summary": f"Count {table}",
                "parameters": add_headers("count"),
                "responses": {
                    "200": {"description": "ok",
                            "content": {"application/json": {"schema": {
                                "type": "object",
                                "properties": {"count": {"type": "integer"}},
                            }}}},
                    **_common_responses(has_perms, bool(ownership_hdr)),
                },
            }
        if r.get("agg_stats_enabled"):
            stats_props = {
                col: {"type": "object",
                      "properties": {op: {"type": "number"} for op in r["agg_stats_ops"]}}
                for col in r["agg_stats_columns"]
            }
            paths.setdefault(f"{path}/stats", {})["get"] = {
                "summary": f"Stats over {table}",
                "parameters": add_headers("stats"),
                "responses": {
                    "200": {"description": "ok",
                            "content": {"application/json": {"schema": {
                                "type": "object", "properties": stats_props,
                            }}}},
                    **_common_responses(has_perms, bool(ownership_hdr)),
                },
            }

    return {
        "openapi": "3.0.3",
        "info": {
            "title":   cfg["service"]["name"],
            "version": "1.0.0",
            "description": "Generated by spido-pg from config.json",
        },
        "servers": [{"url": f"http://localhost:{cfg['service']['port']}"}],
        "paths": paths,
        "components": {"schemas": schemas},
    }


def build_migrations(cfg: Dict[str, Any]) -> Dict[str, str]:
    """Return {filename: sql_text} for every schema bit the generated
    service expects to exist in PG. Caller writes them to migrations/.
    Generator does NOT run them — that's the operator's responsibility.
    """
    out: Dict[str, str] = {}

    # Idempotency log — one CREATE per distinct table name across
    # resources (most setups share a single log, but a multi-tenant
    # service may want per-namespace tables).
    idem_tables: set = set()
    for r in cfg["resources"]:
        if r.get("idempotency", {}).get("enabled"):
            idem_tables.add(r["idempotency"]["table"])
    for i, t in enumerate(sorted(idem_tables)):
        sql = (
            f"-- Idempotency log for POST handlers using header "
            f"{', '.join(sorted({r['idempotency']['header'] for r in cfg['resources'] if r.get('idempotency', {}).get('enabled')}))}.\n"
            f"-- Pre-claimed before each insert; populated with status+body\n"
            f"-- after success so duplicate requests can replay the response.\n"
            f"CREATE TABLE IF NOT EXISTS {t} (\n"
            f"    key        text PRIMARY KEY,\n"
            f"    endpoint   text        NOT NULL,\n"
            f"    status     int         NOT NULL DEFAULT 0,\n"
            f"    body       text        NOT NULL DEFAULT '',\n"
            f"    created_at timestamptz NOT NULL DEFAULT now()\n"
            f");\n"
            f"CREATE INDEX IF NOT EXISTS {t}_created_at_idx ON {t} (created_at);\n"
        )
        out[f"00{i}_idempotency_{t}.sql"] = sql

    # Refresh tokens — when auth.refresh.enabled. pgcrypto is required for
    # the crypt()-based password verification in /auth/login.
    if cfg.get("auth", {}).get("refresh", {}).get("enabled"):
        rt_table = cfg["auth"]["refresh"]["refresh_table"]
        out["100_pgcrypto.sql"] = (
            "-- pgcrypto provides crypt() + gen_salt('bf') used by /auth/login\n"
            "-- to verify and (during user creation) generate bcrypt password\n"
            "-- hashes server-side. Operators store users with:\n"
            "--   INSERT INTO users (email, password_hash) VALUES\n"
            "--     ($1, crypt($2, gen_salt('bf', 12)))\n"
            "CREATE EXTENSION IF NOT EXISTS pgcrypto;\n"
        )
        out[f"101_refresh_tokens_{rt_table}.sql"] = (
            f"-- Refresh token registry. /auth/refresh DELETE-RETURNINGs the\n"
            f"-- old row and INSERTs a fresh one — single-use rotation so a\n"
            f"-- leaked token is worthless after one /auth/refresh call.\n"
            f"CREATE TABLE IF NOT EXISTS {rt_table} (\n"
            f"    token      text PRIMARY KEY,\n"
            f"    user_id    text        NOT NULL,\n"
            f"    expires_at timestamptz NOT NULL,\n"
            f"    created_at timestamptz NOT NULL DEFAULT now()\n"
            f");\n"
            f"CREATE INDEX IF NOT EXISTS {rt_table}_user_id_idx "
            f"ON {rt_table} (user_id);\n"
            f"CREATE INDEX IF NOT EXISTS {rt_table}_expires_at_idx "
            f"ON {rt_table} (expires_at);\n"
        )

    # Push: device registry + queue. The queue is consumed by an
    # external worker — server only writes rows.
    if cfg.get("push", {}).get("enabled"):
        dt = cfg["push"]["device_token_table"]
        qt = cfg["push"]["queue_table"]
        out[f"300_push_devices_{dt}.sql"] = (
            f"-- Device tokens. /push/register-device upserts on app launch\n"
            f"-- (and whenever the platform rotates the token).\n"
            f"CREATE TABLE IF NOT EXISTS {dt} (\n"
            f"    token         text PRIMARY KEY,\n"
            f"    user_id       text NOT NULL,\n"
            f"    platform      text NOT NULL CHECK (platform IN ('ios','android')),\n"
            f"    locale        text,\n"
            f"    created_at    timestamptz NOT NULL DEFAULT now(),\n"
            f"    last_seen_at  timestamptz NOT NULL DEFAULT now()\n"
            f");\n"
            f"CREATE INDEX IF NOT EXISTS {dt}_user_id_idx ON {dt} (user_id);\n"
        )
        out[f"301_push_queue_{qt}.sql"] = (
            f"-- Push queue. Hooks (or any backend code) INSERT here; an\n"
            f"-- external worker (Python / Go / Node) SELECTs pending rows,\n"
            f"-- fans out to FCM/APNs using the device_tokens table, and\n"
            f"-- UPDATEs the row to status='sent' or 'failed'.\n"
            f"--\n"
            f"-- Example after_insert hook:\n"
            f"--   INSERT INTO {qt} (user_id, title, body, payload)\n"
            f"--   VALUES ($2, 'New comment', 'Someone replied', '{{}}')\n"
            f"CREATE TABLE IF NOT EXISTS {qt} (\n"
            f"    id          bigserial PRIMARY KEY,\n"
            f"    user_id     text        NOT NULL,\n"
            f"    title       text        NOT NULL,\n"
            f"    body        text        NOT NULL,\n"
            f"    payload     jsonb,\n"
            f"    status      text        NOT NULL DEFAULT 'pending'\n"
            f"                CHECK (status IN ('pending','sent','failed')),\n"
            f"    attempts    int         NOT NULL DEFAULT 0,\n"
            f"    last_error  text,\n"
            f"    created_at  timestamptz NOT NULL DEFAULT now(),\n"
            f"    sent_at     timestamptz\n"
            f");\n"
            f"CREATE INDEX IF NOT EXISTS {qt}_pending_idx ON {qt} (created_at) "
            f"WHERE status = 'pending';\n"
            f"CREATE INDEX IF NOT EXISTS {qt}_user_id_idx ON {qt} (user_id);\n"
        )

    # Files metadata table — when files.enabled.
    if cfg.get("files", {}).get("enabled"):
        ft = cfg["files"]["metadata_table"]
        out[f"200_files_{ft}.sql"] = (
            f"-- File upload metadata. Rows are inserted by /files/upload-url\n"
            f"-- with status='pending' and an S3 presigned PUT URL is handed\n"
            f"-- to the client. After the client PUTs the bytes, /files/:id/\n"
            f"-- confirm flips status='ready'. Stale 'pending' rows can be\n"
            f"-- garbage-collected after the URL TTL has lapsed.\n"
            f"CREATE TABLE IF NOT EXISTS {ft} (\n"
            f"    id           text PRIMARY KEY,\n"
            f"    owner_id     text NOT NULL,\n"
            f"    filename     text NOT NULL,\n"
            f"    content_type text NOT NULL DEFAULT 'application/octet-stream',\n"
            f"    s3_key       text NOT NULL,\n"
            f"    status       text NOT NULL DEFAULT 'pending',\n"
            f"    created_at   timestamptz NOT NULL DEFAULT now(),\n"
            f"    confirmed_at timestamptz\n"
            f");\n"
            f"CREATE INDEX IF NOT EXISTS {ft}_owner_id_idx ON {ft} (owner_id);\n"
            f"CREATE INDEX IF NOT EXISTS {ft}_status_idx ON {ft} (status) "
            f"WHERE status = 'pending';\n"
        )

    return out


def write_lua_config(out_dir: Path, cfg: Dict[str, Any]) -> None:
    """Minimal spido.lua so operators can tweak server params without rebuild."""
    lines = [
        "-- generated by spido-pg generator. do not edit by hand.",
        f'server.port    = {cfg["service"]["port"]}',
        "server.threads = 0  -- 0 = one worker per cpu",
        "",
        "-- routes are wired in C++; lua only sets transport-level knobs",
    ]
    (out_dir / "spido.lua").write_text("\n".join(lines) + "\n")


def main(argv: List[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="spido-pg code generator")
    ap.add_argument("config", type=Path)
    ap.add_argument("--output", "-o", type=Path, required=True)
    ap.add_argument("--build", action="store_true",
                    help="run cmake + make in the generated directory")
    ap.add_argument("--spido-dir", type=Path, default=None,
                    help="path to libspido install/build (passed as -DSPIDO_DIR)")
    ap.add_argument("--spido-pg-dir", type=Path, default=None,
                    help="path to spido-pg build (passed as -DSPIDO_PG_DIR)")
    args = ap.parse_args(argv)

    if not args.config.exists():
        sys.stderr.write(f"error: config not found: {args.config}\n")
        return 1

    cfg = load_config(args.config)
    svc = cfg["service"]["name"]
    out_dir = args.output / svc
    if out_dir.exists():
        shutil.rmtree(out_dir)

    env = Environment(
        loader=FileSystemLoader(HERE / "templates"),
        undefined=StrictUndefined,
        trim_blocks=True,
        lstrip_blocks=True,
        keep_trailing_newline=True,
    )
    ctx = {
        "cfg": cfg,
        "service": cfg["service"],
        "database": cfg["database"],
        "cache":    cfg["cache"],
        "auth":     cfg["auth"],
        "resources": cfg["resources"],
        "proxies":  cfg["proxy"],
        "spido_dir":    str(args.spido_dir) if args.spido_dir else "",
        "spido_pg_dir": str(args.spido_pg_dir) if args.spido_pg_dir else "",
    }

    write(out_dir / "main.cpp",            render(env, "main.cpp.j2",            ctx))
    write(out_dir / "service_helpers.h",   render(env, "service_helpers.h.j2",   ctx))
    write(out_dir / "service_setup.h",     render(env, "service_setup.h.j2",     ctx))
    write(out_dir / "service_setup.cpp",   render(env, "service_setup.cpp.j2",   ctx))
    write(out_dir / "CMakeLists.txt",      render(env, "CMakeLists.txt.j2",      ctx))
    if cfg["auth"].get("refresh", {}).get("enabled"):
        write(out_dir / "auth_endpoints.h",
              render(env, "auth_endpoints.h.j2", ctx))
        write(out_dir / "auth_endpoints.cpp",
              render(env, "auth_endpoints.cpp.j2", ctx))
    if cfg.get("files", {}).get("enabled"):
        write(out_dir / "files_endpoints.h",
              render(env, "files_endpoints.h.j2", ctx))
        write(out_dir / "files_endpoints.cpp",
              render(env, "files_endpoints.cpp.j2", ctx))
    if cfg.get("push", {}).get("enabled"):
        write(out_dir / "push_endpoints.h",
              render(env, "push_endpoints.h.j2", ctx))
        write(out_dir / "push_endpoints.cpp",
              render(env, "push_endpoints.cpp.j2", ctx))
    write(out_dir / "openapi.json",        json.dumps(build_openapi(cfg), indent=2))
    for mname, sql in build_migrations(cfg).items():
        write(out_dir / "migrations" / mname, sql)
    for r in cfg["resources"]:
        rctx = {**ctx, "r": r}
        write(out_dir / "handlers" / f"{r['table']}.h",
              render(env, "handler.h.j2", rctx))
        write(out_dir / "handlers" / f"{r['table']}.cpp",
              render(env, "handler.cpp.j2", rctx))
    write_lua_config(out_dir, cfg)

    print(f"generated {out_dir}/")
    print(f"  main.cpp")
    print(f"  service_helpers.h")
    print(f"  service_setup.{{h,cpp}}")
    for r in cfg["resources"]:
        print(f"  handlers/{r['table']}.{{h,cpp}}")
    print(f"  CMakeLists.txt")
    print(f"  openapi.json")
    print(f"  spido.lua")
    migs = build_migrations(cfg)
    for m in sorted(migs):
        print(f"  migrations/{m}")

    if args.build:
        build_dir = out_dir / "build"
        build_dir.mkdir(exist_ok=True)
        cmd = ["cmake", ".."]
        if args.spido_dir:    cmd.append(f"-DSPIDO_DIR={args.spido_dir}")
        if args.spido_pg_dir: cmd.append(f"-DSPIDO_PG_DIR={args.spido_pg_dir}")
        r = subprocess.run(cmd, cwd=build_dir)
        if r.returncode != 0: return r.returncode
        r = subprocess.run(["make", f"-j{os.cpu_count() or 1}"], cwd=build_dir)
        if r.returncode != 0: return r.returncode
        print(f"built {build_dir}/{svc}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
