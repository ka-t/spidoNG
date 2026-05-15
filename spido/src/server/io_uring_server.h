#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <liburing.h>

#ifdef SPIDO_WITH_KTLS
#include "server/tls.h"
#endif

#include "server/access_log.h"
#include "server/http_parser.h"
#ifdef SPIDO_WITH_JWT
#include "server/jwt.h"
#endif
#include "server/router.h"
#include "spido/server.h"

namespace spido {

// One IoUringWorker owns:
//   * a SO_REUSEPORT listener fd on the same (bind, port) as every other
//     worker — the kernel hashes new connections across them, so no cross-
//     thread fd handoff is needed
//   * one io_uring instance (SQPOLL when enabled)
//   * a fd-indexed connection table (flat vector, O(1) lookup)
//   * a free-list of recycled Conn structs so accept doesn't malloc
//
// The eventfd is kept only as a stop signal; routine I/O never touches it.
class IoUringWorker {
public:
    IoUringWorker(unsigned id,
                  const ServerConfig& cfg,
                  const Router& router,
                  AccessLogRing* access_log_ring
#ifdef SPIDO_WITH_JWT
                  , JwtVerifier* jwt
#endif
#ifdef SPIDO_WITH_KTLS
                  , TlsContext* tls_ctx
#endif
                  );
    ~IoUringWorker();

    IoUringWorker(const IoUringWorker&)            = delete;
    IoUringWorker& operator=(const IoUringWorker&) = delete;

    void start(unsigned cpu);
    void stop();
    void join() { if (thread_.joinable()) thread_.join(); }

    unsigned id() const noexcept { return id_; }
    int      listener_fd() const noexcept { return listener_fd_; }

private:
    enum class Op : uint8_t {
        Eventfd, Accept, Recv, Send, Close, CancelNoop, Timer,
        TlsAccept, TlsHandshakePoll,
    };

    struct Conn {
        int           fd = -1;
        std::string   in;
        std::string   out;
        size_t        out_off       = 0;
        bool          recv_inflight = false;
        bool          send_inflight = false;
        bool          want_close    = false;
        bool          fixed_file    = false;
        // Slowloris-style timeout state. `deadline_ns` is a monotonic
        // absolute time (CLOCK_MONOTONIC ns); 0 means no deadline.
        // The kind decides which config knob set it, so periodic_sweep
        // can advance the deadline correctly on phase change (header→
        // body→idle) when a positive event happens.
        enum class Phase : uint8_t { Headers, Body, Idle, Writing };
        Phase    phase        = Phase::Headers;
        uint64_t deadline_ns  = 0;
#ifdef SPIDO_WITH_KTLS
        // TLS state — used only during the OpenSSL handshake. After the
        // handshake completes the socket is in kTLS mode and we treat it
        // as a regular plaintext conn; ssl is freed (BIO_NOCLOSE keeps fd).
        SSL*  ssl              = nullptr;
        bool  tls_pending      = false;  // handshake in progress
        bool  tls_poll_inflight = false;
#endif

        void reset_for_reuse() noexcept {
            fd = -1;
            in.clear();
            out.clear();
            out_off = 0;
            recv_inflight = false;
            send_inflight = false;
            want_close    = false;
            fixed_file    = false;
            phase         = Phase::Headers;
            deadline_ns   = 0;
#ifdef SPIDO_WITH_KTLS
            // ssl is freed elsewhere; reset_for_reuse fires when the Conn
            // returns to the pool, by which time the SSL handle is gone.
            ssl               = nullptr;
            tls_pending       = false;
            tls_poll_inflight = false;
#endif
        }
    };

