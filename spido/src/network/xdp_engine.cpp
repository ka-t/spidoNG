#include "network/xdp_engine.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/if_xdp.h>

#include "utils/cpu_affinity.h"

namespace spido::net {

namespace {
constexpr unsigned kBatch = 64;
} // namespace

XdpWorker::XdpWorker(unsigned cpu,
                     std::unique_ptr<XdpSocket> sock,
                     Umem& umem,
                     FrameHandler handler)
    : cpu_(cpu), sock_(std::move(sock)), umem_(umem),
      handler_(std::move(handler)) {}

XdpWorker::~XdpWorker() {
    stop();
    join();
}

void XdpWorker::start() {
    thread_ = std::thread([this] {
        if (!util::pin_thread(::pthread_self(), cpu_)) {
            std::fprintf(stderr, "spido/xdp: pin worker to CPU %u failed\n", cpu_);
        }
        run();
    });
}

void XdpWorker::stop() {
    stop_.store(true, std::memory_order_release);
}

void XdpWorker::join() {
    if (thread_.joinable()) thread_.join();
}

void XdpWorker::run() {
    // Seed the fill ring before we ask the kernel for any packets.
    umem_.replenish_fill(umem_.frames() / 2);

    pollfd pfd{};
    pfd.fd     = sock_->fd();
    pfd.events = POLLIN;

    while (!stop_.load(std::memory_order_acquire)) {
        // Refill before each iteration so the kernel always has empty
        // frames to write incoming packets into.
        umem_.replenish_fill(kBatch);

        // Short poll timeout so stop() is observed promptly even when
        // there's no traffic. Real loads will spend their time in drain_rx.
        int pr = ::poll(&pfd, 1, 100);
        if (pr < 0) {
            if (errno == EINTR) continue;
            std::perror("spido/xdp: poll");
            break;
        }
        if (pr == 0) continue;

        drain_rx();
        drain_completions();
        kick_tx();
    }

    // Drain once more on the way out so frames in the completion ring
    // get recycled into our free pool before UMEM teardown.
    drain_completions();
}

void XdpWorker::drain_rx() {
    uint32_t idx_rx = 0;
    uint32_t got    = ::xsk_ring_cons__peek(sock_->rx(), kBatch, &idx_rx);
    for (uint32_t i = 0; i < got; ++i) {
        const xdp_desc* d =
            ::xsk_ring_cons__rx_desc(sock_->rx(), idx_rx + i);
        const uint8_t* p = umem_.frame_data(d->addr);
        handler_(*this, RxFrame{p, d->len, d->addr});
        // Recycle the frame back to the free pool. A future userspace TCP
        // implementation may instead retain it across multiple events.
        umem_.release(d->addr);
    }
    if (got) ::xsk_ring_cons__release(sock_->rx(), got);
}

void XdpWorker::drain_completions() {
    uint32_t idx_cq = 0;
    uint32_t done   = ::xsk_ring_cons__peek(umem_.comp(), kBatch, &idx_cq);
    for (uint32_t i = 0; i < done; ++i) {
        uint64_t addr = *::xsk_ring_cons__comp_addr(umem_.comp(), idx_cq + i);
        umem_.release(addr);
    }
    if (done) ::xsk_ring_cons__release(umem_.comp(), done);
}

void XdpWorker::kick_tx() {
    // Tell the kernel there's data on the TX ring to drain. Needs to be
    // called for sockets without zero-copy auto-kick; cheap to always do.
    if (::xsk_ring_prod__needs_wakeup(sock_->tx())) {
        ::sendto(sock_->fd(), nullptr, 0, MSG_DONTWAIT, nullptr, 0);
    }
}

bool XdpWorker::tx_enqueue(const uint8_t* data, size_t len) {
    uint64_t addr = 0;
    if (!umem_.alloc(addr)) return false;
    if (len > umem_.frame_sz()) {
        umem_.release(addr);
        return false;
    }
    std::memcpy(umem_.frame_data(addr), data, len);

    uint32_t idx = 0;
    if (::xsk_ring_prod__reserve(sock_->tx(), 1, &idx) != 1) {
        umem_.release(addr);
        return false;
    }
    xdp_desc* d = ::xsk_ring_prod__tx_desc(sock_->tx(), idx);
    d->addr = addr;
    d->len  = static_cast<uint32_t>(len);
    ::xsk_ring_prod__submit(sock_->tx(), 1);
    return true;
}

// ---------------------------------------------------------------------------

std::unique_ptr<XdpEngine> XdpEngine::start(Config cfg,
                                            XdpWorker::FrameHandler handler) {
    auto self = std::unique_ptr<XdpEngine>(new XdpEngine());

    XdpLoader::Config lcfg;
    lcfg.iface = cfg.iface;
    self->loader_ = XdpLoader::load(lcfg);
    if (!self->loader_) return nullptr;

    self->umem_ = Umem::create(cfg.umem);
    if (!self->umem_) return nullptr;

    bool any_zc = false;
    for (int q = 0; q < cfg.num_queues; ++q) {
        XdpSocket::Config scfg;
        scfg.iface           = cfg.iface;
        scfg.queue           = q;
        scfg.prefer_zerocopy = cfg.prefer_zerocopy;
        auto sock = XdpSocket::create(*self->umem_, scfg);
        if (!sock) return nullptr;
        if (sock->config().achieved_zerocopy) any_zc = true;

        if (!self->loader_->register_xsk(q, sock->fd())) return nullptr;

        unsigned cpu = cfg.cpu_base + static_cast<unsigned>(q);
        auto w = std::make_unique<XdpWorker>(cpu, std::move(sock),
                                             *self->umem_, handler);
        self->workers_.push_back(std::move(w));
    }
    self->zerocopy_ = any_zc;

    for (auto& w : self->workers_) w->start();
    return self;
}

XdpEngine::~XdpEngine() {
    stop();
    join();
}

void XdpEngine::stop() {
    for (auto& w : workers_) w->stop();
}

void XdpEngine::join() {
    for (auto& w : workers_) w->join();
    workers_.clear();
}

} // namespace spido::net
