#pragma once

/**
 * latch/thread_pool.hpp
 *
 * Fixed-size thread pool backed by the latch::MPMCQueue.
 *
 * Design:
 *   - A pool of N worker threads all pull tasks from a shared MPMC queue.
 *   - submit() returns a std::future<R> so callers can wait for results.
 *   - submit() spins (with yield) if the queue is full — this provides
 *     natural back-pressure instead of silently dropping tasks.
 *   - Workers drain the remaining queue after stop_ is set, ensuring every
 *     submitted task runs to completion before the destructor returns.
 *
 * Usage:
 *   latch::ThreadPool pool(4);
 *   auto fut = pool.submit([](int x) { return x * x; }, 7);
 *   std::cout << fut.get() << '\n';  // prints 49
 *
 * Thread safety: all public methods are safe to call concurrently.
 */

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

#include "mpmc_queue.hpp"

namespace latch {

class ThreadPool {
    using Task = std::function<void()>;

    // 4096-slot queue gives plenty of headroom for bursts while keeping the
    // fixed allocation reasonable (~256 KB for pointer-sized elements).
    static constexpr std::size_t kQueueCapacity = 4096;

public:
    /* ------------------------------------------------------------------ */
    /*  Construction / destruction                                          */
    /* ------------------------------------------------------------------ */
    explicit ThreadPool(
        std::size_t num_threads = std::thread::hardware_concurrency())
        : stop_(false) {
        if (num_threads == 0)
            throw std::invalid_argument("ThreadPool: num_threads must be > 0");

        workers_.reserve(num_threads);
        for (std::size_t i = 0; i < num_threads; ++i)
            workers_.emplace_back([this] { worker_loop(); });
    }

    ~ThreadPool() {
        // Signal workers to drain and exit.
        stop_.store(true, std::memory_order_release);
        for (auto& t : workers_)
            if (t.joinable()) t.join();
    }

    // Non-copyable, non-movable (threads hold a pointer to *this).
    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /* ------------------------------------------------------------------ */
    /*  Submit                                                              */
    /* ------------------------------------------------------------------ */
    /**
     * Submit a callable with arguments to the pool.
     * Returns std::future<R> where R = std::invoke_result_t<F, Args...>.
     * Blocks (spin-yield) if the internal queue is full.
     * Throws std::runtime_error if called after the pool has been destroyed.
     */
    template <typename F, typename... Args>
    [[nodiscard]] auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>> {
        using R = std::invoke_result_t<F, Args...>;

        if (stop_.load(std::memory_order_relaxed))
            throw std::runtime_error("ThreadPool: pool is stopped");

        // Wrap in packaged_task so we get a future for free.
        // Heap-allocate via shared_ptr so the lambda stored in the queue
        // is cheaply copyable (pointer-sized capture).
        auto task_ptr = std::make_shared<std::packaged_task<R()>>(
            [f   = std::forward<F>(f),
             // Capture args as a tuple to avoid the forwarding reference
             // lifetime issue when args are temporaries.
             tup = std::make_tuple(std::forward<Args>(args)...)]() mutable {
                return std::apply(std::move(f), std::move(tup));
            });

        std::future<R> fut = task_ptr->get_future();

        // Enqueue.  Spin-yield if full — provides natural back-pressure.
        Task wrapper = [task_ptr] { (*task_ptr)(); };
        while (!queue_.try_enqueue(std::move(wrapper))) {
            std::this_thread::yield();
            wrapper = [task_ptr] { (*task_ptr)(); }; // rebuild after move
        }

        return fut;
    }

    /* ------------------------------------------------------------------ */
    /*  Helpers                                                             */
    /* ------------------------------------------------------------------ */
    std::size_t thread_count() const noexcept { return workers_.size(); }
    std::size_t pending_approx() const noexcept { return queue_.size_approx(); }

private:
    void worker_loop() {
        while (!stop_.load(std::memory_order_relaxed)) {
            if (auto task = queue_.try_dequeue()) {
                (*task)();
            } else {
                // No work available — yield to avoid burning 100% CPU.
                std::this_thread::yield();
            }
        }
        // Drain remaining tasks before exiting.
        while (auto task = queue_.try_dequeue())
            (*task)();
    }

    MPMCQueue<Task, kQueueCapacity> queue_;
    std::vector<std::thread>        workers_;
    std::atomic<bool>               stop_;
};

} // namespace latch
