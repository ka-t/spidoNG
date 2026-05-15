#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spido_pg {

// PostgreSQL OIDs we care about (from src/include/catalog/pg_type.dat).
enum class Oid : uint32_t {
    Bool        = 16,
    Bytea       = 17,
    Int2        = 21,
    Int4        = 23,
    Int8        = 20,
    Float4      = 700,
    Float8      = 701,
    Text        = 25,
    Varchar     = 1043,
    TimestampTz = 1184,
    Timestamp   = 1114,
    Uuid        = 2950,
    Numeric     = 1700,
    Json        = 114,
    Jsonb       = 3802,
    Unknown     = 0,
};

// A single bound parameter for an extended-query Bind.
//
// `value` holds the raw payload. For binary format (`text_format=false`)
// numeric types are encoded network-byte-order — the factory helpers below
// do the conversion for you. For text format (`text_format=true`) we just
// pass the bytes verbatim; PG parses them according to the column type the
// prepared statement was Parse'd with (or infers from context if `type` is
// Oid::Unknown). Text format is what the generator uses for inserts driven
// by JSON bodies, since the wire payload is literally the JSON value's
// printable form.
//
// is_null=true → -1 length on the wire (PgSQL NULL); `value` is ignored.
struct PgParam {
    Oid                   type        = Oid::Unknown;
    bool                  is_null     = false;
    bool                  text_format = false;
    std::string           value;

    static PgParam null()                              { PgParam p; p.is_null = true; return p; }
    static PgParam i2  (int16_t v);
    static PgParam i4  (int32_t v);
    static PgParam i8  (int64_t v);
    static PgParam f4  (float v);
    static PgParam f8  (double v);
    static PgParam b   (bool v);
    static PgParam text(std::string_view v);
    static PgParam bytea(std::span<const uint8_t> v);

    // Text-format param. Used by the generator: each JSON body value is
    // serialized to its canonical string ("42", "true", "John"), and PG
    // casts according to the destination column's type. PG accepts both
    // unquoted JSON-style values for numbers/bools and raw strings for
    // text.
    static PgParam from_text(std::string_view v) {
        PgParam p; p.text_format = true; p.value.assign(v); return p;
    }
};

// One column header from a RowDescription.
struct PgField {
    std::string name;
    Oid         type_oid = Oid::Unknown;
    int16_t     format   = 1;       // 1 = binary, 0 = text
    int16_t     type_len = -1;
};

// One row from DataRow. Each cell is either null or a raw byte payload
// (binary format, network byte order). Lifetime tied to PgResult.
struct PgRow {
    // -1 length cells become empty string + null=true via the parallel
    // nulls bitmap.
    std::vector<std::string> cells;
    std::vector<bool>        nulls;
};

// Result of one query / portal execution.
struct PgResult {
    std::vector<PgField> fields;
    std::vector<PgRow>   rows;

    // CommandComplete tag, e.g. "SELECT 3", "INSERT 0 5". Useful for
    // extracting affected-row counts without parsing every DataRow.
    std::string tag;

    // 'I' (idle), 'T' (in transaction), 'E' (transaction failed).
    char tx_status = 'I';

    // Set on protocol-level error (ErrorResponse from server).
    bool        had_error = false;
    std::string error_severity;
    std::string error_code;       // SQLSTATE
    std::string error_message;

    bool ok() const noexcept { return !had_error; }

    // Convenience accessors. Binary decoders, return nullopt on null.
    std::optional<int32_t>     i4 (size_t row, size_t col) const;
    std::optional<int64_t>     i8 (size_t row, size_t col) const;
    std::optional<double>      f8 (size_t row, size_t col) const;
    std::optional<bool>        b  (size_t row, size_t col) const;
    std::optional<std::string_view> text(size_t row, size_t col) const;
};

// Prepared-statement handle. Just the wire-level name plus parameter types
// the connection re-uses on every exec_prepared.
struct StmtHandle {
    std::string         name;
    std::vector<Oid>    param_types;
    // Bumped on reconnect to force re-Parse on next use.
    uint64_t            epoch = 0;
};

enum class ConnState : uint8_t {
    Disconnected = 0,
    Connecting,
    Authenticating,
    Ready,
    Busy,
    Error,
};

// One PostgreSQL backend connection, blocking semantics. The implementation
// uses io_uring under the hood (single-shot read/write SQEs) but the public
// API hides that — exec() blocks the calling worker thread until the
// CommandComplete + ReadyForQuery have been consumed.
//
// Not thread-safe. Owners must serialize access (the pool does this by
// only handing one connection to one borrower at a time).
class PgConnection {
public:
    PgConnection();
    ~PgConnection();

    PgConnection(const PgConnection&)            = delete;
    PgConnection& operator=(const PgConnection&) = delete;
    PgConnection(PgConnection&&) noexcept;
    PgConnection& operator=(PgConnection&&) noexcept;

    bool connect(std::string_view socket_path,
                 std::string_view user,
                 std::string_view password,
                 std::string_view dbname);

    PgResult   exec(std::string_view sql);
    StmtHandle prepare(std::string_view name,
                       std::string_view sql,
                       std::span<const Oid> param_types);
    PgResult   exec_prepared(const StmtHandle& s, std::span<const PgParam> params);

    void close() noexcept;
    bool is_ready() const noexcept { return state_ == ConnState::Ready; }

    int                fd()    const noexcept;
    int32_t            pid()   const noexcept { return backend_pid_; }
    std::string_view   last_error() const noexcept { return last_error_; }
    uint64_t           epoch() const noexcept { return epoch_; }

private:
    bool authenticate(std::string_view user,
                      std::string_view password,
                      std::string_view dbname);
    bool send_all(const void* buf, size_t len);
    bool recv_msg(char& type, std::vector<uint8_t>& payload);
    bool drain_until_ready(PgResult* out);

    int                                fd_ = -1;
    ConnState                          state_ = ConnState::Disconnected;
    int32_t                            backend_pid_ = 0;
    int32_t                            secret_key_  = 0;
    uint64_t                           epoch_       = 1;
    std::string                        last_error_;
    std::unordered_map<std::string,std::string> server_params_;

    // SCRAM-SHA-256 in-progress state. Set during the AuthSASL handshake,
    // consumed during AuthSASLContinue + AuthSASLFinal. Defined opaquely
    // in scram.h (private internal header in src/).
    struct ScramClientFirst {
        std::string gs2_header;
        std::string bare;
        std::string client_nonce;
        std::string full;
    };
    ScramClientFirst                   pending_scram_client_first_;
    std::string                        pending_scram_server_sig_;

    // io_uring instance. Owned by the connection unless attached to an
    // external one — see attach_ring(). Hidden behind a pimpl-style
    // pointer so consumers don't have to pull liburing in.
    struct Ring;
    std::unique_ptr<Ring> ring_;
};

} // namespace spido_pg
