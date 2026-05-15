#pragma once

// Shared runtime helpers used by generator-emitted HTTP handlers:
//   * percent-decoded query-string parser
//   * filter / sort / pagination structs
//   * safe WHERE / ORDER BY / LIMIT builders
//
// Header-only because the generator emits per-resource code that inlines
// the column whitelist into each call site — the helpers below operate
// against compile-time-known column tables rather than runtime maps.

#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "spido_pg/connection.h"
#include "spido_pg/string_map.h"

namespace spido_pg {

// ---------- query-string parser ----------

// Percent-decode a token. '+' → space when in_query=true.
inline std::string url_decode(std::string_view s, bool in_query = true) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '%' && i + 2 < s.size()) {
            auto hex = [](char x) -> int {
                if (x >= '0' && x <= '9') return x - '0';
                if (x >= 'a' && x <= 'f') return x - 'a' + 10;
                if (x >= 'A' && x <= 'F') return x - 'A' + 10;
                return -1;
            };
            int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        if (c == '+' && in_query) { out.push_back(' '); continue; }
        out.push_back(c);
    }
    return out;
}

// Parse "k1=v1&k2=v2&..." into an owning map. Empty value if missing '='.
// Repeated keys: last-wins for single get(), but the full list is preserved
// via QueryParams::all().
class QueryParams {
public:
    void parse(std::string_view raw) {
        size_t i = 0;
        while (i < raw.size()) {
            size_t amp = raw.find('&', i);
            if (amp == std::string_view::npos) amp = raw.size();
            std::string_view kv = raw.substr(i, amp - i);
            size_t eq = kv.find('=');
            std::string key, val;
            if (eq == std::string_view::npos) {
                key = url_decode(kv);
            } else {
                key = url_decode(kv.substr(0, eq));
                val = url_decode(kv.substr(eq + 1));
            }
            if (!key.empty()) entries_.emplace_back(std::move(key), std::move(val));
            i = amp + 1;
        }
    }

    // Last value for `key`, or empty if absent. Returns view into stored data.
    std::string_view get(std::string_view key) const noexcept {
        for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
            if (it->first == key) return it->second;
        }
        return {};
    }

    bool has(std::string_view key) const noexcept {
        for (auto& [k, _] : entries_) if (k == key) return true;
        return false;
    }

    // All values for `key`, in insertion order. Empty vector if absent.
    std::vector<std::string_view> all(std::string_view key) const {
        std::vector<std::string_view> out;
        for (auto& [k, v] : entries_) if (k == key) out.push_back(v);
        return out;
    }

    const std::vector<std::pair<std::string, std::string>>& entries() const noexcept {
        return entries_;
    }

private:
    std::vector<std::pair<std::string, std::string>> entries_;
};

// ---------- filter representation ----------

enum class FilterOp : uint8_t {
    Eq, Neq, Gt, Gte, Lt, Lte, Like, ILike, In, NotIn, IsNull, IsNotNull, Between,
};

inline std::string_view filter_op_sql(FilterOp op) {
    switch (op) {
        case FilterOp::Eq:        return "=";
        case FilterOp::Neq:       return "<>";
        case FilterOp::Gt:        return ">";
        case FilterOp::Gte:       return ">=";
        case FilterOp::Lt:        return "<";
        case FilterOp::Lte:       return "<=";
        case FilterOp::Like:      return "LIKE";
        case FilterOp::ILike:     return "ILIKE";
        case FilterOp::In:        return "IN";
        case FilterOp::NotIn:     return "NOT IN";
        case FilterOp::IsNull:    return "IS NULL";
        case FilterOp::IsNotNull: return "IS NOT NULL";
        case FilterOp::Between:   return "BETWEEN";
    }
    return "=";
}

// One filter clause already resolved against the column whitelist. The
// generator emits a `parse_filters_<table>(req, out)` function that maps
// query params → these.
struct FilterClause {
    std::string_view column;     // generator-side constant, no allocation
    FilterOp         op;
    std::vector<PgParam> values; // 1 (most ops) / 2 (between) / n (in)
};

// ---------- sort ----------

struct SortKey {
    std::string_view column;
    bool             descending = false;
};

// Parse "?sort=name,-age" against an allowed set. Unknown columns dropped.
// Caller passes the whitelist as a span of string_view — generator emits a
// constexpr array per resource.
inline std::vector<SortKey> parse_sort(std::string_view raw,
                                       std::span<const std::string_view> allowed)
{
    std::vector<SortKey> out;
    size_t i = 0;
    while (i < raw.size()) {
        size_t comma = raw.find(',', i);
        if (comma == std::string_view::npos) comma = raw.size();
        std::string_view tok = raw.substr(i, comma - i);
        i = comma + 1;
        if (tok.empty()) continue;
        bool desc = false;
        if (tok.front() == '-') { desc = true; tok.remove_prefix(1); }
        else if (tok.front() == '+') tok.remove_prefix(1);
        for (auto col : allowed) {
            if (col == tok) { out.push_back({col, desc}); break; }
        }
    }
    return out;
}

