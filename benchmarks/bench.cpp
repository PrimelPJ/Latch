/**
 * benchmarks/bench.cpp
 *
 * Throughput benchmarks for latch primitives.
 * Run with:   ./bench [--quick]
 *
 * Measurements reported as operations/second (higher is better).
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <vector>

#include "latch/latch.hpp"

/* ====================================================================== */
/*  Timing utility                                                          */
/* ====================================================================== */

using Clock = std::chrono::steady_clock;
using ns    = std::chrono::nanoseconds;

template <typename Fn>
static double measure_ns(Fn&& fn) {
    auto t0 = Clock::now();
    fn();
    auto t1 = Clock::now();
    return static_cast<double>(
        std::chrono::duration_cast<ns>(t1 - t0).count());
}

static void print_row(const char* label, long long ops, double ns_total) {
    double ops_per_s = static_cast<double>(ops) / (ns_total / 1e9);
    std::printf("  %-45s  %8.2f M ops/s\n", label, ops_per_s / 1e6);
}

/* ====================================================================== */
/*  Baseline: mutex-protected std::queue                                    */
/* ====================================================================== */

template <typename T>
class MutexQueue {
public:
    void push(T v) {
        std::lock_guard<std::mutex> lg(mu_);
        q_.push(std::move(v));
    }
    bool pop(T& out) {
        std::lock_guard<std::mutex> lg(mu_);
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop();
        return true;
    }
private:
    std::mutex   mu_;
    std::queue<T> q_;
};

/* ====================================================================== */
/*  MPMC vs Mutex: multi-producer multi-consumer throughput                 */
/* ====================================================================== */

template <int NProducers, int NConsumers, long long ItemsPerProducer>
static void bench_mpmc_vs_mutex(bool quick) {
    const long long items = quick ? ItemsPerProducer / 10 : ItemsPerProducer;
    const long long total = items * NProducers;

    // --- latch::MPMCQueue ---
    {
        latch::MPMCQueue<long long, 4096> q;
        std::atomic<long long> produced{0};
        std::vector<std::thread> ts;

        double elapsed = measure_ns([&]() {
            for (int p = 0; p < NProducers; ++p)
                ts.emplace_back([&, p]() {
                    for (long long i = 0; i < items; ++i)
                        while (!q.try_enqueue(p * items + i))
                            std::this_thread::yield();
                    produced.fetch_add(items, std::memory_order_relaxed);
                });

            for (int c = 0; c < NConsumers; ++c)
                ts.emplace_back([&]() {
                    long long n = total / NConsumers;
                    while (n > 0) {
                        if (q.try_dequeue()) --n;
                        else std::this_thread::yield();
                    }
                });

            for (auto& t : ts) t.join();
        });

        char label[80];
        std::snprintf(label, sizeof(label),
                      "MPMCQueue  %dP/%dC", NProducers, NConsumers);
        print_row(label, total, elapsed);
    }

    // --- MutexQueue baseline ---
    {
        MutexQueue<long long> q;
        std::vector<std::thread> ts;

        double elapsed = measure_ns([&]() {
            for (int p = 0; p < NProducers; ++p)
                ts.emplace_back([&, p]() {
                    for (long long i = 0; i < items; ++i)
                        q.push(p * items + i);
                });

            for (int c = 0; c < NConsumers; ++c)
                ts.emplace_back([&]() {
                    long long n = total / NConsumers;
                    while (n > 0) {
                        long long v;
                        if (q.pop(v)) --n;
                        else std::this_thread::yield();
                    }
                });

            for (auto& t : ts) t.join();
        });

        char label[80];
        std::snprintf(label, sizeof(label),
                      "MutexQueue %dP/%dC", NProducers, NConsumers);
        print_row(label, total, elapsed);
    }
}

/* ====================================================================== */
/*  SPSC throughput                                                         */
/* ====================================================================== */

static void bench_spsc(bool quick) {
    const long long items = quick ? 500'000LL : 5'000'000LL;

    // latch::SPSCQueue
    {
        latch::SPSCQueue<long long, 4096> q;
        std::atomic<long long> sum{0};

        double elapsed = measure_ns([&]() {
            std::thread prod([&]() {
                for (long long i = 0; i < items; ++i)
                    while (!q.try_enqueue(i))
                        std::this_thread::yield();
            });
            std::thread cons([&]() {
                long long n = items;
                while (n > 0) {
                    if (auto v = q.try_dequeue()) { sum += *v; --n; }
                    else std::this_thread::yield();
                }
            });
            prod.join();
            cons.join();
        });

        print_row("SPSCQueue  1P/1C", items, elapsed);
    }

    // Mutex baseline 1P/1C
    {
        MutexQueue<long long> q;
        std::atomic<long long> sum{0};

        double elapsed = measure_ns([&]() {
            std::thread prod([&]() {
                for (long long i = 0; i < items; ++i)
                    q.push(i);
            });
            std::thread cons([&]() {
                long long n = items;
                while (n > 0) {
                    long long v;
                    if (q.pop(v)) { sum += v; --n; }
                    else std::this_thread::yield();
                }
            });
            prod.join();
            cons.join();
        });

        print_row("MutexQueue 1P/1C", items, elapsed);
    }
}

/* ====================================================================== */
/*  Spinlock vs std::mutex on a shared counter                             */
/* ====================================================================== */

