// PostgreSQL frontend wire protocol (version 3).
//
// We bypass libpq and speak the protocol directly over an AF_UNIX socket.
// I/O is driven through io_uring (one ring per connection): every send and
// recv becomes a single-shot SQE. The completion is harvested via
// io_uring_wait_cqe, so the public API still looks synchronous — the
// caller's worker thread parks in the kernel until bytes are available
// rather than spinning on read()/write(). That keeps us off the libc poll()
// path and matches what libspido does for its HTTP sockets.

#include "spido_pg/connection.h"

#include "md5.h"
#include "scram.h"
#include "wire.h"

#include <liburing.h>

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>

namespace spido_pg {

namespace {

constexpr unsigned kRingEntries = 8;
constexpr size_t   kRecvChunk   = 16 * 1024;
constexpr int      kIOTimeoutMs = 30 * 1000;

// Re-host the IEEE 754 layout we get from the host into network byte order.
// We assume host floats are IEEE 754 (true on every platform we care about);
// only the byte order needs flipping for big-endian wire format.
uint32_t htonf(float v)  { uint32_t u; std::memcpy(&u, &v, 4); return htonl(u); }
uint64_t htondll(double v) {
    uint64_t u; std::memcpy(&u, &v, 8);
    uint64_t r = 0;
    for (int i = 0; i < 8; ++i) r = (r << 8) | ((u >> (i*8)) & 0xff);
    return r;
}
double   ntohdll(uint64_t u) {
    uint64_t r = 0;
    for (int i = 0; i < 8; ++i) r = (r << 8) | ((u >> (i*8)) & 0xff);
    double v; std::memcpy(&v, &r, 8); return v;
}

} // namespace

// ---------- Ring (io_uring wrapper) ----------

struct PgConnection::Ring {
    io_uring ring{};
    bool     inited = false;

    bool init() {
        if (inited) return true;
        io_uring_params p{};
        if (int rc = io_uring_queue_init_params(kRingEntries, &ring, &p); rc < 0) {
            errno = -rc; return false;
        }
        inited = true;
        return true;
    }

    ~Ring() { if (inited) io_uring_queue_exit(&ring); }

    // Returns >= 0 bytes transferred, or -errno on failure.
    int blocking_io(int fd, void* buf, size_t len, bool is_write) {
        if (!inited && !init()) return -errno;
        io_uring_sqe* sqe = io_uring_get_sqe(&ring);
        if (!sqe) return -ENOSPC;
        if (is_write) io_uring_prep_send(sqe, fd, buf, len, MSG_NOSIGNAL);
        else          io_uring_prep_recv(sqe, fd, buf, len, 0);
        io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(1));

        // Hard timeout SQE linked to the I/O — if the kernel hasn't completed
        // within kIOTimeoutMs, the I/O is cancelled with -ECANCELED. Prevents
        // the worker from sleeping forever if PG goes silent mid-frame.
        __kernel_timespec ts{ kIOTimeoutMs/1000, (kIOTimeoutMs%1000)*1000000L };
        sqe->flags |= IOSQE_IO_LINK;
        io_uring_sqe* tsqe = io_uring_get_sqe(&ring);
        if (!tsqe) return -ENOSPC;
        io_uring_prep_link_timeout(tsqe, &ts, 0);
        io_uring_sqe_set_data(tsqe, reinterpret_cast<void*>(2));

        if (int rc = io_uring_submit(&ring); rc < 0) return rc;

        int result = -EIO;
        for (int harvested = 0; harvested < 2; ++harvested) {
            io_uring_cqe* cqe = nullptr;
            int rc = io_uring_wait_cqe(&ring, &cqe);
            if (rc < 0) return rc;
            uintptr_t tag = reinterpret_cast<uintptr_t>(io_uring_cqe_get_data(cqe));
            int res = cqe->res;
            io_uring_cqe_seen(&ring, cqe);
            if (tag == 1) result = res;
            // tag==2: link timeout completion; ignore unless it cancelled us.
        }
        return result;
    }
};

// ---------- PgParam constructors ----------

