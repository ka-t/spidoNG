#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>

namespace spido::util {

// Bounded single-producer single-consumer queue. Capacity must be a power
// of two; head/tail counters are free-running and masked on access so the
// queue can hold capacity items (not capacity-1).
//
// One acceptor thread enqueues accepted fds; the owning worker dequeues.
// MPSC isn't needed because each ring is paired 1:1 with a worker.
template <typename T>
class SpscRing {
    static_assert(std::is_trivially_destructible_v<T> || std::is_move_constructible_v<T>);

public:
    explicit SpscRing(size_t capacity)
        : cap_(capacity), mask_(capacity - 1) {
        if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
            // Round up to next power of two.
            cap_ = std::bit_ceil(capacity == 0 ? size_t{1} : capacity);
            mask_ = cap_ - 1;
        }
        buf_ = std::unique_ptr<Slot[]>(new Slot[cap_]);
    }

    bool push(T v) noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);
        const auto head = head_.load(std::memory_order_acquire);
        if (tail - head >= cap_) return false;          // full
        buf_[tail & mask_].store(std::move(v));
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& out) noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto tail = tail_.load(std::memory_order_acquire);
        if (head == tail) return false;                  // empty
        buf_[head & mask_].load(out);
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    size_t capacity() const noexcept { return cap_; }

private:
    // Per-slot storage in raw bytes so we never default-construct T.
    struct alignas(64) Slot {
        alignas(T) std::byte storage[sizeof(T)];
        void store(T&& v) { ::new (storage) T(std::move(v)); }
        void load(T& out) {
            T* p = std::launder(reinterpret_cast<T*>(storage));
            out  = std::move(*p);
            p->~T();
        }
    };

    size_t                       cap_;
    size_t                       mask_;
    std::unique_ptr<Slot[]>      buf_;

    // Counters on their own cache lines to avoid false sharing.
    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> tail_{0};
};

} // namespace spido::util
