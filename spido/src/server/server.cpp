#include "spido/server.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#include "server/access_log.h"
#ifdef SPIDO_WITH_JWT
#include "server/jwt.h"
#endif
#include "server/router.h"
#include "server/thread_pool.h"
#include "utils/cpu_affinity.h"

#include <unordered_map>

#ifdef SPIDO_WITH_XDP
#include "network/xdp_engine.h"
#endif

#ifdef SPIDO_WITH_KTLS
#include "server/tls.h"
#endif

namespace spido {

struct Server::Impl {
    ServerConfig                    cfg;
    Router                          router;
    std::unique_ptr<ThreadPool>     pool;
#ifdef SPIDO_WITH_XDP
    std::unique_ptr<net::XdpEngine> xdp;
#endif
#ifdef SPIDO_WITH_KTLS
    std::unique_ptr<TlsContext>     tls;
#endif
    std::unique_ptr<AccessLogger>   access_log;
#ifdef SPIDO_WITH_JWT
    std::unique_ptr<JwtVerifier>    jwt;
#endif
    std::unordered_map<std::string, Handler> named_handlers;
    std::atomic<bool>               stop_flag{false};
};

namespace {

// Blocks SIGINT/SIGTERM on the calling thread so the dedicated sigwait
// thread is the only one that picks them up. Workers inherit this mask
// and never get interrupted out of io_cqring_wait by stray signals.
void block_term_signals_for_workers() {
    sigset_t s;
    ::sigemptyset(&s);
    ::sigaddset(&s, SIGINT);
    ::sigaddset(&s, SIGTERM);
    ::pthread_sigmask(SIG_BLOCK, &s, nullptr);

    // Ignore SIGPIPE — a peer disconnect shouldn't tear down the process.
    struct sigaction sp{};
    sp.sa_handler = SIG_IGN;
    ::sigaction(SIGPIPE, &sp, nullptr);
}

// Builds the wire bytes for a static 200 response with the given body.
// Cached responses always advertise keep-alive; if the client requested
// close the worker still closes after sending — the header advertises
// keep-alive but the connection won't be reused, which is benign.
std::shared_ptr<const std::string>
build_static_response(std::string_view body, std::string_view content_type) {
    auto s = std::make_shared<std::string>();
    s->reserve(96 + body.size());
    s->append("HTTP/1.1 200 OK\r\nContent-Type: ");
    s->append(content_type);
    s->append("\r\nContent-Length: ");
    s->append(std::to_string(body.size()));
    s->append("\r\nConnection: keep-alive\r\n\r\n");
    s->append(body);
    return s;
}

} // namespace

Server::Server() : impl_(std::make_unique<Impl>()) {}

Server::Server(uint16_t port) : impl_(std::make_unique<Impl>()) {
    impl_->cfg.port = port;
}

Server::Server(ServerConfig cfg) : impl_(std::make_unique<Impl>()) {
    impl_->cfg = std::move(cfg);
}

Server::~Server() = default;

Server& Server::route(Method m, std::string_view path, Handler h,
                      bool require_auth) {
    impl_->router.add(m, path, std::move(h), require_auth);
    return *this;
}
Server& Server::get (std::string_view p, Handler h, bool a) { return route(Method::GET,    p, std::move(h), a); }
Server& Server::post(std::string_view p, Handler h, bool a) { return route(Method::POST,   p, std::move(h), a); }
Server& Server::put (std::string_view p, Handler h, bool a) { return route(Method::PUT,    p, std::move(h), a); }
Server& Server::del (std::string_view p, Handler h, bool a) { return route(Method::DELETE, p, std::move(h), a); }

ServerConfig& Server::config() noexcept             { return impl_->cfg; }
const ServerConfig& Server::config() const noexcept { return impl_->cfg; }

Server& Server::register_handler(std::string_view name, Handler h) {
    impl_->named_handlers[std::string(name)] = std::move(h);
    return *this;
}

Server& Server::route_by_name(Method m, std::string_view path,
                              std::string_view handler_name,
                              bool require_auth) {
    auto it = impl_->named_handlers.find(std::string(handler_name));
    if (it == impl_->named_handlers.end()) {
        std::fprintf(stderr,
            "spido: route_by_name(%.*s): handler \"%.*s\" not registered — "
            "route skipped\n",
            int(path.size()), path.data(),
            int(handler_name.size()), handler_name.data());
        return *this;
    }
    return route(m, path, it->second, require_auth);
}

Server& Server::serve_static(Method m, std::string_view path,
                             std::string_view body,
                             std::string_view content_type) {
    impl_->router.add_static(m, path, build_static_response(body, content_type));
    return *this;
}

void Server::stop() {
    if (!impl_) return;
    impl_->stop_flag.store(true, std::memory_order_release);
    // No signal handler / wake pipe — listen() is in sigwait. Programmatic
    // stop() works by sending SIGTERM to the calling process; that's the
    // most reliable way to wake sigwait from a non-signal context.
    ::kill(::getpid(), SIGTERM);
}

void Server::listen() {
    // Block SIGINT/SIGTERM on this thread before spawning workers so the
    // mask propagates. The main thread then sigwait's on these signals;
    // workers never see them.
    block_term_signals_for_workers();

#ifdef SPIDO_WITH_KTLS
    TlsContext* tls_ptr = nullptr;
    if (impl_->cfg.tls_port > 0 && !impl_->cfg.tls_cert_path.empty()) {
        impl_->tls = TlsContext::create(impl_->cfg.tls_cert_path,
                                        impl_->cfg.tls_key_path);
        if (!impl_->tls) {
            std::fprintf(stderr,
                "spido: TLS context init failed; tls_port disabled\n");
        }
        tls_ptr = impl_->tls.get();
    }
#endif

    // Optional access log. When the path is set we spawn a background
    // writer; workers push lines into a lock-free SPSC ring so the hot
    // path doesn't touch the file or take any lock.
    AccessLogger* logger_ptr = nullptr;
    if (!impl_->cfg.access_log_path.empty()) {
        unsigned n = impl_->cfg.threads ? impl_->cfg.threads
                                        : util::online_cpus();
        impl_->access_log = AccessLogger::create(
            impl_->cfg.access_log_path, n, impl_->cfg.access_log_drop_on_full);
        logger_ptr = impl_->access_log.get();
    }

#ifdef SPIDO_WITH_JWT
    // JWT verifier — built lazily, only when enabled. The hot path holds
    // a raw pointer; nullptr means "never check auth".
    JwtVerifier* jwt_ptr = nullptr;
    if (impl_->cfg.jwt.enabled) {
        JwtConfig jc;
        jc.enabled         = true;
        jc.algorithm       = impl_->cfg.jwt.algorithm;
        jc.secret          = impl_->cfg.jwt.secret;
        jc.public_key_pem  = impl_->cfg.jwt.public_key_pem;
        jc.required_claims = impl_->cfg.jwt.required_claims;
        jc.issuer          = impl_->cfg.jwt.issuer;
        jc.audience        = impl_->cfg.jwt.audience;
        jc.cache_size      = impl_->cfg.jwt.cache_size;
        jc.leeway_seconds  = impl_->cfg.jwt.leeway_seconds;

        impl_->jwt = std::make_unique<JwtVerifier>();
        if (!impl_->jwt->init(jc)) {
            std::fprintf(stderr,
                "spido: JWT init failed (bad algorithm/secret/key) — "
                "auth-required routes will always return 401\n");
        }
        jwt_ptr = impl_->jwt.get();
    }
#endif // SPIDO_WITH_JWT

    impl_->pool = std::make_unique<ThreadPool>(impl_->cfg, impl_->router,
                                               logger_ptr
#ifdef SPIDO_WITH_JWT
                                               , jwt_ptr
#endif
#ifdef SPIDO_WITH_KTLS
                                               , tls_ptr
#endif
                                               );
    impl_->pool->start();

    std::fprintf(stderr,
                 "spido: listening on %s:%u with %zu workers "
                 "(sqpoll=%d defer_taskrun=%d, per-worker SO_REUSEPORT)\n",
                 impl_->cfg.bind.c_str(), impl_->cfg.port,
                 impl_->pool->size(),
                 int(impl_->cfg.sqpoll && !impl_->cfg.defer_taskrun),
                 int(impl_->cfg.defer_taskrun));
#ifdef SPIDO_WITH_KTLS
    if (impl_->tls) {
        std::fprintf(stderr,
                     "spido: kTLS listener on %s:%u (cert=%s)\n",
                     impl_->cfg.bind.c_str(), impl_->cfg.tls_port,
                     impl_->cfg.tls_cert_path.c_str());
    }
#endif

#ifdef SPIDO_WITH_XDP
    if (impl_->cfg.xdp && !impl_->cfg.xdp_iface.empty()) {
        net::XdpEngine::Config xcfg;
        xcfg.iface           = impl_->cfg.xdp_iface;
        xcfg.num_queues      = impl_->cfg.xdp_queues;
        xcfg.prefer_zerocopy = impl_->cfg.xdp_zerocopy;
        xcfg.cpu_base        = impl_->pool->size();

        auto handler = [](net::XdpWorker&, net::XdpWorker::RxFrame f) {
            (void)f;
        };

        impl_->xdp = net::XdpEngine::start(std::move(xcfg), std::move(handler));
        if (impl_->xdp) {
            std::fprintf(stderr,
                         "spido: AF_XDP engine running on %s (queues=%d, zerocopy=%d)\n",
                         impl_->cfg.xdp_iface.c_str(),
                         impl_->xdp->num_queues(),
                         int(impl_->xdp->zerocopy()));
        } else {
            std::fprintf(stderr,
                         "spido: AF_XDP setup failed — falling back to io_uring only\n");
        }
    }
#endif

    // The main thread has no acceptor work — workers run their own accept
    // loops over SO_REUSEPORT listeners. Just sit in sigwait until SIGINT,
    // SIGTERM, or a programmatic stop() (which sends SIGTERM to self).
    sigset_t wait_set;
    ::sigemptyset(&wait_set);
    ::sigaddset(&wait_set, SIGINT);
    ::sigaddset(&wait_set, SIGTERM);
    int sig = 0;
    while (!impl_->stop_flag.load(std::memory_order_acquire)) {
        int rc = ::sigwait(&wait_set, &sig);
        if (rc != 0) {
            if (rc == EINTR) continue;
            break;
        }
        impl_->stop_flag.store(true, std::memory_order_release);
        break;
    }

#ifdef SPIDO_WITH_XDP
    if (impl_->xdp) {
        impl_->xdp->stop();
        impl_->xdp->join();
        impl_->xdp.reset();
    }
#endif

    impl_->pool->stop();
    impl_->pool->join();
    impl_->pool.reset();

    if (impl_->access_log) {
        impl_->access_log->stop();
        impl_->access_log.reset();
    }
}

} // namespace spido