PgParam PgParam::i2(int16_t v) {
    PgParam p; p.type = Oid::Int2; p.value.resize(2);
    uint16_t u = htons(static_cast<uint16_t>(v));
    std::memcpy(p.value.data(), &u, 2); return p;
}
PgParam PgParam::i4(int32_t v) {
    PgParam p; p.type = Oid::Int4; p.value.resize(4);
    uint32_t u = htonl(static_cast<uint32_t>(v));
    std::memcpy(p.value.data(), &u, 4); return p;
}
PgParam PgParam::i8(int64_t v) {
    PgParam p; p.type = Oid::Int8; p.value.resize(8);
    uint64_t u = static_cast<uint64_t>(v);
    uint64_t r = 0;
    for (int i = 0; i < 8; ++i) r = (r << 8) | ((u >> (i*8)) & 0xff);
    std::memcpy(p.value.data(), &r, 8); return p;
}
PgParam PgParam::f4(float v)  {
    PgParam p; p.type = Oid::Float4; p.value.resize(4);
    uint32_t u = htonf(v); std::memcpy(p.value.data(), &u, 4); return p;
}
PgParam PgParam::f8(double v) {
    PgParam p; p.type = Oid::Float8; p.value.resize(8);
    uint64_t u = htondll(v); std::memcpy(p.value.data(), &u, 8); return p;
}
PgParam PgParam::b(bool v)    { PgParam p; p.type = Oid::Bool; p.value.assign(1, v ? 1 : 0); return p; }
PgParam PgParam::text(std::string_view v) {
    PgParam p; p.type = Oid::Text; p.value.assign(v.begin(), v.end()); return p;
}
PgParam PgParam::bytea(std::span<const uint8_t> v) {
    PgParam p; p.type = Oid::Bytea;
    p.value.assign(reinterpret_cast<const char*>(v.data()),
                   reinterpret_cast<const char*>(v.data()) + v.size());
    return p;
}

// ---------- PgResult accessors ----------

std::optional<int32_t> PgResult::i4(size_t row, size_t col) const {
    if (row >= rows.size() || col >= rows[row].cells.size()) return std::nullopt;
    if (rows[row].nulls[col]) return std::nullopt;
    const auto& c = rows[row].cells[col];
    if (c.size() < 4) return std::nullopt;
    return wire::get_i32(reinterpret_cast<const uint8_t*>(c.data()));
}
std::optional<int64_t> PgResult::i8(size_t row, size_t col) const {
    if (row >= rows.size() || col >= rows[row].cells.size()) return std::nullopt;
    if (rows[row].nulls[col]) return std::nullopt;
    const auto& c = rows[row].cells[col];
    if (c.size() < 8) return std::nullopt;
    return wire::get_i64(reinterpret_cast<const uint8_t*>(c.data()));
}
std::optional<double> PgResult::f8(size_t row, size_t col) const {
    if (row >= rows.size() || col >= rows[row].cells.size()) return std::nullopt;
    if (rows[row].nulls[col]) return std::nullopt;
    const auto& c = rows[row].cells[col];
    if (c.size() < 8) return std::nullopt;
    uint64_t u; std::memcpy(&u, c.data(), 8); return ntohdll(u);
}
std::optional<bool> PgResult::b(size_t row, size_t col) const {
    if (row >= rows.size() || col >= rows[row].cells.size()) return std::nullopt;
    if (rows[row].nulls[col]) return std::nullopt;
    const auto& c = rows[row].cells[col];
    if (c.empty()) return std::nullopt;
    return c[0] != 0;
}
std::optional<std::string_view> PgResult::text(size_t row, size_t col) const {
    if (row >= rows.size() || col >= rows[row].cells.size()) return std::nullopt;
    if (rows[row].nulls[col]) return std::nullopt;
    return std::string_view(rows[row].cells[col]);
}

// ---------- PgConnection ----------

PgConnection::PgConnection() : ring_(std::make_unique<Ring>()) {}
PgConnection::~PgConnection() { close(); }

