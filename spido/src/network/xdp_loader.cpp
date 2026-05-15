#include "network/xdp_loader.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <net/if.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "network/xdp_program.h"

namespace spido::net {

std::unique_ptr<XdpLoader> XdpLoader::load(Config cfg) {
    auto self = std::unique_ptr<XdpLoader>(new XdpLoader(cfg));

    self->ifindex_ = static_cast<int>(::if_nametoindex(self->cfg_.iface.c_str()));
    if (self->ifindex_ == 0) {
        std::fprintf(stderr, "spido/xdp: unknown interface '%s': %s\n",
                     self->cfg_.iface.c_str(), std::strerror(errno));
        return nullptr;
    }

    auto blob = xdp_program_blob();
    if (blob.len == 0) {
        std::fprintf(stderr, "spido/xdp: BPF object blob is empty — build without SPIDO_WITH_XDP?\n");
        return nullptr;
    }

    self->obj_ = ::bpf_object__open_mem(blob.data, blob.len, nullptr);
    if (!self->obj_) {
        std::fprintf(stderr, "spido/xdp: bpf_object__open_mem failed\n");
        return nullptr;
    }

    if (int rc = ::bpf_object__load(self->obj_); rc) {
        std::fprintf(stderr, "spido/xdp: bpf_object__load failed: %s\n",
                     std::strerror(-rc));
        ::bpf_object__close(self->obj_);
        self->obj_ = nullptr;
        return nullptr;
    }

    self->prog_ = ::bpf_object__find_program_by_name(self->obj_, "spido_xdp");
    if (!self->prog_) {
        std::fprintf(stderr, "spido/xdp: program 'spido_xdp' not found in object\n");
        return nullptr;
    }
    self->prog_fd_ = ::bpf_program__fd(self->prog_);

    bpf_map* map = ::bpf_object__find_map_by_name(self->obj_, kXdpMapName);
    if (!map) {
        std::fprintf(stderr, "spido/xdp: map '%s' not found\n", kXdpMapName);
        return nullptr;
    }
    self->xsks_map_fd_ = ::bpf_map__fd(map);

    // Replace any previously attached program. If a stale one is bound to
    // the iface this avoids EBUSY on repeated runs.
    int rc = ::bpf_xdp_attach(self->ifindex_, self->prog_fd_,
                              self->cfg_.xdp_flags | XDP_FLAGS_UPDATE_IF_NOEXIST,
                              nullptr);
    if (rc == -EBUSY || rc == -EEXIST) {
        rc = ::bpf_xdp_attach(self->ifindex_, self->prog_fd_,
                              self->cfg_.xdp_flags, nullptr);
    }
    if (rc) {
        std::fprintf(stderr, "spido/xdp: bpf_xdp_attach failed: %s\n",
                     std::strerror(-rc));
        return nullptr;
    }
    self->attached_ = true;
    return self;
}

XdpLoader::~XdpLoader() {
    if (attached_) {
        ::bpf_xdp_detach(ifindex_, cfg_.xdp_flags, nullptr);
    }
    if (obj_) ::bpf_object__close(obj_);
}

bool XdpLoader::register_xsk(int queue, int xsk_fd) {
    if (xsks_map_fd_ < 0) return false;
    int rc = ::bpf_map_update_elem(xsks_map_fd_, &queue, &xsk_fd, 0);
    if (rc) {
        std::fprintf(stderr, "spido/xdp: register XSK queue=%d failed: %s\n",
                     queue, std::strerror(-rc));
        return false;
    }
    return true;
}

} // namespace spido::net
