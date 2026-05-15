#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <bpf/xsk.h>

namespace spido::net {

// UMEM is the shared memory region the kernel reads/writes packets into
// for an AF_XDP socket. We pre-carve it into fixed-size frames and hand
// out frame addresses (byte offsets into the area) to the rings.
//
// Layout: 4096 frames * 4 KiB = 16 MiB by default. Frames addresses are
// the offset into the area, which is what AF_XDP descriptors expect.
class Umem {
public:
    struct Config {
        size_t frames     = 4096;
        size_t frame_size = 4096;
        size_t fq_size    = 4096;  // fill ring
        size_t cq_size    = 4096;  // completion ring
    };

    // Returns nullptr on failure.
    static std::unique_ptr<Umem> create(Config cfg);
    ~Umem();

    Umem(const Umem&)            = delete;
    Umem& operator=(const Umem&) = delete;

    xsk_umem*           handle()   noexcept { return umem_; }
    void*               area()     noexcept { return area_; }
    size_t              frames()   const noexcept { return cfg_.frames; }
    size_t              frame_sz() const noexcept { return cfg_.frame_size; }
    xsk_ring_prod*      fill()     noexcept { return &fq_; }
    xsk_ring_cons*      comp()     noexcept { return &cq_; }

    // Pointer to the start of frame at offset `addr`.
    uint8_t* frame_data(uint64_t addr) noexcept {
        return static_cast<uint8_t*>(area_) + addr;
    }

    // Pop a free frame address; returns false if none available.
    bool alloc(uint64_t& out) noexcept {
        if (free_.empty()) return false;
        out = free_.back();
        free_.pop_back();
        return true;
    }
    void release(uint64_t addr) { free_.push_back(addr); }

    // Reserve N entries on the fill ring and stage the first N free frames.
    // Returns number actually staged.
    size_t replenish_fill(size_t n);

private:
    Umem(Config cfg) : cfg_(cfg) {}

    Config              cfg_;
    void*               area_   = nullptr;
    size_t              area_sz_ = 0;
    xsk_umem*           umem_   = nullptr;
    xsk_ring_prod       fq_{};
    xsk_ring_cons       cq_{};
    std::vector<uint64_t> free_;
};

} // namespace spido::net