PgConnection::PgConnection(PgConnection&& o) noexcept
    : fd_(o.fd_), state_(o.state_), backend_pid_(o.backend_pid_),
      secret_key_(o.secret_key_), epoch_(o.epoch_),
      last_error_(std::move(o.last_error_)),
      server_params_(std::move(o.server_params_)),
      ring_(std::move(o.ring_))
{
    o.fd_ = -1; o.state_ = ConnState::Disconnected;
}
PgConnection& PgConnection::operator=(PgConnection&& o) noexcept {
    if (this != &o) {
        close();
        fd_ = o.fd_; state_ = o.state_; backend_pid_ = o.backend_pid_;
        secret_key_ = o.secret_key_; epoch_ = o.epoch_;
        last_error_ = std::move(o.last_error_);
        server_params_ = std::move(o.server_params_);
        ring_ = std::move(o.ring_);
        o.fd_ = -1; o.state_ = ConnState::Disconnected;
    }
    return *this;
}

int PgConnection::fd() const noexcept { return fd_; }

void PgConnection::close() noexcept {
    if (fd_ >= 0) {
        // Best-effort Terminate. If the socket is wedged, we don't care.
        uint8_t term[5] = { wire::kTerminate, 0, 0, 0, 4 };
        ::send(fd_, term, sizeof term, MSG_NOSIGNAL | MSG_DONTWAIT);
        ::close(fd_);
        fd_ = -1;
    }
    state_ = ConnState::Disconnected;
    server_params_.clear();
    ++epoch_;
}

bool PgConnection::send_all(const void* buf, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    while (len > 0) {
        int n = ring_->blocking_io(fd_, const_cast<uint8_t*>(p), len, /*is_write=*/true);
        if (n <= 0) {
            last_error_ = "send failed: ";
            last_error_ += std::strerror(n == 0 ? EPIPE : -n);
            return false;
        }
        p += n; len -= n;
    }
    return true;
}

bool PgConnection::recv_msg(char& type, std::vector<uint8_t>& payload) {
    // 1-byte type + 4-byte length-including-self.
    uint8_t hdr[5];
    size_t got = 0;
    while (got < 5) {
        int n = ring_->blocking_io(fd_, hdr + got, 5 - got, /*is_write=*/false);
        if (n <= 0) {
            last_error_ = "recv header failed: ";
            last_error_ += std::strerror(n == 0 ? ECONNRESET : -n);
            return false;
        }
        got += static_cast<size_t>(n);
    }
    type = static_cast<char>(hdr[0]);
    int32_t total = wire::get_i32(hdr + 1);
    if (total < 4 || total > 64 * 1024 * 1024) {
        last_error_ = "protocol error: bad message length";
        return false;
    }
    size_t body = static_cast<size_t>(total - 4);
    payload.resize(body);
    size_t pgot = 0;
    while (pgot < body) {
        int n = ring_->blocking_io(fd_, payload.data() + pgot, body - pgot, /*is_write=*/false);
        if (n <= 0) {
            last_error_ = "recv body failed: ";
            last_error_ += std::strerror(n == 0 ? ECONNRESET : -n);
            return false;
        }
        pgot += static_cast<size_t>(n);
    }
    return true;
}

bool PgConnection::connect(std::string_view socket_path,
                           std::string_view user,
                           std::string_view password,
                           std::string_view dbname)
{
    close();

    if (!ring_->init()) { last_error_ = "io_uring init failed"; return false; }

    fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) { last_error_ = "socket() failed"; return false; }

    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(sa.sun_path)) {
        last_error_ = "socket path too long"; close(); return false;
    }
    std::memcpy(sa.sun_path, socket_path.data(), socket_path.size());

    state_ = ConnState::Connecting;
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&sa), sizeof sa) < 0) {
        last_error_ = "connect(\""; last_error_ += socket_path;
        last_error_ += "\"): "; last_error_ += std::strerror(errno);
        close(); return false;
    }

    state_ = ConnState::Authenticating;
    if (!authenticate(user, password, dbname)) { close(); return false; }

    state_ = ConnState::Ready;
    return true;
}