// ---------- pagination ----------

struct Pagination {
    uint32_t limit  = 20;
    uint32_t offset = 0;
};

inline Pagination parse_pagination(const QueryParams& qp,
                                   uint32_t default_size,
                                   uint32_t max_size)
{
    Pagination p;
    p.limit = default_size;
    auto parse_uint = [](std::string_view s, uint32_t& out) -> bool {
        if (s.empty()) return false;
        uint32_t v = 0;
        auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
        if (ec != std::errc()) return false;
        out = v;
        return true;
    };
    uint32_t v = 0;
    if (parse_uint(qp.get("page_size"), v) || parse_uint(qp.get("limit"), v)) {
        if (v == 0) v = default_size;
        if (v > max_size) v = max_size;
        p.limit = v;
    }
    if (parse_uint(qp.get("offset"), v)) {
        p.offset = v;
    } else if (parse_uint(qp.get("page"), v)) {
        if (v > 0) p.offset = (v - 1) * p.limit;
    }
    return p;
}

// ---------- WHERE / ORDER / LIMIT builders ----------

// Appends a parameter placeholder ($N) and pushes its value. Returns the
// 1-based index of the param it just bound.
inline size_t bind_param(std::string& sql, std::vector<PgParam>& params, PgParam v) {
    params.push_back(std::move(v));
    sql.push_back('$');
    sql.append(std::to_string(params.size()));
    return params.size();
}

// Append one filter clause as "<col> <op> <placeholder>".  Returns false
// (no-op) if the clause is malformed (e.g. IN with empty list) so the
// caller can keep the surrounding " AND " logic simple.
inline bool append_filter(std::string& sql,
                          std::vector<PgParam>& params,
                          const FilterClause& f)
{
    switch (f.op) {
        case FilterOp::IsNull:
        case FilterOp::IsNotNull:
            sql.append(f.column);
            sql.push_back(' ');
            sql.append(filter_op_sql(f.op));
            return true;

        case FilterOp::Between:
            if (f.values.size() != 2) return false;
            sql.append(f.column);
            sql.append(" BETWEEN ");
            bind_param(sql, params, f.values[0]);
            sql.append(" AND ");
            bind_param(sql, params, f.values[1]);
            return true;

        case FilterOp::In:
        case FilterOp::NotIn: {
            if (f.values.empty()) return false;
            sql.append(f.column);
            sql.push_back(' ');
            sql.append(filter_op_sql(f.op));
            sql.append(" (");
            for (size_t i = 0; i < f.values.size(); ++i) {
                if (i) sql.push_back(',');
                bind_param(sql, params, f.values[i]);
            }
            sql.push_back(')');
            return true;
        }

        default: {
            if (f.values.size() != 1) return false;
            sql.append(f.column);
            sql.push_back(' ');
            sql.append(filter_op_sql(f.op));
            sql.push_back(' ');
            bind_param(sql, params, f.values[0]);
            return true;
        }
    }
}

// Build a clause section onto `sql`. `leading` is the first separator
// emitted before any clause ("WHERE" or "AND"); subsequent clauses use
// " AND ". Returns true if any clause was emitted. Filters whose append
// fails (e.g. empty IN) are skipped silently.
inline bool append_clause_section(std::string& sql,
                                  std::vector<PgParam>& params,
                                  std::span<const FilterClause> filters,
                                  std::span<const std::string_view> raw_clauses,
                                  std::string_view leading)
{
    bool started = false;
    auto sep = [&]() {
        sql.push_back(' ');
        if (!started) { sql.append(leading); started = true; }
        else          { sql.append("AND"); }
        sql.push_back(' ');
    };

    std::string staging;
    for (const auto& f : filters) {
        staging.clear();
        if (append_filter(staging, params, f)) {
            sep();
            sql.append(staging);
        }
    }
    for (auto c : raw_clauses) {
        if (c.empty()) continue;
        sep();
        sql.append(c);
    }
    return started;
}

// "SELECT ... FROM t{{WHERE}}..." — produces " WHERE c1 AND c2" or "".
inline void build_where_section(std::string& out,
                                std::vector<PgParam>& params,
                                std::span<const FilterClause> filters,
                                std::span<const std::string_view> raw = {})
{
    append_clause_section(out, params, filters, raw, "WHERE");
}

// "... WHERE pk=$1{{ANDWHERE}}" — produces " AND c1 AND c2" or "".
inline void build_and_section(std::string& out,
                              std::vector<PgParam>& params,
                              std::span<const FilterClause> filters,
                              std::span<const std::string_view> raw = {})
{
    append_clause_section(out, params, filters, raw, "AND");
}

