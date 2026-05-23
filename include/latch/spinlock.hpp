#pragma once

/**
 * latch/spinlock.hpp
 *
 * Test-and-test-and-set spinlock with hardware pause hint.
 *
 * Strategy:
 *   1. Fast path: a single exchange to try to grab the lock.
 *   2. Slow path: spin on a load (not an exchange) so we don't
 *      broadcast cache-line invalidations to every core while waiting.
 *      This is the "test-and-test-and-set" optimisation.
 *   3. PAUSE / YIELD hint so the CPU knows it is in a spin loop —
 *      avoids pipeline hazards on x86 (memory-order machine-clear) and
 *      hints the hyperthread scheduler on SMT systems.
 *
 * The Spinlock satisfies the BasicLockable named requirement, so it can
 * be used with std::lock_guard / std::unique_lock.
 *
 * When NOT to use this:
 *   If the critical section is longer than a few dozen nanoseconds, prefer
 *   std::mutex, which yields the OS scheduler slot instead of burning CPU.
 */

#include <atomic>
#include <thread>

namespace latch {

class Spinlock {
public:
    Spinlock() noexcept = default;

    Spinlock(const Spinlock&)            = delete;
    Spinlock& operator=(const Spinlock&) = delete;

    /* ------------------------------------------------------------------ */
    /*  Lock                                                                */
    /* ------------------------------------------------------------------ */
    void lock() noexcept {
        for (;;) {
            // Fast path: attempt acquisition without thrashing the cache line.
            if (!flag_.exchange(true, std::memory_order_acquire))
                return;

            // Slow path: spin on a read until the lock looks free.
            while (flag_.load(std::memory_order_relaxed)) {
                spin_hint();
            }
        }
    }

    bool try_lock() noexcept {
        return !flag_.load(std::memory_order_relaxed) &&
               !flag_.exchange(true, std::memory_order_acquire);
    }

    void unlock() noexcept {
        flag_.store(false, std::memory_order_release);
    }

private:
    /** Issue the appropriate CPU hint for a spin-wait loop. */
    static void spin_hint() noexcept {
#if defined(__x86_64__) || defined(__i386__)
        // PAUSE: prevents speculative memory-order violations on x86 and
        // gives the hyperthread on the other logical core a bigger slice.
        __asm__ volatile("pause" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
        // YIELD: hint to the pipeline that this is a spin-wait.
        __asm__ volatile("yield" ::: "memory");
#else
        // Fallback: just let the OS reschedule.
        std::this_thread::yield();
#endif
    }

    std::atomic<bool> flag_{false};
};

} // namespace latch