bool PgConnection::authenticate(std::string_view user,
                                std::string_view password,
                                std::string_view dbname)
{
    // StartupMessage. No type byte. Layout:
    //   int32 length, int32 0x00030000, "key\0value\0"..., "\0"
    std::vector<uint8_t> buf;
    size_t len_off = buf.size();
    wire::put_i32(buf, 0);          // placeholder length
    wire::put_i32(buf, 196608);     // protocol version 3.0
    wire::put_str(buf, "user");           wire::put_str(buf, user);
    wire::put_str(buf, "database");       wire::put_str(buf, dbname);
    wire::put_str(buf, "client_encoding");wire::put_str(buf, "UTF8");
    wire::put_str(buf, "application_name");wire::put_str(buf, "spido_pg");
    buf.push_back(0);               // end of parameters
    wire::patch_len(buf, len_off);

    if (!send_all(buf.data(), buf.size())) return false;

    std::vector<uint8_t> payload;
    char type;
    while (true) {
        if (!recv_msg(type, payload)) return false;
        switch (type) {
        case wire::kAuthentication: {
            if (payload.size() < 4) { last_error_ = "short auth msg"; return false; }
            uint32_t sub = static_cast<uint32_t>(wire::get_i32(payload.data()));
            if (sub == wire::kAuthOk) {
                // Proceed to ParameterStatus / BackendKeyData / ReadyForQuery.
                break;
            } else if (sub == wire::kAuthCleartext) {
                std::vector<uint8_t> reply;
                reply.push_back(wire::kPassword);
                size_t lo = reply.size(); wire::put_i32(reply, 0);
                wire::put_str(reply, password);
                wire::patch_len(reply, lo);
                if (!send_all(reply.data(), reply.size())) return false;
            } else if (sub == wire::kAuthMD5) {
                if (payload.size() < 8) { last_error_ = "short md5 salt"; return false; }
                uint8_t salt[4];
                std::memcpy(salt, payload.data() + 4, 4);
                std::string token = md5::pg_password(password, user, salt);
                std::vector<uint8_t> reply;
                reply.push_back(wire::kPassword);
                size_t lo = reply.size(); wire::put_i32(reply, 0);
                wire::put_str(reply, token);
                wire::patch_len(reply, lo);
                if (!send_all(reply.data(), reply.size())) return false;
            } else if (sub == wire::kAuthSASL) {
                // SASL mechanism list: NUL-separated strings, terminated
                // by an empty mechanism. PG14+ always offers
                // SCRAM-SHA-256 first; we ignore SCRAM-SHA-256-PLUS
                // (channel binding) since we're on a Unix socket.
                const char* mlist = reinterpret_cast<const char*>(payload.data() + 4);
                size_t remaining = payload.size() - 4;
                bool has_sha256 = false;
                while (remaining > 0 && *mlist) {
                    size_t l = ::strnlen(mlist, remaining);
                    if (l == 0) break;
                    std::string_view mech(mlist, l);
                    if (mech == "SCRAM-SHA-256") { has_sha256 = true; }
                    mlist     += l + 1;
                    remaining -= l + 1;
                }
                if (!has_sha256) {
                    last_error_ = "server didn't offer SCRAM-SHA-256";
                    return false;
                }
                auto cf = scram::build_client_first();

                // SASLInitialResponse: mech NUL-string + i32(initial-response-len)
                // + initial-response bytes (no NUL terminator).
                std::vector<uint8_t> reply;
                reply.push_back(wire::kPassword);
                size_t lo = reply.size(); wire::put_i32(reply, 0);
                wire::put_str(reply, "SCRAM-SHA-256");
                wire::put_i32(reply, static_cast<int32_t>(cf.full.size()));
                wire::put_bytes(reply, cf.full.data(), cf.full.size());
                wire::patch_len(reply, lo);
                if (!send_all(reply.data(), reply.size())) return false;

                // Stash for the SASLContinue step. Field-copy so the
                // public connection.h doesn't have to know about scram::.
                pending_scram_client_first_.gs2_header   = cf.gs2_header;
                pending_scram_client_first_.bare         = cf.bare;
                pending_scram_client_first_.client_nonce = cf.client_nonce;
                pending_scram_client_first_.full         = cf.full;
            } else if (sub == wire::kAuthSASLContinue) {
                // Server-first message — parse, derive client-final.
                std::string_view server_first(
                    reinterpret_cast<const char*>(payload.data() + 4),
                    payload.size() - 4);
                scram::ServerFirst sf;
                if (!scram::parse_server_first(server_first, sf)) {
                    last_error_ = "scram: bad server-first"; return false;
                }
                // Server nonce must start with the client nonce we sent.
                if (sf.r_nonce.size() < pending_scram_client_first_.client_nonce.size() ||
                    std::string_view(sf.r_nonce).substr(0,
                        pending_scram_client_first_.client_nonce.size())
                    != pending_scram_client_first_.client_nonce) {
                    last_error_ = "scram: server nonce doesn't extend client"; return false;
                }
                // Reconstruct a local scram::ClientFirst from the
                // public-header struct so build_client_final() can use it.
                scram::ClientFirst cf_local;
                cf_local.gs2_header   = pending_scram_client_first_.gs2_header;
                cf_local.bare         = pending_scram_client_first_.bare;
                cf_local.client_nonce = pending_scram_client_first_.client_nonce;
                cf_local.full         = pending_scram_client_first_.full;
                auto art = scram::build_client_final(password, cf_local, sf);
                pending_scram_server_sig_ = std::move(art.expected_server_sig_b64);

                std::vector<uint8_t> reply;
                reply.push_back(wire::kPassword);
                size_t lo = reply.size(); wire::put_i32(reply, 0);
                wire::put_bytes(reply, art.client_final.data(), art.client_final.size());
                wire::patch_len(reply, lo);
                if (!send_all(reply.data(), reply.size())) return false;
            } else if (sub == wire::kAuthSASLFinal) {
                std::string_view server_final(
                    reinterpret_cast<const char*>(payload.data() + 4),
                    payload.size() - 4);
                if (!scram::verify_server_final(server_final,
                                                 pending_scram_server_sig_)) {
                    last_error_ = "scram: server signature mismatch — auth failed";
                    return false;
                }
                // Server signature verified; expect AuthOk next.
            } else {
                last_error_ = "unsupported auth method (sub=" +
                              std::to_string(sub) + ")";
                return false;
            }
            break;
        }
        case wire::kParameterStatus: {
            // "name\0value\0"
            const char* p = reinterpret_cast<const char*>(payload.data());
            size_t k = ::strnlen(p, payload.size());
            if (k >= payload.size()) break;
            const char* v = p + k + 1;
            size_t vl = ::strnlen(v, payload.size() - k - 1);
            server_params_.emplace(std::string(p, k), std::string(v, vl));
            break;
        }
        case wire::kBackendKeyData: {
            if (payload.size() >= 8) {
                backend_pid_ = wire::get_i32(payload.data());
                secret_key_  = wire::get_i32(payload.data() + 4);
            }
            break;
        }
        case wire::kReadyForQuery:
            return true;
        case wire::kErrorResponse: {
            // Walk field-tag/value pairs. 'M' is the human-readable message.
            const char* p = reinterpret_cast<const char*>(payload.data());
            size_t i = 0;
            while (i < payload.size() && p[i] != 0) {
                char tag = p[i++];
                size_t l = ::strnlen(p + i, payload.size() - i);
                std::string_view val(p + i, l);
                if (tag == 'M') { last_error_ = "PG error: "; last_error_ += val; }
                i += l + 1;
            }
            if (last_error_.empty()) last_error_ = "PG error (no message)";
            return false;
        }
        default:
            // NoticeResponse and friends — ignore during startup.
            break;
        }
    }
}

