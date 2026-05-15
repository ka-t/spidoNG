#pragma once

#include <cstddef>
#include <string_view>

#include "spido/request.h"

namespace spido {

// Stateless HTTP/1.1 request parser. Each call rescans the buffer from
// offset 0 — connection buffers grow append-only between recvs, and the
// header section is small enough that the cost is negligible compared to
// recv. Returns Done with consumed() == bytes used by this request so the
// caller can compact the buffer and parse the next pipelined message.
class HttpParser {
public:
    enum class Status : uint8_t {
        NeedMore,
        Done,
        BadRequest,
        TooLarge,
    };

    // The buffer is taken as mutable: when Transfer-Encoding: chunked is
    // used, decoded body bytes are written back into the same buffer in
    // place (chunked encoding always has overhead, so the decoded stream
    // is strictly shorter than the encoded one).
    Status parse(char* data, size_t len, size_t max_body, Request& out) noexcept;

    size_t consumed() const noexcept { return consumed_; }

private:
    size_t consumed_ = 0;
};

} // namespace spido
