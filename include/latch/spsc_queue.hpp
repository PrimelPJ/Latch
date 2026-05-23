#pragma once

/**
 * latch/spsc_queue.hpp
 *
 * Single-producer / single-consumer (SPSC) bounded ring buffer.
 *
 * When you only have one writer and one reader, you can avoid all CAS
 * loops: the producer owns `tail_` exclusively; the consumer owns
 * `head_` exclusively.  Each side reads the other's cursor with acquire
 * semantics — that's all the synchronisation needed.
 *
 * This makes the SPSC queue the fastest possible concurrent queue.
 * Benchmark numbers on a modern x86 machine typically show 5–10x
 * throughput over even a well-tuned MPMC queue.
 *
 * Requirements:
 *   - Exactly ONE thread may call try_enqueue at a time.
 *   - Exactly ONE thread may call try_dequeue at a time.
 *   - Capacity must be a power of two >= 2.
 */

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <type_traits>

namespace latch {

template <typename T, std::size_t Capacity>
class SPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
    static_assert(Capacity >= 2, "Capacity must be >= 2");

    static constexpr std::size_t kMask           = Capacity - 1;
    static constexpr std::size_t kCacheLineSize   = 64;

public:
    SPSCQueue() noexcept : head_(0), tail_(0) {}

    SPSCQueue(const SPSCQueue&)            = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    /* ------------------------------------------------------------------ */
    /*  Producer side                                                       */
    /* ------------------------------------------------------------------ */
    bool try_enqueue(T item) noexcept(std::is_nothrow_move_assignable_v<T>) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        // The slot AFTER tail; if it equals head, the queue is full.
        const std::size_t next = (tail + 1) & kMask;
        if (next == head_.load(std::memory_order_acquire))
            return false;   // full

        buffer_[tail] = std::move(item);
        tail_.store(next, std::memory_order_release);
        return true;
    }

    /* ------------------------------------------------------------------ */
    /*  Consumer side                                                       */
    /* ------------------------------------------------------------------ */
    std::optional<T> try_dequeue() noexcept(std::is_nothrow_move_constructible_v<T>) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire))
            return std::nullopt;    // empty

        T result = std::move(buffer_[head]);
        head_.store((head + 1) & kMask, std::memory_order_release);
        return result;
    }

    /* ------------------------------------------------------------------ */
    /*  Helpers                                                             */
    /* ------------------------------------------------------------------ */
    bool empty() const noexcept {
        return head_.load(std::memory_order_relaxed) ==
               tail_.load(std::memory_order_relaxed);
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    // Producer-side cursor — on its own cache line.
    alignas(kCacheLineSize) std::atomic<std::size_t> head_;
    char _pad0[kCacheLineSize - sizeof(std::atomic<std::size_t>)];

    // Consumer-side cursor — on its own cache line.
    alignas(kCacheLineSize) std::atomic<std::size_t> tail_;
    char _pad1[kCacheLineSize - sizeof(std::atomic<std::size_t>)];

    std::array<T, Capacity> buffer_;
};

} // namespace latch