// Walk messages from PG until ReadyForQuery, accumulating RowDescription,
// DataRow, CommandComplete, ErrorResponse into `out`.
bool PgConnection::drain_until_ready(PgResult* out) {
    std::vector<uint8_t> payload;
    char type;
    bool in_error = false;
    while (true) {
        if (!recv_msg(type, payload)) return false;
        switch (type) {
        case wire::kRowDescription: {
            if (!out) break;
            out->fields.clear();
            if (payload.size() < 2) return false;
            int16_t n = wire::get_i16(payload.data());
            size_t p = 2;
            for (int i = 0; i < n; ++i) {
                PgField f;
                size_t name_len = ::strnlen(reinterpret_cast<const char*>(payload.data() + p),
                                            payload.size() - p);
                f.name.assign(reinterpret_cast<const char*>(payload.data() + p), name_len);
                p += name_len + 1;
                if (p + 18 > payload.size()) return false;
                /* table oid (i32) */          p += 4;
                /* column attr   (i16) */      p += 2;
                f.type_oid = static_cast<Oid>(static_cast<uint32_t>(wire::get_i32(payload.data() + p)));
                p += 4;
                f.type_len = wire::get_i16(payload.data() + p); p += 2;
                /* typmod (i32) */              p += 4;
                f.format = wire::get_i16(payload.data() + p);   p += 2;
                out->fields.push_back(std::move(f));
            }
            break;
        }
        case wire::kDataRow: {
            if (!out) break;
            if (payload.size() < 2) return false;
            int16_t n = wire::get_i16(payload.data());
            size_t p = 2;
            PgRow row;
            row.cells.resize(n);
            row.nulls.assign(n, false);
            for (int i = 0; i < n; ++i) {
                if (p + 4 > payload.size()) return false;
                int32_t len = wire::get_i32(payload.data() + p); p += 4;
                if (len < 0) {
                    row.nulls[i] = true;
                } else {
                    if (p + static_cast<size_t>(len) > payload.size()) return false;
                    row.cells[i].assign(reinterpret_cast<const char*>(payload.data() + p),
                                        static_cast<size_t>(len));
                    p += static_cast<size_t>(len);
                }
            }
            out->rows.push_back(std::move(row));
            break;
        }
        case wire::kCommandComplete: {
            if (out) {
                size_t l = ::strnlen(reinterpret_cast<const char*>(payload.data()), payload.size());
                out->tag.assign(reinterpret_cast<const char*>(payload.data()), l);
            }
            break;
        }
        case wire::kEmptyQuery:
        case wire::kPortalSuspended:
        case wire::kParseComplete:
        case wire::kBindComplete:
        case wire::kCloseComplete:
        case wire::kNoData:
        case wire::kParameterDesc:
            break;
        case wire::kParameterStatus: {
            const char* p = reinterpret_cast<const char*>(payload.data());
            size_t k = ::strnlen(p, payload.size());
            if (k < payload.size()) {
                const char* v = p + k + 1;
                size_t vl = ::strnlen(v, payload.size() - k - 1);
                server_params_[std::string(p, k)] = std::string(v, vl);
            }
            break;
        }
        case wire::kNoticeResponse:
            // Async non-fatal notice. Ignore.
            break;
        case wire::kErrorResponse: {
            in_error = true;
            if (!out) break;
            out->had_error = true;
            const char* p = reinterpret_cast<const char*>(payload.data());
            size_t i = 0;
            while (i < payload.size() && p[i] != 0) {
                char tag = p[i++];
                size_t l = ::strnlen(p + i, payload.size() - i);
                std::string val(p + i, l);
                switch (tag) {
                case 'S': out->error_severity = std::move(val); break;
                case 'C': out->error_code     = std::move(val); break;
                case 'M': out->error_message  = std::move(val); break;
                }
                i += l + 1;
            }
            if (out->error_message.empty()) out->error_message = "PG error (no message)";
            last_error_ = out->error_message;
            break;
        }
        case wire::kReadyForQuery: {
            if (!payload.empty() && out) out->tx_status = static_cast<char>(payload[0]);
            // Once we hit ReadyForQuery the protocol round is over.
            return !in_error || (out && out->had_error);
        }
        case wire::kNotification:
            // We're processing a regular query; don't drop unsolicited NOTIFY
            // here either — the LISTEN connection lives separately, but if a
            // session that issued LISTEN gets reused for SELECTs, the notify
            // is the caller's problem to track. For now: ignore quietly.
            break;
        default:
            // Unknown / not-yet-handled frame. Skip; we already consumed body.
            break;
        }
    }
}

