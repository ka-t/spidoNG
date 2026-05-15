#include "server/io_uring_server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include <ctime>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>

#include "spido/request.h"
#include "spido/response.h"
#include "utils/cpu_affinity.h"

#ifdef SPIDO_WITH_KTLS
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

namespace spido {

namespace {

constexpr size_t kRecvChunk = 16 * 1024;
constexpr size_t kInReserve = 8 * 1024;

void write_400(std::string& out) {
    static constexpr char k[] =
        "HTTP/1.1 400 Bad Request\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n";
    out.append(k, sizeof(k) - 1);
}

void write_413(std::string& out) {
    static constexpr char k[] =
        "HTTP/1.1 413 Payload Too Large\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n";
    out.append(k, sizeof(k) - 1);
}

void write_404(std::string& out) {
    static constexpr char k[] =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Length: 9\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: keep-alive\r\n\r\nNot Found";
    out.append(k, sizeof(k) - 1);
}

} // namespace

IoUringWorker::IoUringWorker(unsigned id,
                             const ServerConfig& cfg,
                             const Router& router,
                             AccessLogRing* access_log_ring
#ifdef SPIDO_WITH_JWT
                             , JwtVerifier* jwt
#endif
#ifdef SPIDO_WITH_KTLS
                             , TlsContext* tls_ctx
#endif
                             )
    : id_(id), cfg_(cfg), router_(router),
      access_log_ring_(access_log_ring)
#ifdef SPIDO_WITH_JWT
      , jwt_(jwt)
#endif
#ifdef SPIDO_WITH_KTLS
      , tls_ctx_(tls_ctx)
#endif
{
    eventfd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (eventfd_ < 0) { std::perror("spido: eventfd"); std::abort(); }

    // Pre-grow the fd table so the first 4k connections don't need to
    // realloc. Real fd values can be higher than this — grow on demand.
    conns_by_fd_.resize(4096, nullptr);

    // Pre-allocate a chunk of Conn objects so the first few thousand
    // accepts pull from the free-list rather than `new`.
    constexpr size_t kPrealloc = 1024;
    conn_storage_.reserve(kPrealloc);
    free_list_.reserve(kPrealloc);
    for (size_t i = 0; i < kPrealloc; ++i) {
        auto c = std::make_unique<Conn>();
        c->in.reserve(kInReserve);
        free_list_.push_back(c.get());
        conn_storage_.push_back(std::move(c));
    }
}

IoUringWorker::~IoUringWorker() {
#ifdef SPIDO_HAVE_BUFRING
    teardown_buf_ring();
#endif
    if (eventfd_ >= 0)    ::close(eventfd_);
    if (listener_fd_ >= 0) ::close(listener_fd_);
#ifdef SPIDO_WITH_KTLS
    if (tls_listener_fd_ >= 0) ::close(tls_listener_fd_);
#endif
    if (ring_inited_) io_uring_queue_exit(&ring_);
}

void IoUringWorker::init_ring() {
    io_uring_params params{};
#ifdef IORING_SETUP_DEFER_TASKRUN
    if (cfg_.defer_taskrun) {
        // SINGLE_ISSUER requires the ring be created and used by the same
        // task — that's why init runs on the worker thread, not the ctor.
        params.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
    } else
#endif
    if (cfg_.sqpoll) {
        params.flags          = IORING_SETUP_SQPOLL;
        params.sq_thread_idle = cfg_.sqpoll_idle_ms;
    }

    int rc = io_uring_queue_init_params(cfg_.sq_entries, &ring_, &params);
    if (rc < 0 && params.flags != 0) {
        params = {};
        rc = io_uring_queue_init_params(cfg_.sq_entries, &ring_, &params);
    }
    if (rc < 0) {
        std::fprintf(stderr,
                     "spido: io_uring_queue_init failed on worker %u: %s\n",
                     id_, std::strerror(-rc));
        std::abort();
    }
    ring_inited_ = true;

    // Register a sparse file table so accepted sockets land in pre-reserved
    // slots — every subsequent recv/send/close skips the kernel-side fdget
    // refcount. Slots are kernel-allocated via IORING_FILE_INDEX_ALLOC by
    // multishot_accept_direct; we just track which slot belongs to which
    // Conn (conns_by_fd_ is reused as conns_by_slot_).
    constexpr unsigned kMaxFiles = 16384;
    int rc2 = ::io_uring_register_files_sparse(&ring_, kMaxFiles);
    if (rc2 == 0) {
        files_registered_ = true;
    } else {
        std::fprintf(stderr,
                     "spido: io_uring_register_files_sparse failed (%s) — "
                     "falling back to non-direct fds\n", std::strerror(-rc2));
    }

#ifdef SPIDO_HAVE_BUFRING
    setup_buf_ring();
#endif
}

#ifdef SPIDO_HAVE_BUFRING

void IoUringWorker::setup_buf_ring() {
    // 4096 buffers × 2 KiB each = 8 MiB per worker. Plenty for HTTP
    // request packets — accumulator catches the rare oversize case.
    buf_count_ = 4096;
    buf_size_  = 2048;

    int err = 0;
    buf_ring_ = ::io_uring_setup_buf_ring(&ring_, buf_count_, kBufGroup, 0, &err);
    if (!buf_ring_) {
        std::fprintf(stderr,
                     "spido: io_uring_setup_buf_ring failed on worker %u: %s\n",
                     id_, std::strerror(-err));
        std::abort();
    }

    buf_mem_ = ::mmap(nullptr, buf_count_ * buf_size_,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf_mem_ == MAP_FAILED) {
        std::perror("spido: mmap buf_ring"); std::abort();
    }

    buf_ring_mask_ = ::io_uring_buf_ring_mask(buf_count_);
    // Seed every buffer into the ring.
    for (size_t i = 0; i < buf_count_; ++i) {
        ::io_uring_buf_ring_add(buf_ring_,
                                buffer_addr(uint16_t(i)),
                                buf_size_,
                                uint16_t(i),
                                buf_ring_mask_,
                                int(i));
    }
    ::io_uring_buf_ring_advance(buf_ring_, int(buf_count_));
}

void IoUringWorker::teardown_buf_ring() {
    if (buf_ring_) {
        ::io_uring_free_buf_ring(&ring_, buf_ring_, buf_count_, kBufGroup);
        buf_ring_ = nullptr;
    }
    if (buf_mem_) {
        ::munmap(buf_mem_, buf_count_ * buf_size_);
        buf_mem_ = nullptr;
    }
}

void IoUringWorker::return_buffer(uint16_t bid) {
    ::io_uring_buf_ring_add(buf_ring_,
                            buffer_addr(bid), buf_size_, bid,
                            buf_ring_mask_, 0);
    ::io_uring_buf_ring_advance(buf_ring_, 1);
}

#endif // SPIDO_HAVE_BUFRING

void IoUringWorker::open_listener() {
    // port == 0 → no plaintext listener (HTTPS-only or nginx-fronted setup).
    if (cfg_.port == 0) return;
    listener_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listener_fd_ < 0) {
        std::perror("spido: socket"); std::abort();
    }
    int one = 1;
    ::setsockopt(listener_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    ::setsockopt(listener_fd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    ::setsockopt(listener_fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    if (cfg_.tcp_fastopen > 0) {
        int qlen = cfg_.tcp_fastopen;
        // Best effort — sysctl net.ipv4.tcp_fastopen must allow server side.
        ::setsockopt(listener_fd_, IPPROTO_TCP, TCP_FASTOPEN,
                     &qlen, sizeof(qlen));
    }
    if (cfg_.so_busy_poll_us > 0) {
        int us = int(cfg_.so_busy_poll_us);
        ::setsockopt(listener_fd_, SOL_SOCKET, SO_BUSY_POLL, &us, sizeof(us));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(cfg_.port);
    if (::inet_pton(AF_INET, cfg_.bind.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "spido: invalid bind addr\n"); std::abort();
    }
    if (::bind(listener_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("spido: bind"); std::abort();
    }
    if (::listen(listener_fd_, cfg_.backlog) < 0) {
        std::perror("spido: listen"); std::abort();
    }
}

void IoUringWorker::start(unsigned cpu) {
    thread_ = std::thread([this, cpu] {
        if (!util::pin_thread(::pthread_self(), cpu)) {
            std::fprintf(stderr, "spido: failed to pin worker %u to CPU %u\n",
                         id_, cpu);
        }
        run();
    });
}

void IoUringWorker::stop() {
    stop_.store(true, std::memory_order_release);
    uint64_t one = 1;
    ssize_t r = ::write(eventfd_, &one, sizeof(one));
    (void)r;
}

void IoUringWorker::arm_eventfd_poll() {
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    io_uring_prep_poll_add(sqe, eventfd_, POLLIN);
    io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(
                                   make_ud(Op::Eventfd, eventfd_)));
}

void IoUringWorker::submit_accept() {
    if (accept_inflight_ || stop_.load(std::memory_order_acquire)) return;
    if (listener_fd_ < 0) return; // plaintext listener disabled
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
#ifdef SPIDO_HAVE_MULTISHOT
    if (files_registered_) {
        // Direct accept: kernel auto-allocates a registered-file slot and
        // returns it in cqe->res. SOCK_CLOEXEC isn't meaningful for direct
        // (no userspace fd) and some kernels reject the flag; pass 0.
        io_uring_prep_multishot_accept_direct(sqe, listener_fd_,
                                              nullptr, nullptr, 0);
    } else {
        io_uring_prep_multishot_accept(sqe, listener_fd_, nullptr, nullptr,
                                       SOCK_CLOEXEC | SOCK_NONBLOCK);
    }
#else
    io_uring_prep_accept(sqe, listener_fd_, nullptr, nullptr,
                         SOCK_CLOEXEC | SOCK_NONBLOCK);
#endif
    io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(
                                   make_ud(Op::Accept, listener_fd_)));
    accept_inflight_ = true;
}

#ifdef SPIDO_WITH_KTLS

void IoUringWorker::open_tls_listener() {
    if (cfg_.tls_port == 0 || !tls_ctx_) return;

    tls_listener_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (tls_listener_fd_ < 0) {
        std::perror("spido/tls: socket — TLS disabled on this worker");
        return;
    }
    int one = 1;
    ::setsockopt(tls_listener_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    ::setsockopt(tls_listener_fd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    ::setsockopt(tls_listener_fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(cfg_.tls_port);
    ::inet_pton(AF_INET, cfg_.bind.c_str(), &addr.sin_addr);
    auto fail = [&](const char* what) {
        std::fprintf(stderr,
            "spido/tls: %s on worker %u — TLS disabled here: %s\n",
            what, id_, std::strerror(errno));
        ::close(tls_listener_fd_);
        tls_listener_fd_ = -1;
    };
    if (::bind(tls_listener_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        fail("bind"); return;
    }
    if (::listen(tls_listener_fd_, cfg_.backlog) < 0) {
        fail("listen"); return;
    }
}

void IoUringWorker::submit_tls_accept() {
    if (tls_listener_fd_ < 0) return; // TLS not active on this worker
    if (tls_accept_inflight_) return;
    if (stop_.load(std::memory_order_acquire)) return;
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    // Regular (non-direct) accept — we need a userspace fd because the
    // OpenSSL handshake (SSL_set_fd + send/recv) and the post-handshake
    // setsockopt(SOL_TLS, ...) both want a real fd.
    io_uring_prep_multishot_accept(sqe, tls_listener_fd_, nullptr, nullptr,
                                   SOCK_CLOEXEC | SOCK_NONBLOCK);
    io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(
                                   make_ud(Op::TlsAccept, tls_listener_fd_)));
    tls_accept_inflight_ = true;
}

void IoUringWorker::handle_tls_accept(int32_t res, uint32_t flags) {
    if (res < 0) {
        if (res != -ECANCELED)
            std::fprintf(stderr, "spido/tls: accept: %s\n", std::strerror(-res));
        if (!(flags & IORING_CQE_F_MORE)) {
            tls_accept_inflight_ = false;
            submit_tls_accept();
        }
        return;
    }
    int fd = res;
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    Conn* c = attach_conn(fd);
    if (!c) { ::close(fd); return; }
    c->ssl = tls_ctx_->new_session(fd);
    if (!c->ssl) { ::close(fd); release_conn(fd); return; }
    c->tls_pending = true;
    set_deadline(*c, Conn::Phase::Headers); // TLS conn also under slowloris
    drive_tls_handshake(*c);

    if (!(flags & IORING_CQE_F_MORE)) {
        tls_accept_inflight_ = false;
        submit_tls_accept();
    }
}

void IoUringWorker::submit_tls_handshake_poll(Conn& c, int poll_events) {
    if (c.fd < 0 || c.tls_poll_inflight) return;
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    io_uring_prep_poll_add(sqe, c.fd, poll_events);
    io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(
                                   make_ud(Op::TlsHandshakePoll, c.fd)));
    c.tls_poll_inflight = true;
}

void IoUringWorker::drive_tls_handshake(Conn& c) {
    if (!c.ssl) return;
    int rc = ::SSL_do_handshake(c.ssl);
    if (rc == 1) {
        on_tls_handshake_done(c);
        return;
    }
    int err = ::SSL_get_error(c.ssl, rc);
    if (err == SSL_ERROR_WANT_READ) {
        submit_tls_handshake_poll(c, POLLIN);
    } else if (err == SSL_ERROR_WANT_WRITE) {
        submit_tls_handshake_poll(c, POLLOUT);
    } else {
        // Hard handshake error — log, close.
        unsigned long e;
        while ((e = ERR_get_error()) != 0) {
            char buf[256];
            ERR_error_string_n(e, buf, sizeof(buf));
            std::fprintf(stderr, "spido/tls: handshake: %s\n", buf);
        }
        c.want_close = true;
        ::SSL_free(c.ssl); c.ssl = nullptr;
        c.tls_pending = false;
        close_conn(c);
    }
}

void IoUringWorker::on_tls_handshake_done(Conn& c) {
    // OpenSSL has (with SSL_OP_ENABLE_KTLS) already installed the kernel
    // TLS keys via setsockopt(SOL_TLS, TLS_TX/TLS_RX). From here on
    // io_uring recv/send on the raw fd transports plaintext. Free SSL
    // but keep the fd open (the socket BIO has BIO_NOCLOSE since SSL
    // never owned the fd in close-on-free terms — SSL_set_fd alone
    // doesn't transfer ownership).
    c.tls_pending = false;
    ::SSL_free(c.ssl); c.ssl = nullptr;
    submit_recv(c);
}

#endif // SPIDO_WITH_KTLS

uint64_t IoUringWorker::now_ns() noexcept {
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1'000'000'000ULL + uint64_t(ts.tv_nsec);
}

void IoUringWorker::set_deadline(Conn& c, Conn::Phase ph) noexcept {
    c.phase = ph;
    unsigned secs = 0;
    switch (ph) {
        case Conn::Phase::Headers: secs = cfg_.header_timeout_s; break;
        case Conn::Phase::Body:    secs = cfg_.body_timeout_s;   break;
        case Conn::Phase::Idle:    secs = cfg_.idle_timeout_s;   break;
        case Conn::Phase::Writing: secs = cfg_.write_timeout_s;  break;
    }
    c.deadline_ns = secs ? now_ns() + uint64_t(secs) * 1'000'000'000ULL : 0;
}

void IoUringWorker::arm_periodic_timer() {
    if (timer_inflight_) return;
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    io_uring_prep_timeout(sqe, &timer_ts_, 0, 0);
    io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(make_ud(Op::Timer, 0)));
    timer_inflight_ = true;
}

void IoUringWorker::sweep_deadlines() {
    const uint64_t t = now_ns();
    for (Conn* c : conns_by_fd_) {
        if (!c || c->fd < 0)            continue;
        if (c->deadline_ns == 0)         continue;
        if (c->deadline_ns > t)          continue;
        // Past deadline — close.
        c->want_close = true;
        close_conn(*c);
    }
}

void IoUringWorker::run() {
    init_ring();
    open_listener();
#ifdef SPIDO_WITH_KTLS
    open_tls_listener();
#endif
    arm_eventfd_poll();
    submit_accept();
#ifdef SPIDO_WITH_KTLS
    submit_tls_accept();
#endif
    arm_periodic_timer();
    io_uring_submit(&ring_);

    while (!stop_.load(std::memory_order_acquire) || live_conns_ > 0) {
        io_uring_cqe* cqe = nullptr;
        int r = io_uring_wait_cqe(&ring_, &cqe);
        if (r == -EINTR) continue;
        if (r < 0) break;

        unsigned head = 0;
        unsigned cnt  = 0;
        io_uring_for_each_cqe(&ring_, head, cqe) {
            ++cnt;
            auto ud  = static_cast<uintptr_t>(cqe->user_data);
            int32_t  res = cqe->res;
            Op  op = ud_op(ud);
            int fd = ud_fd(ud);

            if (op == Op::Eventfd) {
                uint64_t v = 0;
                ssize_t r2 = ::read(eventfd_, &v, sizeof(v));
                (void)r2;
                if (stop_.load(std::memory_order_acquire)) abort_all();
                arm_eventfd_poll();
                continue;
            }
            if (op == Op::Timer) {
                timer_inflight_ = false;
                if (!stop_.load(std::memory_order_acquire)) {
                    sweep_deadlines();
                    arm_periodic_timer();
                }
                continue;
            }

            if (op == Op::Accept) {
                handle_accept(res, cqe->flags);
                if (!(cqe->flags & IORING_CQE_F_MORE)) {
                    accept_inflight_ = false;
                    submit_accept();
                }
                continue;
            }
#ifdef SPIDO_WITH_KTLS
            if (op == Op::TlsAccept) {
                handle_tls_accept(res, cqe->flags);
                continue;
            }
            if (op == Op::TlsHandshakePoll) {
                Conn* c = get_conn(fd);
                if (!c) continue;
                c->tls_poll_inflight = false;
                if (res < 0) {
                    c->want_close = true;
                    if (c->ssl) { ::SSL_free(c->ssl); c->ssl = nullptr; }
                    close_conn(*c);
                    continue;
                }
                drive_tls_handshake(*c);
                continue;
            }
#endif

            if (op == Op::Close) {
                release_conn(fd);
                continue;
            }
            if (op == Op::CancelNoop) {
                // Cancel completion — already accounted for by the close
                // path that submitted it. Nothing to do.
                continue;
            }

            Conn* c = get_conn(fd);
            if (!c) continue; // op landed after we already erased

            if (op == Op::Recv) {
                // For multishot recv, leave recv_inflight=true as long as
                // IORING_CQE_F_MORE is set; handle_recv flips it on end.
                handle_recv(*c, res, cqe->flags);
            } else if (op == Op::Send) {
                c->send_inflight = false;
                handle_send(*c, res);
            }
        }
        if (cnt) io_uring_cq_advance(&ring_, cnt);
        io_uring_submit(&ring_);
    }
}

IoUringWorker::Conn* IoUringWorker::attach_conn(int fd) {
    if (fd < 0) return nullptr;
    if (size_t(fd) >= conns_by_fd_.size()) {
        conns_by_fd_.resize(size_t(fd) * 2 + 16, nullptr);
    }
    Conn* c;
    if (!free_list_.empty()) {
        c = free_list_.back();
        free_list_.pop_back();
        c->reset_for_reuse();
    } else {
        auto up = std::make_unique<Conn>();
        up->in.reserve(kInReserve);
        c = up.get();
        conn_storage_.push_back(std::move(up));
    }
    c->fd = fd;
    conns_by_fd_[fd] = c;
    ++live_conns_;
    return c;
}

void IoUringWorker::release_conn(int fd) {
    if (fd < 0 || size_t(fd) >= conns_by_fd_.size()) return;
    Conn* c = conns_by_fd_[fd];
    if (!c) return;
#ifdef SPIDO_WITH_KTLS
    if (c->ssl) { ::SSL_free(c->ssl); c->ssl = nullptr; }
#endif
    conns_by_fd_[fd] = nullptr;
    free_list_.push_back(c);
    --live_conns_;
}

void IoUringWorker::handle_accept(int32_t res, uint32_t /*flags*/) {
    if (res < 0) {
        if (res == -ECANCELED) return;
        std::fprintf(stderr, "spido: accept w=%u: %s\n", id_, std::strerror(-res));
        return;
    }
    int sock_id = res;   // file slot when files_registered_, real fd otherwise
    if (!files_registered_) {
        int one = 1;
        ::setsockopt(sock_id, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (cfg_.tcp_quickack) {
            ::setsockopt(sock_id, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
        }
        if (cfg_.so_busy_poll_us > 0) {
            int us = int(cfg_.so_busy_poll_us);
            ::setsockopt(sock_id, SOL_SOCKET, SO_BUSY_POLL, &us, sizeof(us));
        }
    }

    Conn* c = attach_conn(sock_id);
    if (!c) {
        if (files_registered_) {
            io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            io_uring_prep_close_direct(sqe, unsigned(sock_id));
            io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(
                                          make_ud(Op::Close, sock_id)));
        } else {
            ::close(sock_id);
        }
        return;
    }
    c->fixed_file = files_registered_; // HTTP path uses direct-accept slots
    set_deadline(*c, Conn::Phase::Headers);
    submit_recv(*c);
}

void IoUringWorker::submit_recv(Conn& c) {
    if (c.recv_inflight || c.want_close || c.fd < 0) return;
    if (c.in.size() >= cfg_.max_request_bytes) {
        write_413(c.out);
        c.want_close = true;
        submit_send(c);
        return;
    }

#ifdef SPIDO_HAVE_BUFRING
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    io_uring_prep_recv_multishot(sqe, c.fd, nullptr, 0, 0);
    sqe->flags     |= IOSQE_BUFFER_SELECT;
    sqe->buf_group  = kBufGroup;
    if (c.fixed_file) sqe->flags |= IOSQE_FIXED_FILE;
    io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(make_ud(Op::Recv, c.fd)));
    c.recv_inflight = true;
#else
    size_t old = c.in.size();
    c.in.resize(old + kRecvChunk);
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    io_uring_prep_recv(sqe, c.fd, c.in.data() + old, kRecvChunk, 0);
    if (c.fixed_file) sqe->flags |= IOSQE_FIXED_FILE;
    io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(make_ud(Op::Recv, c.fd)));
    c.recv_inflight = true;
#endif
}

void IoUringWorker::submit_send(Conn& c) {
    if (c.send_inflight || c.fd < 0) return;
    if (c.out_off >= c.out.size()) return;
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    io_uring_prep_send(sqe, c.fd,
                       c.out.data() + c.out_off,
                       c.out.size() - c.out_off,
                       MSG_NOSIGNAL);
    if (c.fixed_file) sqe->flags |= IOSQE_FIXED_FILE;
    io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(make_ud(Op::Send, c.fd)));
    c.send_inflight = true;
}

void IoUringWorker::handle_recv(Conn& c, int32_t res, uint32_t flags) {
#ifdef SPIDO_HAVE_BUFRING
    // Multishot recv path. The kernel reports the chosen buffer id in the
    // top bits of cqe->flags whenever it actually consumed one.
    const bool has_buf = (flags & IORING_CQE_F_BUFFER) != 0;
    const bool more    = (flags & IORING_CQE_F_MORE) != 0;

    if (!more) c.recv_inflight = false;

    if (res <= 0 || !has_buf) {
        // Peer closed, error, or no buffer was available (-ENOBUFS).
        // Re-arm on -ENOBUFS so we don't drop the connection silently.
        c.want_close = (res <= 0);
        if (!c.want_close && !c.recv_inflight) submit_recv(c);
        if (c.want_close) close_conn(c);
        return;
    }

    uint16_t bid = uint16_t(flags >> IORING_CQE_BUFFER_SHIFT);
    const char* src = buffer_addr(bid);
    size_t got = size_t(res);
    c.in.append(src, got);
    return_buffer(bid);

    // First bytes after an Idle gap → kick header timeout (slowloris-safe:
    // only transitions back to Headers, doesn't extend existing Headers
    // deadline — so dripping bytes during headers can't keep us open).
    if (c.phase == Conn::Phase::Idle) set_deadline(c, Conn::Phase::Headers);

    process_input(c);
    if (c.out_off < c.out.size()) submit_send(c);
    if (!c.recv_inflight && !c.want_close) submit_recv(c);
    close_conn(c);
#else
    c.recv_inflight = false;
    (void)flags;
    if (res <= 0) {
        if (c.in.size() >= kRecvChunk) c.in.resize(c.in.size() - kRecvChunk);
        c.want_close = true;
        close_conn(c);
        return;
    }
    size_t got = size_t(res);
    c.in.resize(c.in.size() - kRecvChunk + got);
    if (c.phase == Conn::Phase::Idle) set_deadline(c, Conn::Phase::Headers);
    process_input(c);
    if (c.out_off < c.out.size()) submit_send(c);
    submit_recv(c);
    close_conn(c);
#endif
}

void IoUringWorker::handle_send(Conn& c, int32_t res) {
    if (res <= 0) {
        c.want_close = true;
        close_conn(c);
        return;
    }
    c.out_off += size_t(res);
    if (c.out_off >= c.out.size()) {
        c.out.clear();
        c.out_off = 0;
        if (!c.want_close) {
            // Response fully drained and keep-alive — switch to idle deadline.
            set_deadline(c, Conn::Phase::Idle);
        }
    } else {
        submit_send(c);
    }
    close_conn(c);
}

void IoUringWorker::refresh_date_header() noexcept {
    // Coarse: only update when the wall-clock second has rolled over.
    time_t now = ::time(nullptr);
    if (now == date_hdr_sec_ && date_hdr_len_ > 0) return;
    date_hdr_sec_ = now;
    struct tm tm{};
    ::gmtime_r(&now, &tm);
    // RFC 7231 IMF-fixdate: "Sun, 06 Nov 1994 08:49:37 GMT"
    int n = std::strftime(date_hdr_, sizeof(date_hdr_),
                          "Date: %a, %d %b %Y %H:%M:%S GMT\r\n", &tm);
    date_hdr_len_ = (n > 0) ? size_t(n) : 0;
}

void IoUringWorker::process_input(Conn& c) {
    if (cfg_.emit_date_header) refresh_date_header();
    for (;;) {
        Request req;
        auto status = parser_.parse(c.in.data(), c.in.size(),
                                    cfg_.max_request_bytes, req);
        if (status == HttpParser::Status::NeedMore) return;
        if (status == HttpParser::Status::BadRequest) {
            write_400(c.out);
            c.want_close = true;
            return;
        }
        if (status == HttpParser::Status::TooLarge) {
            write_413(c.out);
            c.want_close = true;
            return;
        }

        const RouteEntry* entry = router_.find(req.method, req.path, &req.params);
        if (!entry) entry = router_.not_found();

        // Request fully parsed — we'll be queuing the response next.
        set_deadline(c, Conn::Phase::Writing);

        size_t response_offset = c.out.size();

#ifdef SPIDO_WITH_JWT
        // JWT middleware. Zero-overhead when disabled or when the route
        // didn't opt in: one bool check before any header lookup.
        if (jwt_ && entry && entry->require_auth) {
            JwtCacheEntry je;
            auto authz = req.header("Authorization");
            auto token = extract_bearer(authz);
            JwtStatus st = token.empty()
                ? JwtStatus::Missing
                : jwt_->verify(token, je);
            if (st != JwtStatus::Valid) {
                const char* reason = jwt_status_str(st);
                std::string body;
                body.reserve(48);
                body.append("{\"error\":\"unauthorized\",\"reason\":\"");
                body.append(reason);
                body.append("\"}");
                char hdrs[160];
                int hn = std::snprintf(hdrs, sizeof(hdrs),
                    "HTTP/1.1 401 Unauthorized\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: %zu\r\n"
                    "Connection: %s\r\n\r\n",
                    body.size(),
                    req.keep_alive ? "keep-alive" : "close");
                if (hn > 0) {
                    c.out.append(hdrs, size_t(hn));
                    c.out.append(body);
                }
                if (!req.keep_alive) c.want_close = true;
                size_t used = parser_.consumed();
                if (used >= c.in.size()) c.in.clear();
                else                     c.in.erase(0, used);
                if (!req.keep_alive) return;
                continue;
            }
        }
#endif // SPIDO_WITH_JWT

        if (entry && entry->cached) {
            // Fast path: append the precomputed bytes. No parser→handler→
            // serialize chain. Date header (if enabled) is injected between
            // the status line and the rest — cached body already includes
            // Content-Type/Length so we just slot Date in.
            if (cfg_.emit_date_header && date_hdr_len_) {
                // The precomputed buffer always starts with "HTTP/1.1 200 OK\r\n".
                constexpr size_t kStatusLen = 17;
                c.out.append(entry->cached->data(), kStatusLen);
                c.out.append(date_hdr_, date_hdr_len_);
                c.out.append(entry->cached->data() + kStatusLen,
                             entry->cached->size() - kStatusLen);
            } else {
                c.out.append(*entry->cached);
            }
        } else if (entry && entry->handler) {
            Response res;
            res.keep_alive = req.keep_alive;
            entry->handler(req, res);
            if (!req.keep_alive) res.keep_alive = false;
            if (cfg_.emit_date_header && date_hdr_len_) {
                // Inject Date header by recording it explicitly.
                res.header("Date", std::string(date_hdr_ + 6,
                                               date_hdr_len_ - 8));
            }
            res.serialize(c.out);
        } else {
            write_404(c.out);
        }

        size_t used = parser_.consumed();
        if (used >= c.in.size()) c.in.clear();
        else                     c.in.erase(0, used);

        // Access log emit. Best-effort: drop the line if the ring is full
        // and config asked for it. Format: combined-ish without IP since
        // we don't track peer addrs yet.
        if (access_log_ring_) {
            char* slot = access_log_ring_->reserve();
            if (slot) {
                // Status code: pull from first line of the just-written
                // response bytes ("HTTP/1.1 NNN ...").
                int status = 0;
                const char* rb = c.out.data() + response_offset;
                size_t rb_len = c.out.size() - response_offset;
                if (rb_len > 12) {
                    status = (rb[9]-'0')*100 + (rb[10]-'0')*10 + (rb[11]-'0');
                }
                int n = std::snprintf(slot, AccessLogRing::kLineMax,
                    "%.*s %.*s %.*s %d\n",
                    int(std::min<size_t>(req.path.size(), 96)), req.path.data(),
                    int(std::min<size_t>(req.version.size(), 8)),  req.version.data(),
                    int(std::min<size_t>(req.header("User-Agent").size(), 64)),
                    req.header("User-Agent").data(),
                    status);
                (void)n;
                access_log_ring_->commit();
            }
        }

        if (!req.keep_alive) {
            c.want_close = true;
            return;
        }
    }
}

void IoUringWorker::close_conn(Conn& c) {
    if (!c.want_close) return;
    if (c.fd < 0)        return;

    const bool aborting = stop_.load(std::memory_order_acquire);

    if (!aborting) {
        if (c.recv_inflight || c.send_inflight) {
            // Only sockets we hold as real fds (non-fixed) can ::shutdown.
            // Fixed-file slots fall through to the cancel-and-close path.
            if (!c.fixed_file) { ::shutdown(c.fd, SHUT_RDWR); return; }
        } else {
            if (c.out_off < c.out.size()) return;
            int handle = c.fd;
            c.fd = -1;
            io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            if (c.fixed_file) {
                io_uring_prep_close_direct(sqe, unsigned(handle));
            } else {
                io_uring_prep_close(sqe, handle);
            }
            io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(
                                           make_ud(Op::Close, handle)));
            return;
        }
    }

    // Shutdown / cancel path: cancel any in-flight ops and close. The
    // close_direct CQE is what drives release_conn — cancel CQEs are
    // tagged CancelNoop so they don't prematurely free the slot before
    // the actual close fires.
    auto noop_tag = reinterpret_cast<void*>(make_ud(Op::CancelNoop, c.fd));
    if (c.recv_inflight) {
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        io_uring_prep_cancel(sqe,
            reinterpret_cast<void*>(make_ud(Op::Recv, c.fd)), 0);
        io_uring_sqe_set_data(sqe, noop_tag);
    }
    if (c.send_inflight) {
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        io_uring_prep_cancel(sqe,
            reinterpret_cast<void*>(make_ud(Op::Send, c.fd)), 0);
        io_uring_sqe_set_data(sqe, noop_tag);
    }
    int handle = c.fd;
    bool use_direct = c.fixed_file;
    c.fd = -1;
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (use_direct) {
        io_uring_prep_close_direct(sqe, unsigned(handle));
    } else {
        io_uring_prep_close(sqe, handle);
    }
    io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(make_ud(Op::Close, handle)));
}

void IoUringWorker::abort_all() {
    if (accept_inflight_) {
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        io_uring_prep_cancel(sqe,
            reinterpret_cast<void*>(make_ud(Op::Accept, listener_fd_)), 0);
        io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(
                                       make_ud(Op::CancelNoop, listener_fd_)));
    }
#ifdef SPIDO_WITH_KTLS
    if (tls_accept_inflight_) {
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        io_uring_prep_cancel(sqe,
            reinterpret_cast<void*>(make_ud(Op::TlsAccept, tls_listener_fd_)), 0);
        io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(
                                       make_ud(Op::CancelNoop, tls_listener_fd_)));
    }
#endif
    for (Conn* c : conns_by_fd_) {
        if (!c || c->fd < 0) continue;
        c->want_close = true;
#ifdef SPIDO_WITH_KTLS
        if (c->tls_poll_inflight) {
            io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            io_uring_prep_cancel(sqe,
                reinterpret_cast<void*>(make_ud(Op::TlsHandshakePoll, c->fd)), 0);
            io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(
                                           make_ud(Op::CancelNoop, c->fd)));
        }
        if (c->ssl) { ::SSL_free(c->ssl); c->ssl = nullptr; }
#endif
        close_conn(*c);
    }
}

} // namespace spido
