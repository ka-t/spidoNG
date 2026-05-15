#include "network/xdp_socket.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <linux/if_xdp.h>

namespace spido::net {

std::unique_ptr<XdpSocket> XdpSocket::create(Umem& umem, Config cfg) {
    auto self = std::unique_ptr<XdpSocket>(new XdpSocket(cfg));

    // Try zero-copy first if the driver supports it; otherwise libbpf will
    // return -EOPNOTSUPP and we re-try in copy mode.
    auto try_bind = [&](uint16_t bind_flags) -> int {
        xsk_socket_config scfg{};
        scfg.rx_size = self->cfg_.rx_size;
        scfg.tx_size = self->cfg_.tx_size;
        // Don't let libbpf load its built-in XDP program — we attach our own.
        scfg.libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD;
        scfg.xdp_flags    = 0;
        scfg.bind_flags   = bind_flags;

        return ::xsk_socket__create(&self->xsk_,
                                    self->cfg_.iface.c_str(),
                                    self->cfg_.queue,
                                    umem.handle(),
                                    &self->rx_, &self->tx_,
                                    &scfg);
    };

    int rc = -EOPNOTSUPP;
    if (self->cfg_.prefer_zerocopy) {
        rc = try_bind(XDP_ZEROCOPY);
        if (rc == 0) self->cfg_.achieved_zerocopy = true;
    }
    if (rc) {
        rc = try_bind(XDP_COPY);
        if (rc == 0) self->cfg_.achieved_zerocopy = false;
    }
    if (rc) {
        std::fprintf(stderr,
                     "spido/xdp: xsk_socket__create %s:q%d failed: %s\n",
                     self->cfg_.iface.c_str(), self->cfg_.queue,
                     std::strerror(-rc));
        return nullptr;
    }
    return self;
}

XdpSocket::~XdpSocket() {
    if (xsk_) ::xsk_socket__delete(xsk_);
}

int XdpSocket::fd() const noexcept {
    return xsk_ ? ::xsk_socket__fd(xsk_) : -1;
}

} // namespace spido::net
