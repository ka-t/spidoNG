#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace spido::mem {

namespace detail {

inline constexpr size_t kCacheLine = 64;

struct AlignedDelete {
    void operator()(std::byte* p) const noexcept {
        ::operator delete[](p, std::align_val_t{kCacheLine});
    }
};

using AlignedBytes = std::unique_ptr<std::byte[], AlignedDelete>;

} // namespace detail

// Fixed-block pool. Each pool is owned by a single thread (per-worker),
// so allocation/free is just a stack-pop / stack-push with no atomics.
// Blocks are 64-byte aligned to avoid false sharing on hot paths.
class FixedPool {
public:
    FixedPool(size_t block_size, size_t initial_blocks);
    ~FixedPool();

    FixedPool(const FixedPool&)            = delete;
    FixedPool& operator=(const FixedPool&) = delete;

    void* allocate() noexcept;
    void  deallocate(void* p) noexcept;

    size_t block_size() const noexcept { return block_size_; }

private:
    struct FreeNode { FreeNode* next; };

    struct Chunk {
        detail::AlignedBytes mem;
        size_t blocks;
    };

    void grow(size_t blocks);

    size_t    block_size_;
    FreeNode* free_list_ = nullptr;
    std::vector<Chunk> chunks_;
};

// Variable-size arena. Bumps a pointer through a chain of slabs and resets
// to the head between requests — the parser uses it for header tokens.
class Arena {
public:
    explicit Arena(size_t slab_size = 64 * 1024);
    ~Arena() = default;

    Arena(const Arena&)            = delete;
    Arena& operator=(const Arena&) = delete;

    void* allocate(size_t n, size_t align = alignof(std::max_align_t));

    template <typename T, typename... Args>
    T* create(Args&&... args) {
        void* p = allocate(sizeof(T), alignof(T));
        return ::new (p) T(std::forward<Args>(args)...);
    }

    // Reset all slabs but keep them mapped — amortizes allocation across
    // many requests on the same connection.
    void reset() noexcept;

private:
    struct Slab {
        detail::AlignedBytes mem;
        size_t cap = 0;
        size_t off = 0;
    };

    void new_slab(size_t at_least);

    size_t            slab_size_;
    std::vector<Slab> slabs_;
    size_t            cur_ = 0;
};

} // namespace spido::mem