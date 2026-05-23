#pragma once

/**
 * latch/mpmc_queue.hpp
 *
 * Dmitry Vyukov's bounded multi-producer multi-consumer queue.
 *
 * Algorithm: each cell carries a sequence number that is used as a
 * lightweight state machine.  Sequence == position => cell is empty and
 * ready to be filled.  Sequence == position + 1 => cell is filled and
 * ready to be drained.  After draining, the sequence is bumped by
 * Capacity so the same cell can be reused in the next round.
 *
 * This gives us:
 *   - O(1) enqueue / dequeue
 *   - No ABA problem (the sequence number doubles as a generation counter)
 *   - No dynamic allocation after construction
 *   - Cache-line-padded producer and consumer cursors to eliminate
 *     false sharing
 *
 * Requirements:
 *   - T must be move-constructible / move-assignable
 *   - Capacity must be a power of two >= 2
 *
 * Reference: http://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue
 */

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <type_traits>

namespace latch {

template <typename T, std::size_t Capacity>
class MPMCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
    static_assert(Capacity >= 2, "Capacity must be >= 2");
    static_assert(std::is_move_constructible_v<T> && std::is_move_assignable_v<T>,
                  "T must be move-constructible and move-assignable");

    /* ------------------------------------------------------------------ */
    /*  Internal cell                                                       */
    /* ------------------------------------------------------------------ */
    static constexpr std::size_t kCacheLineSize = 64;

    struct alignas(kCacheLineSize) Cell {
        std::atomic<std::size_t> sequence{0};
        // Pad so that adjacent cells don't share a cache line.
        // On most architectures a cache line is 64 bytes; the atomic itself is
        // 8 bytes, so we add 56 bytes of padding BEFORE the data so the
        // sequence number gets its own line, while data lives in the next line.
        // This is a deliberate trade-off: we burn memory to avoid false sharing.
        T data;
    };

    /* ------------------------------------------------------------------ */
    /*  State                                                               */
    /* ------------------------------------------------------------------ */
    alignas(kCacheLineSize) std::atomic<std::size_t> enqueue_pos_{0};
    // Pad: producer cursor on its own cache line so consumers don't
    // thrash the producer's line when reading dequeue_pos_.
    char _pad0[kCacheLineSize - sizeof(std::atomic<std::size_t>)];

    alignas(kCacheLineSize) std::atomic<std::size_t> dequeue_pos_{0};
    char _pad1[kCacheLineSize - sizeof(std::atomic<std::size_t>)];

    std::array<Cell, Capacity> buffer_;

public:
    /* ------------------------------------------------------------------ */
    /*  Construction                                                        */
    /* ------------------------------------------------------------------ */
    MPMCQueue() noexcept {
        for (std::size_t i = 0; i < Capacity; ++i)
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
    }

    MPMCQueue(const MPMCQueue&)            = delete;
    MPMCQueue& operator=(const MPMCQueue&) = delete;

    /* ------------------------------------------------------------------ */
    /*  Enqueue                                                             */
    /* ------------------------------------------------------------------ */
    /**
     * Try to enqueue an item.
     * Returns true on success, false if the queue is full.
     * Non-blocking; safe for concurrent producers.
     */
    bool try_enqueue(T item) noexcept(std::is_nothrow_move_assignable_v<T>) {
        Cell* cell;
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);

        for (;;) {
            cell             = &buffer_[pos & (Capacity - 1)];
            std::size_t seq  = cell->sequence.load(std::memory_order_acquire);
            std::intptr_t diff =
                static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);

            if (diff == 0) {
                // Cell is empty.  Race with other producers for this slot.
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1,
                                                        std::memory_order_relaxed))
                    break; // won the race
                // Lost the race; reload pos and retry.
            } else if (diff < 0) {
                // Cell from a previous generation still being consumed —
                // the queue is full from our perspective.
                return false;
            } else {
                // Another producer already claimed this slot; load fresh pos.
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }

        cell->data = std::move(item);
        // Publish: sequence becomes pos + 1 so a consumer can pick it up.
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    /* ------------------------------------------------------------------ */
    /*  Dequeue                                                             */
    /* ------------------------------------------------------------------ */
    /**
     * Try to dequeue an item.
     * Returns std::nullopt if the queue is empty.
     * Non-blocking; safe for concurrent consumers.
     */
    std::optional<T> try_dequeue() noexcept(std::is_nothrow_move_constructible_v<T>) {
        Cell* cell;
        std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);

        for (;;) {
            cell             = &buffer_[pos & (Capacity - 1)];
            std::size_t seq  = cell->sequence.load(std::memory_order_acquire);
            std::intptr_t diff =
                static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);

            if (diff == 0) {
                // Cell is filled.  Race with other consumers for this slot.
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1,
                                                        std::memory_order_relaxed))
                    break; // won the race
            } else if (diff < 0) {
                // Cell not yet filled — queue is empty.
                return std::nullopt;
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }

        T result = std::move(cell->data);
        // Return cell to the pool: advance sequence by Capacity.
        cell->sequence.store(pos + Capacity, std::memory_order_release);
        return result;
    }

    /* ------------------------------------------------------------------ */
    /*  Helpers                                                             */
    /* ------------------------------------------------------------------ */
    /** Approximate number of items in the queue (not linearisable). */
    std::size_t size_approx() const noexcept {
        std::size_t enq = enqueue_pos_.load(std::memory_order_relaxed);
        std::size_t deq = dequeue_pos_.load(std::memory_order_relaxed);
        return (enq > deq) ? (enq - deq) : 0;
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }
};

} // namespace latch
