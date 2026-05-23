#pragma once

/**
 * latch/seqlock.hpp
 *
 * Sequence lock — optimised for read-heavy, write-rare shared data.
 *
 * How it works:
 *   - A 64-bit sequence counter is maintained alongside the payload.
 *   - Writers: increment seq (odd = locked), update payload,
 *     increment seq again (even = unlocked).
 *   - Readers: snapshot seq1, read payload, snapshot seq2.
 *     If seq1 == seq2 AND seq1 is even, the read was coherent.
 *     Otherwise retry.
 *
 * Why this is fast for readers:
 *   Readers never write to shared state — they perform zero atomic
 *   read-modify-write operations.  On hardware with a TSO memory model
 *   (x86) reads are nearly free; on weak-memory ISAs (ARM) the acquire
 *   fences are still lighter than a CAS.
 *
 * Constraints:
 *   - T must be trivially copyable (bit-for-bit copy during read is safe).
 *   - Only ONE writer thread at a time (no mutual exclusion between writers
 *     is provided — pair with a Spinlock or std::mutex if you have multiple
 *     writers).
 *   - Readers and the writer may run concurrently.
 */

#include <atomic>
#include <cstdint>
#include <thread>
#include <type_traits>

namespace latch {

template <typename T>
class Seqlock {
    static_assert(std::is_trivially_copyable_v<T>,
                  "Seqlock<T> requires T to be trivially copyable");

    static constexpr std::size_t kCacheLineSize = 64;

public:
    Seqlock() noexcept : seq_(0), data_{} {}

    explicit Seqlock(const T& initial) noexcept : seq_(0), data_(initial) {}

    /* ------------------------------------------------------------------ */
    /*  Write  (single-writer; protect with a mutex if you have multiple)  */
    /* ------------------------------------------------------------------ */
    void write(const T& val) noexcept {
        // Step 1: begin write — make sequence odd so readers know to retry.
        // acq_rel: we acquire any prior reader sequences (not strictly needed
        // here) and release the new odd value to readers.
        seq_.fetch_add(1, std::memory_order_acq_rel);

        // Step 2: write payload.  The fence ensures this store is not
        // reordered BEFORE the odd-seq store above (on the writer side) and
        // not reordered AFTER the even-seq store below.
        data_ = val;
        std::atomic_thread_fence(std::memory_order_release);

        // Step 3: end write — make sequence even again.
        seq_.fetch_add(1, std::memory_order_release);
    }

    /* ------------------------------------------------------------------ */
    /*  Read  (multiple concurrent readers allowed)                        */
    /* ------------------------------------------------------------------ */
    T read() const noexcept {
        T result;
        for (;;) {
            // Acquire seq1 so we see the latest writer state.
            std::uint64_t s1 = seq_.load(std::memory_order_acquire);
            if (s1 & 1u) {
                // Writer active — spin rather than reading torn data.
                spin_hint();
                continue;
            }

            // Read payload optimistically.
            result = data_;

            // Acquire fence to prevent the payload load from being reordered
            // after the second sequence load.
            std::atomic_thread_fence(std::memory_order_acquire);

            std::uint64_t s2 = seq_.load(std::memory_order_relaxed);
            if (s1 == s2) return result;   // clean read
            // seq changed under us — a write interleaved, retry.
        }
    }

    /* ------------------------------------------------------------------ */
    /*  Helpers                                                             */
    /* ------------------------------------------------------------------ */
    /** Returns true if a write is currently in progress (seq is odd). */
    bool write_in_progress() const noexcept {
        return seq_.load(std::memory_order_relaxed) & 1u;
    }

private:
    static void spin_hint() noexcept {
#if defined(__x86_64__) || defined(__i386__)
        __asm__ volatile("pause" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
        __asm__ volatile("yield" ::: "memory");
#else
        std::this_thread::yield();
#endif
    }

    alignas(kCacheLineSize) std::atomic<std::uint64_t> seq_;
    alignas(kCacheLineSize) T data_;
};

} // namespace latch
