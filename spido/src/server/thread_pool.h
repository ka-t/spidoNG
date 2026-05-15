#pragma once

#include <memory>
#include <vector>

#include "server/access_log.h"
#include "server/io_uring_server.h"
#ifdef SPIDO_WITH_JWT
#include "server/jwt.h"
#endif
#include "spido/server.h"

#ifdef SPIDO_WITH_KTLS
#include "server/tls.h"
#endif

namespace spido {

class Router;

// Owns the per-core IoUringWorker fleet. The pool itself is just an
// addressable collection — distribution policy lives in the acceptor.
class ThreadPool {
public:
    ThreadPool(const ServerConfig& cfg, const Router& router,
               AccessLogger* access_logger
#ifdef SPIDO_WITH_JWT
               , JwtVerifier* jwt
#endif
#ifdef SPIDO_WITH_KTLS
               , TlsContext* tls_ctx
#endif
               );
    ~ThreadPool();

    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void start();
    void stop();
    void join();

    size_t          size()  const noexcept { return workers_.size(); }
    IoUringWorker&  at(size_t i)         { return *workers_[i]; }

private:
    const ServerConfig& cfg_;
    const Router&       router_;
    std::vector<std::unique_ptr<IoUringWorker>> workers_;
    std::vector<unsigned>                        cpus_;
};

} // namespace spido