// In-place replace the FIRST occurrence of `tag` with `val`. No-op if
// tag missing. Used to substitute {{WHERE}} / {{ORDER}} / {{LIMIT}} /
// {{ANDWHERE}} into a SQL template that was emitted at gen-time.
inline void replace_placeholder(std::string& sql,
                                std::string_view tag,
                                std::string_view val)
{
    auto p = sql.find(tag);
    if (p != std::string::npos) sql.replace(p, tag.size(), val);
}

inline void append_order(std::string& sql, std::span<const SortKey> keys) {
    if (keys.empty()) return;
    sql.append(" ORDER BY ");
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i) sql.push_back(',');
        sql.append(keys[i].column);
        sql.append(keys[i].descending ? " DESC" : " ASC");
    }
}

inline void append_limit(std::string& sql,
                         std::vector<PgParam>& params,
                         const Pagination& p)
{
    sql.append(" LIMIT ");
    bind_param(sql, params, PgParam::i8(static_cast<int64_t>(p.limit)));
    sql.append(" OFFSET ");
    bind_param(sql, params, PgParam::i8(static_cast<int64_t>(p.offset)));
}

// ---------- small parsing helpers ----------

// Split a comma-separated value (for IN filters: "?kind=a,b,c").
inline std::vector<std::string_view> split_csv(std::string_view s) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < s.size()) {
        size_t c = s.find(',', i);
        if (c == std::string_view::npos) c = s.size();
        if (c > i) out.push_back(s.substr(i, c - i));
        i = c + 1;
    }
    return out;
}

// ---------- JSON cell writers (for generated row serializers) ----------
//
// db.query()/query_cached() use PG simple-query protocol, so all result
// cells come back in TEXT format. That lets us serialize directly to JSON
// without per-cell binary decoding: numbers are already JSON-compatible
// (e.g. "42", "3.14"), bools just need "t"/"f" → true/false, text needs
// escaping, json/jsonb is already JSON.
//
// Each helper checks the null bitmap and emits "null" on missing cells.

inline void write_json_string(std::string& out, std::string_view s) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"':  out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            case '\b': out.append("\\b"); break;
            case '\f': out.append("\\f"); break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned char>(c));
                    out.append(buf);
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

inline bool cell_is_null(const PgResult& r, size_t row, size_t col) noexcept {
    return row >= r.rows.size()
        || col >= r.rows[row].nulls.size()
        || r.rows[row].nulls[col];
}

inline void write_cell_int(std::string& out, const PgResult& r, size_t row, size_t col) {
    if (cell_is_null(r, row, col)) { out.append("null"); return; }
    auto v = r.text(row, col);
    if (!v || v->empty()) { out.append("null"); return; }
    // PG text rep is digits with optional leading '-'. Trust it as JSON number.
    out.append(*v);
}

inline void write_cell_float(std::string& out, const PgResult& r, size_t row, size_t col) {
    if (cell_is_null(r, row, col)) { out.append("null"); return; }
    auto v = r.text(row, col);
    if (!v || v->empty()) { out.append("null"); return; }
    out.append(*v);
}

inline void write_cell_bool(std::string& out, const PgResult& r, size_t row, size_t col) {
    if (cell_is_null(r, row, col)) { out.append("null"); return; }
    auto v = r.text(row, col);
    if (!v || v->empty()) { out.append("null"); return; }
    out.append((*v)[0] == 't' ? "true" : "false");
}

inline void write_cell_text(std::string& out, const PgResult& r, size_t row, size_t col) {
    if (cell_is_null(r, row, col)) { out.append("null"); return; }
    auto v = r.text(row, col);
    if (!v) { out.append("null"); return; }
    write_json_string(out, *v);
}

// json / jsonb / array columns — the text representation is already valid
// JSON, so we embed it verbatim. Falls back to "null" on missing cells.
inline void write_cell_raw_json(std::string& out, const PgResult& r, size_t row, size_t col) {
    if (cell_is_null(r, row, col)) { out.append("null"); return; }
    auto v = r.text(row, col);
    if (!v || v->empty()) { out.append("null"); return; }
    out.append(*v);
}

// ---------- ownership helper ----------

// One-liner the generator emits when ownership is configured. Pushes the
// owner param and returns its placeholder string already formatted as
// "<column> = $N" so the caller can hand it to append_where via raw_clauses.
// We stash the formatted clause in a caller-owned buffer to keep it alive
// for the duration of the WHERE build.
inline std::string_view bind_ownership(std::string& clause_buf,
                                       std::vector<PgParam>& params,
                                       std::string_view column,
                                       PgParam value)
{
    params.push_back(std::move(value));
    clause_buf.clear();
    clause_buf.append(column);
    clause_buf.append(" = $");
    clause_buf.append(std::to_string(params.size()));
    return clause_buf;
}

} // namespace spido_pg
