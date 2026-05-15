#pragma once

#include <cstddef>

namespace spido::net {

// Compiled spido_xdp.bpf.o, embedded into the library at build time by
// the SPIDO_WITH_XDP CMake path (clang → BPF → xxd -i). The blob is the
// raw ELF; libbpf parses it via bpf_object__open_mem.
struct XdpProgramBlob {
    const unsigned char* data;
    size_t               len;
};

XdpProgramBlob xdp_program_blob() noexcept;

// SEC name of the XDP program in the blob.
inline constexpr const char* kXdpProgSection = "xdp";
inline constexpr const char* kXdpMapName     = "xsks_map";

} // namespace spido::net