static void bench_spinlock_vs_mutex(bool quick) {
    const int threads = 8;
    const long long iters = quick ? 100'000LL : 1'000'000LL;
    const long long total = threads * iters;

    // Spinlock
    {
        latch::Spinlock lock;
        long long counter = 0;
        std::vector<std::thread> ts;

        double elapsed = measure_ns([&]() {
            for (int i = 0; i < threads; ++i)
                ts.emplace_back([&]() {
                    for (long long j = 0; j < iters; ++j) {
                        std::lock_guard<latch::Spinlock> lg(lock);
                        ++counter;
                    }
                });
            for (auto& t : ts) t.join();
        });

        print_row("Spinlock  (8 threads, contested)", total, elapsed);
    }

    // std::mutex
    {
        std::mutex mu;
        long long counter = 0;
        std::vector<std::thread> ts;

        double elapsed = measure_ns([&]() {
            for (int i = 0; i < threads; ++i)
                ts.emplace_back([&]() {
                    for (long long j = 0; j < iters; ++j) {
                        std::lock_guard<std::mutex> lg(mu);
                        ++counter;
                    }
                });
            for (auto& t : ts) t.join();
        });

        print_row("std::mutex (8 threads, contested)", total, elapsed);
    }
}

/* ====================================================================== */
/*  ThreadPool task throughput                                              */
/* ====================================================================== */

static void bench_thread_pool(bool quick) {
    const int tasks = quick ? 10'000 : 200'000;
    latch::ThreadPool pool(std::thread::hardware_concurrency());

    std::atomic<long long> done{0};
    std::vector<std::future<void>> futs;
    futs.reserve(tasks);

    double elapsed = measure_ns([&]() {
        for (int i = 0; i < tasks; ++i)
            futs.push_back(pool.submit([&done]() {
                done.fetch_add(1, std::memory_order_relaxed);
            }));
        for (auto& f : futs) f.get();
    });

    char label[80];
    std::snprintf(label, sizeof(label),
                  "ThreadPool (%zu workers, lightweight tasks)",
                  pool.thread_count());
    print_row(label, tasks, elapsed);
}

/* ====================================================================== */
/*  Seqlock read throughput                                                 */
/* ====================================================================== */

static void bench_seqlock(bool quick) {
    struct alignas(64) Payload { long long a, b, c, d; };

    const long long reads = quick ? 2'000'000LL : 20'000'000LL;
    const int readers = 4;

    // Seqlock
    {
        latch::Seqlock<Payload> sl(Payload{1, 1, 1, 1});

        std::atomic<bool> stop{false};
        std::thread writer([&]() {
            long long v = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                ++v;
                sl.write(Payload{v, v, v, v});
            }
        });

        std::atomic<long long> total_reads{0};
        std::vector<std::thread> ts;

        double elapsed = measure_ns([&]() {
            for (int r = 0; r < readers; ++r)
                ts.emplace_back([&]() {
                    long long n = reads;
                    while (n--) {
                        volatile auto p = sl.read();
                        (void)p;
                    }
                    total_reads.fetch_add(reads, std::memory_order_relaxed);
                });
            for (auto& t : ts) t.join();
            stop.store(true, std::memory_order_release);
        });

        writer.join();
        char label[80];
        std::snprintf(label, sizeof(label),
                      "Seqlock    (%d readers, 1 writer)", readers);
        print_row(label, total_reads.load(), elapsed);
    }

    // std::mutex baseline
    {
        Payload data{1, 1, 1, 1};
        std::mutex mu;

        std::atomic<bool> stop{false};
        std::thread writer([&]() {
            long long v = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                ++v;
                std::lock_guard<std::mutex> lg(mu);
                data = Payload{v, v, v, v};
            }
        });

        std::atomic<long long> total_reads{0};
        std::vector<std::thread> ts;

        double elapsed = measure_ns([&]() {
            for (int r = 0; r < readers; ++r)
                ts.emplace_back([&]() {
                    long long n = reads;
                    while (n--) {
                        std::lock_guard<std::mutex> lg(mu);
                        volatile auto p = data;
                        (void)p;
                    }
                    total_reads.fetch_add(reads, std::memory_order_relaxed);
                });
            for (auto& t : ts) t.join();
            stop.store(true, std::memory_order_release);
        });

        writer.join();
        char label[80];
        std::snprintf(label, sizeof(label),
                      "std::mutex (%d readers, 1 writer)", readers);
        print_row(label, total_reads.load(), elapsed);
    }
}

/* ====================================================================== */
/*  Entry point                                                             */
/* ====================================================================== */

int main(int argc, char** argv) {
    bool quick = (argc > 1 && std::string(argv[1]) == "--quick");

    std::printf("=== latch benchmarks (%s mode) ===\n",
                quick ? "quick" : "full");
    std::printf("  Hardware threads available: %u\n",
                std::thread::hardware_concurrency());
    std::printf("  All numbers: millions of operations per second (M ops/s)\n\n");

    std::printf("-- MPMC vs Mutex queue throughput --\n");
    bench_mpmc_vs_mutex<1, 1, 500'000>(quick);
    bench_mpmc_vs_mutex<2, 2, 500'000>(quick);
    bench_mpmc_vs_mutex<4, 4, 500'000>(quick);
    bench_mpmc_vs_mutex<8, 8, 200'000>(quick);

    std::printf("\n-- SPSC throughput --\n");
    bench_spsc(quick);

    std::printf("\n-- Spinlock vs std::mutex --\n");
    bench_spinlock_vs_mutex(quick);

    std::printf("\n-- ThreadPool task dispatch --\n");
    bench_thread_pool(quick);

    std::printf("\n-- Seqlock read throughput --\n");
    bench_seqlock(quick);

    std::printf("\n=== done ===\n");
    return 0;
}
