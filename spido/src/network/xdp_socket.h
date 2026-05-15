#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <bpf/xsk.h>

#include "network/umem.h"

namespace spido::net {

// AF_XDP socket bound to one (iface, queue). One socket per worker — the
// BPF program steers traffic for a given rx_queue_index into the XSKMAP
// at that index, so per-queue parallelism on the NIC maps cleanly onto
// userspace cores.
class XdpSocket {
public:
    struct Config {
        std::string iface;
        int         queue          = 0;
        size_t      rx_size        = 2048;
        size_t      tx_size        = 2048;
        bool        prefer_zerocopy = true;
        // True if `prefer_zerocopy` was honored at bind time.
        bool        achieved_zerocopy = false;
    };

    static std::unique_ptr<XdpSocket> create(Umem& umem, Config cfg);
    ~XdpSocket();

    XdpSocket(const XdpSocket&)            = delete;
    XdpSocket& operator=(const XdpSocket&) = delete;

    int             fd() const noexcept;
    xsk_socket*     handle() noexcept { return xsk_; }
    xsk_ring_cons*  rx()     noexcept { return &rx_; }
    xsk_ring_prod*  tx()     noexcept { return &tx_; }

    const Config&   config() const noexcept { return cfg_; }

private:
    XdpSocket(Config cfg) : cfg_(std::move(cfg)) {}

    Config         cfg_;
    xsk_socket*    xsk_ = nullptr;
    xsk_ring_cons  rx_{};
    xsk_ring_prod  tx_{};
};

} // namespace spido::net