PgResult PgConnection::exec(std::string_view sql) {
    PgResult r;
    if (state_ != ConnState::Ready) {
        r.had_error = true;
        r.error_message = "connection not ready";
        return r;
    }
    state_ = ConnState::Busy;

    std::vector<uint8_t> buf;
    buf.push_back(wire::kQuery);
    size_t lo = buf.size(); wire::put_i32(buf, 0);
    wire::put_str(buf, sql);
    wire::patch_len(buf, lo);

    if (!send_all(buf.data(), buf.size())) {
        state_ = ConnState::Error;
        r.had_error = true;
        r.error_message = last_error_;
        return r;
    }
    if (!drain_until_ready(&r)) {
        state_ = ConnState::Error;
        if (!r.had_error) {
            r.had_error = true;
            r.error_message = last_error_.empty() ? "drain failed" : last_error_;
        }
        return r;
    }
    state_ = ConnState::Ready;
    return r;
}

StmtHandle PgConnection::prepare(std::string_view name,
                                 std::string_view sql,
                                 std::span<const Oid> param_types)
{
    StmtHandle h;
    h.name.assign(name);
    h.param_types.assign(param_types.begin(), param_types.end());
    h.epoch = epoch_;

    if (state_ != ConnState::Ready) return h;
    state_ = ConnState::Busy;

    std::vector<uint8_t> buf;
    // Parse
    buf.push_back(wire::kParse);
    size_t lo = buf.size(); wire::put_i32(buf, 0);
    wire::put_str(buf, name);
    wire::put_str(buf, sql);
    wire::put_i16(buf, static_cast<int16_t>(param_types.size()));
    for (auto t : param_types) wire::put_i32(buf, static_cast<int32_t>(t));
    wire::patch_len(buf, lo);
    // Sync
    buf.push_back(wire::kSync);
    wire::put_i32(buf, 4);

    if (!send_all(buf.data(), buf.size())) { state_ = ConnState::Error; return h; }
    PgResult r;
    if (!drain_until_ready(&r)) { state_ = ConnState::Error; return h; }
    state_ = ConnState::Ready;
    if (r.had_error) h.name.clear();
    return h;
}

