#include "network/umem.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace spido::net {

std::unique_ptr<Umem> Umem::create(Config cfg) {
    auto self = std::unique_ptr<Umem>(new Umem(cfg));
    self->area_sz_ = cfg.frames * cfg.frame_size;

    // Page-aligned anonymous mapping. We let mmap pick the address; AF_XDP
    // takes a userspace pointer, not an fd.
    self->area_ = ::mmap(nullptr, self->area_sz_,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                         -1, 0);
    if (self->area_ == MAP_FAILED) {
        std::perror("spido/xdp: mmap UMEM");
        return nullptr;
    }

    xsk_umem_config ucfg{};
    ucfg.fill_size      = cfg.fq_size;
    ucfg.comp_size      = cfg.cq_size;
    ucfg.frame_size     = cfg.frame_size;
    ucfg.frame_headroom = 0;
    ucfg.flags          = 0;

    int rc = ::xsk_umem__create(&self->umem_, self->area_, self->area_sz_,
                                &self->fq_, &self->cq_, &ucfg);
    if (rc) {
        std::fprintf(stderr, "spido/xdp: xsk_umem__create failed: %s\n",
                     std::strerror(-rc));
        ::munmap(self->area_, self->area_sz_);
        self->area_ = nullptr;
        return nullptr;
    }

    // All frame addresses start out free.
    self->free_.reserve(cfg.frames);
    for (size_t i = 0; i < cfg.frames; ++i) {
        self->free_.push_back(i * cfg.frame_size);
    }
    return self;
}

Umem::~Umem() {
    if (umem_) ::xsk_umem__delete(umem_);
    if (area_) ::munmap(area_, area_sz_);
}

size_t Umem::replenish_fill(size_t n) {
    uint32_t idx = 0;
    uint32_t got = ::xsk_ring_prod__reserve(&fq_, n, &idx);
    size_t   placed = 0;
    for (uint32_t i = 0; i < got; ++i) {
        uint64_t addr = 0;
        if (!alloc(addr)) break;
        *::xsk_ring_prod__fill_addr(&fq_, idx + i) = addr;
        ++placed;
    }
    ::xsk_ring_prod__submit(&fq_, placed);
    return placed;
}

} // namespace spido::net
