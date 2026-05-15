#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "network/umem.h"
#include "network/xdp_loader.h"
#include "network/xdp_socket.h"

namespace spido::net {

// Per-queue worker. Pinned to one CPU, owns one AF_XDP socket and drives
// its RX/TX/fill/completion rings. Frame processing is delegated through
// the FrameHandler callback so a userspace TCP/IP stack can plug in later.
class XdpWorker {
public:
    // Inbound frame view. payload is the raw Ethernet frame as the NIC
    // delivered it. To send a reply, push frames via tx_enqueue() before
    // returning — the engine flushes after the handler call.
    struct RxFrame {
        const uint8_t* data;
        size_t         len;
        uint64_t       umem_addr; // for recycling
    };

    using FrameHandler = std::function<void(XdpWorker& self, RxFrame)>;

    XdpWorker(unsigned cpu,
              std::unique_ptr<XdpSocket> sock,
              Umem& umem,
              FrameHandler handler);
    ~XdpWorker();

    void start();
    void stop();
    void join();

    // Allocate a tx frame, copy `data` into it, and queue it on the TX ring.
    // Returns false if the TX ring or UMEM has no capacity right now.
    bool tx_enqueue(const uint8_t* data, size_t len);

private:
    void run();
    void drain_rx();
    void drain_completions();
    void kick_tx();

    unsigned                    cpu_;
    std::unique_ptr<XdpSocket>  sock_;
    Umem&                       umem_;
    FrameHandler                handler_;
    std::atomic<bool>           stop_{false};
    std::thread                 thread_;
};

// Top-level XDP engine: loads the BPF program once, sets up one worker
// per (iface, queue) pair, and orchestrates start/stop.
class XdpEngine {
public:
    struct Config {
        std::string iface;
        int         num_queues = 1;
        Umem::Config umem;
        bool        prefer_zerocopy = true;
        unsigned    cpu_base = 0; // first CPU to pin queue 0 to
    };

    // Returns nullptr on any setup failure. Caller can then fall back to
    // the io_uring path.
    static std::unique_ptr<XdpEngine> start(Config cfg,
                                            XdpWorker::FrameHandler handler);
    ~XdpEngine();

    void stop();
    void join();

    bool zerocopy() const noexcept { return zerocopy_; }
    int  num_queues() const noexcept { return static_cast<int>(workers_.size()); }

private:
    XdpEngine() = default;

    std::unique_ptr<XdpLoader>               loader_;
    std::unique_ptr<Umem>                    umem_;
    std::vector<std::unique_ptr<XdpWorker>>  workers_;
    bool                                     zerocopy_ = false;
};

} // namespace spido::net