    void open_listener();
    void init_ring(); // must run on the worker thread for SINGLE_ISSUER
    void run();
    void arm_eventfd_poll();
    void submit_accept();
    void submit_recv(Conn& c);
    void submit_send(Conn& c);
    void close_conn(Conn& c);
    void process_input(Conn& c);
    void handle_accept(int32_t res, uint32_t flags);
    void handle_recv(Conn& c, int32_t res, uint32_t flags);
    void handle_send(Conn& c, int32_t res);
    void abort_all();
    void arm_periodic_timer();
    void sweep_deadlines();
    void set_deadline(Conn& c, Conn::Phase ph) noexcept;
    static uint64_t now_ns() noexcept;

#ifdef SPIDO_WITH_KTLS
    void open_tls_listener();
    void submit_tls_accept();
    void handle_tls_accept(int32_t res, uint32_t flags);
    void submit_tls_handshake_poll(Conn& c, int poll_events);
    void drive_tls_handshake(Conn& c);
    void on_tls_handshake_done(Conn& c);
#endif

#ifdef SPIDO_HAVE_BUFRING
    void setup_buf_ring();
    void teardown_buf_ring();
    void return_buffer(uint16_t bid);
    char* buffer_addr(uint16_t bid) noexcept {
        return static_cast<char*>(buf_mem_) + size_t(bid) * buf_size_;
    }
#endif

    Conn* get_conn(int fd) noexcept {
        if (fd < 0 || size_t(fd) >= conns_by_fd_.size()) return nullptr;
        return conns_by_fd_[fd];
    }
    Conn* attach_conn(int fd);
    void  release_conn(int fd);

    static uintptr_t make_ud(Op op, int fd) noexcept {
        return (uintptr_t(uint8_t(op)) << 56) | uintptr_t(uint32_t(fd));
    }
    static Op  ud_op(uintptr_t ud) noexcept { return Op(uint8_t(ud >> 56)); }
    static int ud_fd(uintptr_t ud) noexcept { return int(uint32_t(ud)); }

    unsigned                       id_;
    const ServerConfig&            cfg_;
    const Router&                  router_;
    AccessLogRing*                 access_log_ring_ = nullptr;
#ifdef SPIDO_WITH_JWT
    JwtVerifier*                   jwt_             = nullptr;
#endif

    io_uring                       ring_{};
    bool                           ring_inited_   = false;
    bool                           files_registered_ = false;
    int                            listener_fd_   = -1;
    int                            eventfd_       = -1;
    bool                           accept_inflight_ = false;
    // Periodic 1-sec timer drives deadline sweep + access-log flush.
    struct __kernel_timespec       timer_ts_{1, 0};
    bool                           timer_inflight_ = false;

#ifdef SPIDO_WITH_KTLS
    TlsContext*                    tls_ctx_           = nullptr;
    int                            tls_listener_fd_   = -1;
    bool                           tls_accept_inflight_ = false;
#endif

#ifdef SPIDO_HAVE_BUFRING
    // Provided-buffer ring for IORING_RECV_MULTISHOT. Kernel picks a free
    // buffer per packet; we return it after copying into the per-Conn
    // accumulator. Group id is fixed (kBufGroup).
    io_uring_buf_ring*             buf_ring_ = nullptr;
    void*                          buf_mem_  = nullptr;
    size_t                         buf_size_ = 0;
    size_t                         buf_count_ = 0;
    int                            buf_ring_mask_ = 0;
    static constexpr int           kBufGroup = 1;
#endif

    // Indexed by fd. Resized on demand. nullptr = no connection.
    std::vector<Conn*>             conns_by_fd_;
    size_t                         live_conns_    = 0;

    // Owning storage for Conn objects; we move pointers in/out of free_list_.
    std::vector<std::unique_ptr<Conn>> conn_storage_;
    std::vector<Conn*>             free_list_;

    std::atomic<bool>              stop_{false};
    std::thread                    thread_;
    HttpParser                     parser_{};

    // Per-worker Date header cache. Refreshed at most once per second on
    // the hot path via a coarse time check; strftime() never runs per-req.
    // Format: "Date: Tue, 12 May 2026 13:42:00 GMT\r\n"
    char     date_hdr_[48]     = {};
    size_t   date_hdr_len_     = 0;
    time_t   date_hdr_sec_     = 0;
    void     refresh_date_header() noexcept;
};

} // namespace spido