PgResult PgConnection::exec_prepared(const StmtHandle& s,
                                     std::span<const PgParam> params)
{
    PgResult r;
    if (state_ != ConnState::Ready || s.name.empty()) {
        r.had_error = true; r.error_message = "not ready or unprepared";
        return r;
    }
    state_ = ConnState::Busy;

    std::vector<uint8_t> buf;

    // Bind. Per-param format codes — generator-emitted params come in as
    // text format (PG infers types from the column the prepared statement
    // was Parse'd against), while the binary helpers (PgParam::i4 etc.)
    // emit wire-native binary. Mixing per call is legal.
    buf.push_back(wire::kBind);
    size_t lo = buf.size(); wire::put_i32(buf, 0);
    wire::put_str(buf, "");        // portal (unnamed)
    wire::put_str(buf, s.name);    // statement
    wire::put_i16(buf, static_cast<int16_t>(params.size()));
    for (const auto& p : params) wire::put_i16(buf, p.text_format ? 0 : 1);
    wire::put_i16(buf, static_cast<int16_t>(params.size()));
    for (const auto& p : params) {
        if (p.is_null) { wire::put_i32(buf, -1); continue; }
        wire::put_i32(buf, static_cast<int32_t>(p.value.size()));
        wire::put_bytes(buf, p.value.data(), p.value.size());
    }
    // Always request binary results — our decoders assume binary on the way
    // back regardless of how we shipped the params.
    wire::put_i16(buf, 1);
    wire::put_i16(buf, 1);
    wire::patch_len(buf, lo);

    // Describe portal
    buf.push_back(wire::kDescribe);
    size_t lo2 = buf.size(); wire::put_i32(buf, 0);
    buf.push_back('P');
    wire::put_str(buf, "");
    wire::patch_len(buf, lo2);

    // Execute portal
    buf.push_back(wire::kExecute);
    size_t lo3 = buf.size(); wire::put_i32(buf, 0);
    wire::put_str(buf, "");
    wire::put_i32(buf, 0); // max_rows = no limit
    wire::patch_len(buf, lo3);

    // Sync
    buf.push_back(wire::kSync);
    wire::put_i32(buf, 4);

    if (!send_all(buf.data(), buf.size())) {
        state_ = ConnState::Error;
        r.had_error = true; r.error_message = last_error_;
        return r;
    }
    if (!drain_until_ready(&r)) {
        state_ = ConnState::Error;
        if (!r.had_error) { r.had_error = true; r.error_message = last_error_; }
        return r;
    }
    state_ = ConnState::Ready;
    return r;
}

} // namespace spido_pg
