#include "server/thread_pool.h"

#include "utils/cpu_affinity.h"

namespace spido {

ThreadPool::ThreadPool(const ServerConfig& cfg, const Router& router,
                       AccessLogger* access_logger
#ifdef SPIDO_WITH_JWT
                       , JwtVerifier* jwt
#endif
#ifdef SPIDO_WITH_KTLS
                       , TlsContext* tls_ctx
#endif
                       )
    : cfg_(cfg), router_(router) {
    unsigned n = cfg_.threads ? cfg_.threads : util::online_cpus();
    cpus_ = util::default_cpu_set(n);
    workers_.reserve(n);
    for (unsigned i = 0; i < n; ++i) {
        AccessLogRing* ring = access_logger ? &access_logger->ring(i) : nullptr;
        workers_.emplace_back(std::make_unique<IoUringWorker>(
            i, cfg_, router_, ring
#ifdef SPIDO_WITH_JWT
            , jwt
#endif
#ifdef SPIDO_WITH_KTLS
            , tls_ctx
#endif
            ));
    }
}

ThreadPool::~ThreadPool() = default;

void ThreadPool::start() {
    for (size_t i = 0; i < workers_.size(); ++i) {
        workers_[i]->start(cpus_[i]);
    }
}

void ThreadPool::stop() {
    for (auto& w : workers_) w->stop();
}

void ThreadPool::join() {
    for (auto& w : workers_) w->join();
}

} // namespace spido
