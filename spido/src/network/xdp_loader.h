#pragma once

#include <memory>
#include <string>
#include <string_view>

struct bpf_object;
struct bpf_program;

namespace spido::net {

// Loads the embedded eBPF object, attaches the XDP program to `iface`,
// and exposes the XSKMAP fd so AF_XDP sockets can register themselves
// for redirection. Destruction detaches the program.
class XdpLoader {
public:
    struct Config {
        std::string iface;
        // XDP_FLAGS_SKB_MODE (generic), DRV_MODE, or HW_MODE.
        // 0 = let kernel pick; usually generic on veth/loopback.
        unsigned    xdp_flags = 0;
    };

    static std::unique_ptr<XdpLoader> load(Config cfg);
    ~XdpLoader();

    XdpLoader(const XdpLoader&)            = delete;
    XdpLoader& operator=(const XdpLoader&) = delete;

    int                 xsks_map_fd() const noexcept { return xsks_map_fd_; }
    const Config&       config()      const noexcept { return cfg_; }
    int                 ifindex()     const noexcept { return ifindex_; }

    // Register an AF_XDP socket fd into the XSKMAP at `queue`.
    bool register_xsk(int queue, int xsk_fd);

private:
    XdpLoader(Config cfg) : cfg_(std::move(cfg)) {}

    Config       cfg_;
    bpf_object*  obj_         = nullptr;
    bpf_program* prog_        = nullptr;
    int          prog_fd_     = -1;
    int          xsks_map_fd_ = -1;
    int          ifindex_     = -1;
    bool         attached_    = false;
};

} // namespace spido::net
