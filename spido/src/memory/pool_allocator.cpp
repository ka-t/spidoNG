#include "memory/pool_allocator.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>

namespace spido::mem {

namespace {

constexpr size_t kCacheLine = detail::kCacheLine;

inline constexpr bool is_power_of_two(size_t x) noexcept {
    return x && ((x & (x - 1)) == 0);
}

inline constexpr size_t round_up(size_t n, size_t to) noexcept {
    return (n + to - 1) & ~(to - 1);
}

inline detail::AlignedBytes alloc_aligned_bytes(size_t bytes) {
    return detail::AlignedBytes(
        static_cast<std::byte*>(
            ::operator new[](bytes, std::align_val_t{kCacheLine})
        )
    );
}

} // namespace

FixedPool::FixedPool(size_t block_size, size_t initial_blocks)
    : block_size_(round_up(std::max(block_size, sizeof(FreeNode)), kCacheLine)) {
    assert(is_power_of_two(kCacheLine));

    chunks_.reserve(8);

    grow(initial_blocks ? initial_blocks : 64);
}

FixedPool::~FixedPool() = default;

void FixedPool::grow(size_t blocks) {
    if (blocks == 0) blocks = 64;

    if (block_size_ != 0 &&
        blocks > std::numeric_limits<size_t>::max() / block_size_) {
        std::abort();
    }

    const size_t bytes = block_size_ * blocks;

    auto raw = alloc_aligned_bytes(bytes);

    std::byte* const base = raw.get();

    FreeNode* head = free_list_;

    for (size_t i = 0; i < blocks; ++i) {
        auto* node = reinterpret_cast<FreeNode*>(base + i * block_size_);
        node->next = head;
        head = node;
    }

    free_list_ = head;

    chunks_.push_back({std::move(raw), blocks});
}

void* FixedPool::allocate() noexcept {
    if (!free_list_) [[unlikely]] {
        size_t next = chunks_.empty() ? 64 : chunks_.back().blocks * 2;
        if (next > 4096) next = 4096;
        grow(next);
    }

    FreeNode* const n = free_list_;
    free_list_ = n->next;

    return n;
}

void FixedPool::deallocate(void* p) noexcept {
    if (!p) return;

    auto* const node = static_cast<FreeNode*>(p);
    node->next = free_list_;
    free_list_ = node;
}

// ---------------------------------------------------------------------------

Arena::Arena(size_t slab_size)
    : slab_size_(std::max(slab_size, kCacheLine)) {
    slabs_.reserve(8);
    new_slab(slab_size_);
}

void Arena::new_slab(size_t at_least) {
    Slab s;

    s.cap = std::max(slab_size_, at_least);
    s.mem = alloc_aligned_bytes(s.cap);
    s.off = 0;

    slabs_.push_back(std::move(s));
    cur_ = slabs_.size() - 1;
}

void* Arena::allocate(size_t n, size_t align) {
    if (align == 0) align = alignof(std::max_align_t);

    assert(is_power_of_two(align));

    for (;;) {
        Slab& s = slabs_[cur_];

        size_t off;

        if (align <= kCacheLine) {
            off = round_up(s.off, align);
        } else {
            const auto base = reinterpret_cast<std::uintptr_t>(s.mem.get());
            const auto ptr  = base + s.off;
            const auto aligned = (ptr + align - 1) & ~(std::uintptr_t(align) - 1);
            off = static_cast<size_t>(aligned - base);
        }

        if (off <= s.cap && n <= s.cap - off) {
            void* const p = s.mem.get() + off;
            s.off = off + n;
            return p;
        }

        if (cur_ + 1 < slabs_.size()) {
            ++cur_;
            slabs_[cur_].off = 0;
            continue;
        }

        if (n > std::numeric_limits<size_t>::max() - align) {
            std::abort();
        }

        new_slab(n + align);
    }
}

void Arena::reset() noexcept {
    cur_ = 0;

    if (!slabs_.empty()) {
        slabs_[0].off = 0;
    }
}

} // namespace spido::mem