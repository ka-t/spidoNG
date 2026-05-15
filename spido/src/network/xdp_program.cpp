#include "network/xdp_program.h"

// The CMake rule generates this file with the embedded BPF object as a
// byte array. Symbol naming follows xxd -i for ${BPF_OBJ}.
extern "C" {
extern const unsigned char spido_xdp_bpf_o[];
extern const unsigned int  spido_xdp_bpf_o_len;
}

namespace spido::net {

XdpProgramBlob xdp_program_blob() noexcept {
    return {spido_xdp_bpf_o, spido_xdp_bpf_o_len};
}

} // namespace spido::net
