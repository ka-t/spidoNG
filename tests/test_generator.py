"""Generator regression tests.

Run from the project root:
    pytest tests/test_generator.py -v

We exercise:
  1. Argparse + config validation rejects malformed input clearly.
  2. The Jinja templates emit syntactically valid C++ — every .cpp must
     compile via `g++ -fsyntax-only`. Catches Jinja typos, dangling
     braces, missing includes, etc.
  3. Method coverage: GET/POST/PUT/DELETE each emit their handler.
  4. New-feature coverage: filters, sort whitelist, pagination,
     relations json_agg, ownership injection, soft_delete switching.
  5. File layout: helpers and per-resource handlers go to the right files.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
GENERATOR = ROOT / "generator" / "generator.py"


def run_gen(config_path, out_dir, expect_fail=False):
    cmd = [sys.executable, str(GENERATOR), str(config_path), "--output", str(out_dir)]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if expect_fail:
        assert proc.returncode != 0, f"expected failure, stdout: {proc.stdout}"
    else:
        assert proc.returncode == 0, f"generator failed: {proc.stderr}"
    return proc


@pytest.fixture
def tmpdir_factory(tmp_path):
    def _make():
        d = tmp_path / "out"
        d.mkdir(exist_ok=True)
        return d
    return _make


# ----- config validation -----

def write_cfg(path, cfg):
    path.write_text(json.dumps(cfg))


def base_cfg():
    return {
        "service":  {"name": "svc", "port": 8080},
        "database": {"socket_path": "/tmp/x", "user": "u", "password": "p",
                     "dbname": "d", "min_conns": 1, "max_conns": 4},
        "cache":    {"enabled": True, "max_bytes": 1024, "default_ttl_s": 5},
        "resources": [],
        "proxy": [],
    }


def test_valid_config_generates_expected_files(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/items", "table": "items", "primary_key": "id",
        "columns": ["id", "name"],
        "methods": ["GET", "POST", "PUT", "DELETE"],
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    svc = out / "svc"
    # Top-level files (project skeleton)
    assert (svc / "main.cpp").exists()
    assert (svc / "CMakeLists.txt").exists()
    assert (svc / "spido.lua").exists()
    # Shared helpers and setup live next to main
    assert (svc / "service_helpers.h").exists()
    assert (svc / "service_setup.h").exists()
    assert (svc / "service_setup.cpp").exists()
    # Per-resource handler files
    assert (svc / "handlers" / "items.h").exists()
    assert (svc / "handlers" / "items.cpp").exists()


def test_missing_columns_errors(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/x", "table": "x", "primary_key": "id",
        # no columns!
        "methods": ["GET"],
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    proc = run_gen(cfg_path, out, expect_fail=True)
    assert "columns" in proc.stderr or "columns" in proc.stdout


def test_missing_table_errors(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({"path": "/y", "columns": ["id"], "methods": ["GET"]})
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    proc = run_gen(cfg_path, out, expect_fail=True)
    assert "table" in proc.stderr or "table" in proc.stdout


# ----- method coverage -----

def test_all_methods_emitted(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id", "name"],
        "methods": ["GET", "POST", "PUT", "DELETE"],
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    handler = (out / "svc" / "handlers" / "users.cpp").read_text()
    # Two GET routes (list + by-id), one POST, one PUT, one DELETE.
    assert handler.count('srv.get("/u"') == 1
    assert handler.count('srv.get("/u/:id"') == 1
    assert handler.count('srv.post("/u"') == 1
    assert handler.count('srv.put("/u/:id"') == 1
    assert handler.count('srv.del("/u/:id"') == 1
    # main.cpp calls the registration entry-point.
    main = (out / "svc" / "main.cpp").read_text()
    assert "register_users_routes(srv, db)" in main


def test_get_only_skips_writes(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/ro", "table": "ro", "primary_key": "id",
        "columns": ["id", "x"],
        "methods": ["GET"],
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    handler = (out / "svc" / "handlers" / "ro.cpp").read_text()
    assert "srv.post" not in handler
    assert "srv.put"  not in handler
    assert "srv.del"  not in handler


def test_batch_write_uses_db_write(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/events", "table": "events", "primary_key": "id",
        "columns": ["id", "kind", "data"],
        "methods": ["POST"],
        "batch_write": True, "batch_size": 100, "flush_interval_ms": 5,
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    handler = (out / "svc" / "handlers" / "events.cpp").read_text()
    # Batch path must route through db.write(endpoint, table, row), not the
    # direct INSERT helper.
    assert 'db.write("/events", "events"' in handler
    # The BatchTableConfig must be registered in build_db_cfg (in service_setup).
    setup = (out / "svc" / "service_setup.cpp").read_text()
    assert 'c.batch.tables["events"]' in setup


def test_credentials_embedded_in_generated_code(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["database"]["user"] = "alice"
    cfg["database"]["password"] = "s3cret"
    cfg["database"]["dbname"]   = "prod"
    cfg["resources"].append({
        "path": "/x", "table": "x", "primary_key": "id",
        "columns": ["id"], "methods": ["GET"],
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    setup = (out / "svc" / "service_setup.cpp").read_text()
    assert '"alice"' in setup and '"s3cret"' in setup and '"prod"' in setup


# ----- new feature coverage -----

def test_filter_emits_param_check(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id", "name", "age"],
        "methods": ["GET"],
        "filters": {
            "min_age": {"column": "age",  "op": "gte"},
            "name":    {"column": "name", "op": "contains"},
        },
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    handler = (out / "svc" / "handlers" / "users.cpp").read_text()
    assert 'qparam(req.query, "min_age")' in handler
    assert 'qparam(req.query, "name")'    in handler
    assert "users.age >= $"      in handler
    assert "users.name ILIKE $"  in handler


def test_filter_unknown_op_rejected(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id", "age"], "methods": ["GET"],
        "filters": {"a": {"column": "age", "op": "bogus"}},
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    proc = run_gen(cfg_path, out, expect_fail=True)
    assert "bogus" in (proc.stderr + proc.stdout)


def test_sort_whitelist_emitted(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id", "name", "created_at"], "methods": ["GET"],
        "sort": {"allowed": ["name", "created_at"], "default": "-created_at"},
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    handler = (out / "svc" / "handlers" / "users.cpp").read_text()
    assert '"name") sqlcol = "users.name"' in handler
    assert '"created_at") sqlcol = "users.created_at"' in handler
    # Default ORDER BY is emitted when caller passes no `sort=`.
    assert "ORDER BY users.created_at DESC" in handler


def test_sort_rejects_column_not_in_resource(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id", "name"], "methods": ["GET"],
        "sort": {"allowed": ["bogus"]},
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    proc = run_gen(cfg_path, out, expect_fail=True)
    assert "bogus" in (proc.stderr + proc.stdout)


def test_pagination_defaults_propagate(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id"], "methods": ["GET"],
        "pagination": {"default": 50, "max": 500},
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    handler = (out / "svc" / "handlers" / "users.cpp").read_text()
    assert "parse_pagination(req.query, 50, 500)" in handler


def test_relations_emit_json_agg(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id", "name"], "methods": ["GET"],
        "relations": {
            "posts": {"table": "posts", "fk": "user_id",
                      "columns": ["id", "title"], "embed": "auto"},
        },
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    handler = (out / "svc" / "handlers" / "users.cpp").read_text()
    assert "json_agg(json_build_object" in handler
    assert "LEFT JOIN posts ON posts.user_id = users.id" in handler
    assert "GROUP BY users.id" in handler
    # Relation-bearing endpoints use the raw passthrough emitter, not
    # spido::Json (which would double-encode the embedded JSON).
    assert "result_to_json_string" in handler


def test_ownership_injection(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id", "name", "owner_id"],
        "methods": ["GET", "POST", "PUT", "DELETE"],
        "ownership": {"column": "owner_id"},  # default header X-User-Id
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    handler = (out / "svc" / "handlers" / "users.cpp").read_text()
    # Every handler reads the ownership header
    assert handler.count('req.header("X-User-Id")') >= 4
    # SELECTs and UPDATE/DELETE all filter by ownership column
    assert "users.owner_id = $" in handler
    # POST overrides the body's ownership column with the trusted header.
    assert "row[2] = spido_pg::PgParam::from_text(owner_id);" in handler


def test_soft_delete_converts_delete_to_update(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id", "name", "deleted_at"],
        "methods": ["GET", "DELETE"],
        "soft_delete": "deleted_at",
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    handler = (out / "svc" / "handlers" / "users.cpp").read_text()
    # DELETE handler emits an UPDATE, not a DELETE FROM.
    assert "UPDATE users SET deleted_at = now()" in handler
    # SELECTs filter out tombstoned rows.
    assert "users.deleted_at IS NULL" in handler


def test_sql_file_override_parsed(tmp_path, tmpdir_factory):
    sql_path = tmp_path / "users.sql"
    sql_path.write_text(
        "-- @list\nSELECT * FROM users {{WHERE}} {{ORDER}} {{LIMIT}}\n"
        "-- @get\nSELECT * FROM users WHERE id = :id\n"
    )
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id"], "methods": ["GET"],
        "sql_file": str(sql_path),
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    # Generator must accept the override file and not crash. (We don't yet
    # consume the parsed blocks downstream — that's a separate step. This
    # test just verifies the parse path is wired up.)
    run_gen(cfg_path, out)


def test_field_validation_emits_validator(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id", "name", "email", "age", "kind"],
        "methods": ["POST", "PUT"],
        "validations": {
            "name":  {"type": "text",  "required": True, "min_length": 1, "max_length": 100},
            "email": {"type": "email", "required": True},
            "age":   {"type": "int",   "min": 0, "max": 150},
            "kind":  {"type": "text",  "enum": ["admin", "user", "guest"]},
        },
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    handler = (out / "svc" / "handlers" / "users.cpp").read_text()
    assert "validate_users(" in handler
    # POST passes is_create=true (enforce required), PUT passes false.
    assert "validate_users(*body, /*is_create=*/true)" in handler
    assert "validate_users(*body, /*is_create=*/false)" in handler
    # Type-specific checks must appear.
    assert "parse_int_strict" in handler
    assert "looks_like_email" in handler
    # Range + length + enum literals propagate.
    assert "below minimum (0)" in handler
    assert "above maximum (150)" in handler
    assert 'too long (max 100)' in handler
    assert '_s != "admin"' in handler


def test_validation_unknown_type_rejected(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id", "x"], "methods": ["POST"],
        "validations": {"x": {"type": "potato"}},
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    proc = run_gen(cfg_path, out, expect_fail=True)
    assert "potato" in (proc.stderr + proc.stdout)


def test_validation_min_on_string_rejected(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id", "name"], "methods": ["POST"],
        "validations": {"name": {"type": "text", "min": 0}},
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    proc = run_gen(cfg_path, out, expect_fail=True)
    assert "min" in (proc.stderr + proc.stdout)


def test_field_masking_emits_projection_helper(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id", "name", "email"],
        "methods": ["GET"],
        "field_masking": True,
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    handler = (out / "svc" / "handlers" / "users.cpp").read_text()
    assert "project_users(std::string_view fields_q)" in handler
    # Whitelist branches per column
    assert 'if (_f == "id")' in handler
    assert 'if (_f == "name")' in handler
    assert 'if (_f == "email")' in handler
    # GET list and GET /:id both call it
    assert handler.count("project_users(qparam(req.query") == 2


def test_include_total_wraps_response(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id", "name"], "methods": ["GET"],
        "pagination": {"default": 25, "max": 100, "include_total": True},
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    handler = (out / "svc" / "handlers" / "users.cpp").read_text()
    # Window function injected into SELECT, response wrapped.
    assert "COUNT(*) OVER () AS _spido_total" in handler
    assert "_spido_total" in handler
    assert "has_next" in handler
    assert "result_to_json_string(pgres, _ncols)" in handler


def test_aggregations_count_endpoint(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id", "name"], "methods": ["GET"],
        "aggregations": {"count": True},
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    handler = (out / "svc" / "handlers" / "users.cpp").read_text()
    assert 'srv.get("/u/count"' in handler
    assert "SELECT COUNT(*) FROM users" in handler
    # Uses the shared where-builder helper (takes a bypass_ownership flag).
    assert "apply_where_users(req, res, sql, params, _bypass_own)" in handler


def test_aggregations_stats_endpoint(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id", "age", "rating"], "methods": ["GET"],
        "aggregations": {
            "stats": {"columns": ["age", "rating"], "ops": ["min", "max", "avg"]},
        },
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    handler = (out / "svc" / "handlers" / "users.cpp").read_text()
    assert 'srv.get("/u/stats"' in handler
    # Each (col, op) pair contributes one expression to the SELECT.
    for col in ("age", "rating"):
        for op in ("MIN", "MAX", "AVG"):
            assert f"{op}(users.{col})" in handler


def test_bulk_endpoint_emitted(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id", "name"], "methods": ["POST"],
        "bulk": {"enabled": True, "max_size": 500},
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    handler = (out / "svc" / "handlers" / "users.cpp").read_text()
    assert 'srv.post("/u/bulk"' in handler
    # Cap from config baked into the size check.
    assert "objects.size() > 500" in handler
    # Direct INSERT path builds a single multi-row VALUES.
    assert "INSERT INTO users" in handler
    assert "split_json_array(req.body)" in handler


def test_idempotency_emits_claim_and_replay(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id"], "methods": ["POST"],
        "idempotency": {"enabled": True},
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    handler = (out / "svc" / "handlers" / "users.cpp").read_text()
    # Pre-claim INSERT with conflict handling
    assert "INSERT INTO idempotency_log (key, endpoint)" in handler
    assert "ON CONFLICT (key) DO NOTHING RETURNING key" in handler
    # Replay fetch
    assert "SELECT status, body FROM idempotency_log" in handler
    # Default header literal embedded
    assert 'req.header("Idempotency-Key")' in handler
    # Store after success
    assert 'UPDATE idempotency_log SET status = $2, body = $3' in handler


def test_permissions_emit_role_checks(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id", "name"],
        "methods": ["GET", "POST", "PUT", "DELETE"],
        "permissions": {
            "list":   ["admin", "viewer"],
            "create": ["admin", "user"],
            "update": ["admin"],
            "delete": ["admin"],
        },
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    handler = (out / "svc" / "handlers" / "users.cpp").read_text()
    # Header mode reads X-User-Roles into _roles_csv and tests with any_role_matches.
    assert handler.count('req.header("X-User-Roles")') >= 4
    assert handler.count("any_role_matches(_roles_csv") >= 4
    assert '"admin"' in handler and '"viewer"' in handler and '"user"' in handler
    assert handler.count("forbidden") >= 4


def test_permissions_bypass_ownership(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id", "name", "owner_id"],
        "methods": ["GET", "POST", "PUT", "DELETE"],
        "ownership": {"column": "owner_id"},
        "permissions": {
            "list": {"roles": ["admin", "user"], "bypass_ownership": ["admin"]},
            "update": {"roles": ["admin", "user"], "bypass_ownership": ["admin"]},
        },
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    handler = (out / "svc" / "handlers" / "users.cpp").read_text()
    # bypass_own variable computed where roles configured
    assert "_bypass_own" in handler
    # Ownership block now conditional in inline list handler
    assert "if (!_bypass_own)" in handler
    # The bypass role list emits as a literal in the role-match call.
    assert '"admin"' in handler and '"user"' in handler


def test_cursor_pagination_emitted(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id", "name"], "methods": ["GET"],
        "pagination": {"mode": "cursor", "default": 20, "max": 100},
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    handler = (out / "svc" / "handlers" / "users.cpp").read_text()
    # Cursor WHERE clause uses pk; ORDER BY pk ASC enforced.
    assert "users.id > $" in handler
    assert "ORDER BY users.id ASC" in handler
    # No OFFSET in cursor mode.
    assert " OFFSET $" not in handler.split('// pagination')[-1].split('auto pgres')[0]
    # Response wrapped with next_cursor.
    assert "next_cursor" in handler
    assert "\\\"next_cursor\\\"" in handler  # the C++ string literal


def test_etag_emits_send_with_etag(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id", "name"], "methods": ["GET"],
        "etag": True,
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    handler = (out / "svc" / "handlers" / "users.cpp").read_text()
    # Both list and get/:id wrap their bodies through send_with_etag(true).
    assert handler.count("send_with_etag(req, res, std::move(_body), 200, true)") == 2


def test_etag_off_passes_false(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id"], "methods": ["GET"],
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    handler = (out / "svc" / "handlers" / "users.cpp").read_text()
    assert "send_with_etag(req, res, std::move(_body), 200, false)" in handler


def test_cursor_rejects_field_masking(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id", "name"], "methods": ["GET"],
        "pagination": {"mode": "cursor"},
        "field_masking": True,
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    proc = run_gen(cfg_path, out, expect_fail=True)
    assert "cursor" in (proc.stderr + proc.stdout)
    assert "field_masking" in (proc.stderr + proc.stdout)


def test_migrations_emitted_for_idempotency(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id"], "methods": ["POST"],
        "idempotency": {"enabled": True},
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    migrations = list((out / "svc" / "migrations").glob("*.sql"))
    assert migrations, "expected migrations/*.sql to be emitted"
    text = migrations[0].read_text()
    assert "CREATE TABLE IF NOT EXISTS idempotency_log" in text
    assert "PRIMARY KEY" in text


def test_jwt_mode_wires_spido_verifier(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["auth"] = {"type": "jwt", "secret": "topsecret",
                   "user_claim": "sub", "roles_claim": "roles"}
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id", "name", "owner_id"], "methods": ["GET", "POST"],
        "ownership": {"column": "owner_id"},
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    main = (out / "svc" / "main.cpp").read_text()
    handler = (out / "svc" / "handlers" / "users.cpp").read_text()
    # main.cpp sets scfg.jwt.enabled = true with our secret
    assert "scfg.jwt.enabled         = true" in main
    assert '"topsecret"' in main
    # Handler reads claims via spido-verified payload, not from X-User-Id.
    assert 'jwt_claim(jwt_payload(req), "sub")' in handler
    # Routes are marked require_auth=true automatically when ownership is set.
    assert "require_auth=*/true" in handler


def test_jwt_mode_roles_from_claim(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["auth"] = {"type": "jwt", "secret": "s",
                   "user_claim": "uid", "roles_claim": "rs"}
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id"], "methods": ["GET"],
        "permissions": {"list": ["admin", "viewer"]},
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    handler = (out / "svc" / "handlers" / "users.cpp").read_text()
    # Roles list comes from the configured JWT claim, not an X-User-Roles header.
    assert 'jwt_claim(jwt_payload(req), "rs")' in handler
    assert 'req.header("X-User-Roles")' not in handler


def test_jwt_missing_secret_rejected(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["auth"] = {"type": "jwt"}    # secret missing
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id"], "methods": ["GET"],
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    proc = run_gen(cfg_path, out, expect_fail=True)
    assert "secret" in (proc.stderr + proc.stdout)


def test_refresh_tokens_emits_endpoints_and_migrations(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["auth"] = {"type": "jwt", "secret": "s",
                   "refresh": {"enabled": True}}
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id"], "methods": ["GET"],
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    svc = out / "svc"
    assert (svc / "auth_endpoints.h").exists()
    assert (svc / "auth_endpoints.cpp").exists()
    auth_cpp = (svc / "auth_endpoints.cpp").read_text()
    assert 'srv.post("/auth/login"'   in auth_cpp
    assert 'srv.post("/auth/refresh"' in auth_cpp
    assert 'srv.post("/auth/logout"'  in auth_cpp
    # Single-use rotation: DELETE-RETURNING then INSERT.
    assert "DELETE FROM refresh_tokens"   in auth_cpp
    assert "RETURNING user_id::text"      in auth_cpp
    # Migrations
    migs = sorted(p.name for p in (svc / "migrations").iterdir())
    assert any("pgcrypto" in m for m in migs)
    assert any("refresh_tokens" in m for m in migs)
    # main.cpp wires the routes
    main = (svc / "main.cpp").read_text()
    assert "svc::auth::register_auth_routes" in main
    cfg = base_cfg()
    # auth omitted → defaults to header mode (backward compat)
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id", "owner_id"], "methods": ["GET"],
        "ownership": {"column": "owner_id"},
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    main = (out / "svc" / "main.cpp").read_text()
    handler = (out / "svc" / "handlers" / "users.cpp").read_text()
    # No JWT wiring in main when auth defaults to header
    assert "scfg.jwt" not in main
    # Handler still reads from X-User-Id header
    assert 'req.header("X-User-Id")' in handler
    assert "jwt_claim" not in handler


def test_files_emit_presigned_endpoint(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["auth"] = {"type": "jwt", "secret": "s"}
    cfg["files"] = {
        "enabled": True, "region": "us-east-1", "bucket": "buk",
        "access_key": "AKIA", "secret_key": "sekret",
    }
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id"], "methods": ["GET"],
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    svc = out / "svc"
    files_cpp = (svc / "files_endpoints.cpp").read_text()
    assert 'srv.post("/files/upload-url"'    in files_cpp
    assert 'srv.post("/files/:id/confirm"'   in files_cpp
    # Credentials inlined as constexpr at codegen time
    assert "\"AKIA\""    in files_cpp
    assert "\"buk\""     in files_cpp
    # AWS V4 signing pieces
    assert "AWS4-HMAC-SHA256" in files_cpp
    assert "UNSIGNED-PAYLOAD" in files_cpp
    # Migration
    migs = sorted(p.name for p in (svc / "migrations").iterdir())
    assert any("files" in m for m in migs)


def test_files_requires_jwt(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    # No auth block — defaults to header mode
    cfg["files"] = {
        "enabled": True, "region": "us-east-1", "bucket": "buk",
        "access_key": "k", "secret_key": "s",
    }
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id"], "methods": ["GET"],
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    proc = run_gen(cfg_path, out, expect_fail=True)
    assert "files.enabled requires auth.type='jwt'" in (proc.stderr + proc.stdout)


def test_push_emits_device_endpoints_and_queue(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["auth"] = {"type": "jwt", "secret": "s"}
    cfg["push"] = {"enabled": True}
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id"], "methods": ["GET"],
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    svc = out / "svc"
    push_cpp = (svc / "push_endpoints.cpp").read_text()
    assert 'srv.post("/push/register-device"' in push_cpp
    assert 'srv.del("/push/register-device"'  in push_cpp
    # platform must be ios/android — guard generated
    assert '"ios" && *platform != "android"'  in push_cpp
    # Migrations: both tables
    migs = sorted(p.name for p in (svc / "migrations").iterdir())
    assert any("device_tokens" in m for m in migs)
    assert any("push_queue"    in m for m in migs)


def test_transactional_hooks_single_conn_flow(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id", "name"], "methods": ["POST"],
        "hooks": {
            "before_insert": "SELECT check_quota($2)",
            "after_insert":  "INSERT INTO audit (pk, uid) VALUES ($1, $2)",
            "transactional": True,
        },
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    handler = (out / "svc" / "handlers" / "users.cpp").read_text()
    # Single-conn checkout + BEGIN/COMMIT/ROLLBACK
    assert "db.pool().checkout()" in handler
    assert '_conn->exec("BEGIN")'  in handler
    assert '_conn->exec("COMMIT")' in handler
    assert '_conn->exec("ROLLBACK")' in handler
    # Hooks inlined via the params substitution helper
    assert "inline_params" in handler


def test_transactional_off_uses_non_tx_path(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id"], "methods": ["POST"],
        "hooks": {"before_insert": "SELECT 1"},
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    handler = (out / "svc" / "handlers" / "users.cpp").read_text()
    assert 'db.pool().checkout()' not in handler
    assert '_conn->exec("BEGIN")'  not in handler


def test_no_migrations_when_no_idempotency(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id"], "methods": ["GET"],
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    assert not (out / "svc" / "migrations").exists()


def test_permissions_custom_header(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id"], "methods": ["GET"],
        "permissions": {"header": "X-Roles", "list": ["admin"]},
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    handler = (out / "svc" / "handlers" / "users.cpp").read_text()
    assert 'req.header("X-Roles")' in handler


def test_hooks_emitted_for_direct_path(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id", "name"],
        "methods": ["POST", "PUT", "DELETE"],
        "hooks": {
            "before_insert": "SELECT check_quota($2)",
            "after_insert":  "INSERT INTO audit VALUES ('users','i',$1,$2)",
            "before_delete": "SELECT can_delete($1, $2)",
        },
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    handler = (out / "svc" / "handlers" / "users.cpp").read_text()
    assert 'hook_bi_users' in handler
    assert 'hook_ai_users' in handler
    assert 'hook_bd_users' in handler
    assert 'SELECT check_quota($2)' in handler
    assert 'SELECT can_delete($1, $2)' in handler
    # after-hooks fall through on error (best-effort logging)
    assert 'after_insert hook failed' in handler


def test_openapi_emitted(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id", "name", "age"],
        "methods": ["GET", "POST", "PUT", "DELETE"],
        "validations": {
            "name": {"type": "text",  "required": True, "max_length": 100},
            "age":  {"type": "int",   "min": 0, "max": 150},
        },
        "filters":   {"min_age": {"column": "age", "op": "gte"}},
        "sort":      {"allowed": ["name"], "default": "-name"},
        "pagination": {"include_total": True},
        "aggregations": {"count": True, "stats": {"columns": ["age"], "ops": ["avg"]}},
        "bulk": {"enabled": True},
        "ownership": {"column": "id"},
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    spec_path = out / "svc" / "openapi.json"
    assert spec_path.exists()
    spec = json.loads(spec_path.read_text())

    assert spec["openapi"].startswith("3.")
    paths = spec["paths"]
    # Every generated route shows up in the spec.
    assert "/u" in paths and "get" in paths["/u"] and "post" in paths["/u"]
    assert "/u/{id}" in paths and {"get", "put", "delete"} <= paths["/u/{id}"].keys()
    assert "/u/count" in paths
    assert "/u/stats" in paths
    assert "/u/bulk"  in paths

    # The list response wraps with meta when include_total=true.
    list_schema = paths["/u"]["get"]["responses"]["200"]["content"]["application/json"]["schema"]
    assert list_schema["type"] == "object"
    assert "data" in list_schema["properties"]
    assert "meta" in list_schema["properties"]

    # Validation rules propagate to schema constraints.
    users_schema = spec["components"]["schemas"]["Users"]
    assert users_schema["properties"]["age"] == {
        "type": "integer", "format": "int32", "minimum": 0, "maximum": 150,
    }
    assert "name" in users_schema.get("required", [])

    # Ownership header documented as required for protected paths.
    list_params = paths["/u"]["get"]["parameters"]
    assert any(p.get("name") == "X-User-Id" and p["in"] == "header"
               for p in list_params)


def test_hooks_unknown_event_rejected(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id"], "methods": ["POST"],
        "hooks": {"on_blue_moon": "SELECT 1"},
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    proc = run_gen(cfg_path, out, expect_fail=True)
    assert "on_blue_moon" in (proc.stderr + proc.stdout)


def test_permissions_unknown_op_rejected(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id"], "methods": ["GET"],
        "permissions": {"fly": ["admin"]},
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    proc = run_gen(cfg_path, out, expect_fail=True)
    assert "fly" in (proc.stderr + proc.stdout)


def test_idempotency_custom_table_and_header(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id"], "methods": ["POST"],
        "idempotency": {"enabled": True, "table": "idem_v2", "header": "X-Req-Key"},
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    handler = (out / "svc" / "handlers" / "users.cpp").read_text()
    assert 'req.header("X-Req-Key")' in handler
    assert 'INSERT INTO idem_v2 (key, endpoint)' in handler


def test_bulk_endpoint_off_skipped(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id"], "methods": ["POST"],
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    handler = (out / "svc" / "handlers" / "users.cpp").read_text()
    assert "/bulk" not in handler
    assert "split_json_array" not in handler


def test_aggregations_stats_rejects_unknown_op(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id", "age"], "methods": ["GET"],
        "aggregations": {"stats": {"columns": ["age"], "ops": ["bogus"]}},
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    proc = run_gen(cfg_path, out, expect_fail=True)
    assert "bogus" in (proc.stderr + proc.stdout)


def test_include_total_off_emits_plain_array(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id", "name"], "methods": ["GET"],
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    handler = (out / "svc" / "handlers" / "users.cpp").read_text()
    assert "COUNT(*) OVER ()" not in handler
    assert "_spido_total"     not in handler


def test_field_masking_off_keeps_static_select(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id", "name"], "methods": ["GET"],
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    handler = (out / "svc" / "handlers" / "users.cpp").read_text()
    assert "project_users" not in handler
    assert 'SELECT users.id, users.name FROM' in handler


def test_sql_file_missing_errors(tmp_path, tmpdir_factory):
    cfg = base_cfg()
    cfg["resources"].append({
        "path": "/u", "table": "users", "primary_key": "id",
        "columns": ["id"], "methods": ["GET"],
        "sql_file": "/nope/does_not_exist.sql",
    })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    proc = run_gen(cfg_path, out, expect_fail=True)
    assert "sql_file" in (proc.stderr + proc.stdout)


# ----- syntactic compile-check -----

@pytest.mark.skipif(shutil.which("g++") is None, reason="g++ not in PATH")
@pytest.mark.parametrize("features", ["minimal", "full"])
def test_generated_sources_compile_syntactically(tmp_path, tmpdir_factory, features):
    """g++ -fsyntax-only on every generated .cpp.

    We don't expect a full link — that needs libspido + libspido_pg + the
    right liburing — but the parser must accept every translation unit.
    Catches Jinja typos (trailing commas, broken braces, missing includes,
    namespace mistakes) across all the split files at once.

    Runs twice: once on a minimal config (no filters/sort/relations/etc),
    once with everything enabled — exercises both Jinja branches.
    """
    cfg = base_cfg()
    if features == "minimal":
        cfg["resources"].append({
            "path": "/u", "table": "users", "primary_key": "id",
            "columns": ["id", "name"],
            "methods": ["GET", "POST", "PUT", "DELETE"],
        })
    else:
        cfg["resources"].append({
            "path": "/u", "table": "users", "primary_key": "id",
            "columns": ["id", "name", "age", "owner_id", "deleted_at"],
            "methods": ["GET", "POST", "PUT", "DELETE"],
            "filters": {
                "min_age": {"column": "age",  "op": "gte"},
                "name":    {"column": "name", "op": "contains"},
                "kind":    {"column": "name", "op": "in"},
                "tombstoned": {"column": "deleted_at", "op": "not_null"},
            },
            "sort": {"allowed": ["name", "age"], "default": "-age"},
            "pagination": {"default": 25, "max": 200},
            "relations": {
                "posts": {"table": "posts", "fk": "user_id",
                          "columns": ["id", "title"], "embed": "auto"},
            },
            "ownership": {"column": "owner_id"},
            "soft_delete": "deleted_at",
        })
    cfg_path = tmp_path / "cfg.json"; write_cfg(cfg_path, cfg)
    out = tmpdir_factory()
    run_gen(cfg_path, out)
    svc = out / "svc"

    sources = sorted(svc.rglob("*.cpp"))
    assert sources, "generator produced no .cpp files"

    failures = []
    for src in sources:
        proc = subprocess.run([
            "g++", "-std=c++20", "-fsyntax-only",
            "-I", str(svc),
            "-I", str(ROOT / "spido-pg" / "include"),
            "-I", str(ROOT / "spido" / "include"),
            str(src),
        ], capture_output=True, text=True)
        if proc.returncode != 0:
            failures.append(f"--- {src.relative_to(svc)} ---\n"
                            f"stdout: {proc.stdout}\nstderr: {proc.stderr}")
    assert not failures, "syntax errors:\n" + "\n".join(failures)
