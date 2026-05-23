/**
 * tests/test_all.cpp
 *
 * Runs correctness tests for every latch primitive.
 * A minimal hand-rolled test runner is used to keep the project
 * dependency-free; switch to GoogleTest / Catch2 if you want more.
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <numeric>
#include <set>
#include <thread>
#include <vector>

#include "latch/latch.hpp"

/* ====================================================================== */
/*  Tiny test runner                                                        */
/* ====================================================================== */

static int g_total = 0, g_passed = 0, g_failed = 0;

#define ASSERT(expr)                                                           \
    do {                                                                       \
        ++g_total;                                                             \
        if (expr) {                                                            \
            ++g_passed;                                                        \
        } else {                                                               \
            ++g_failed;                                                        \
            std::fprintf(stderr, "  FAIL  %s:%d  (%s)\n",                     \
                         __FILE__, __LINE__, #expr);                           \
        }                                                                      \
    } while (0)

static void section(const char* name) {
    std::printf("\n[%s]\n", name);
}

/* ====================================================================== */
/*  MPMCQueue tests                                                         */
/* ====================================================================== */

static void test_mpmc_basic() {
    section("MPMCQueue — basic");

    latch::MPMCQueue<int, 4> q;

    // Empty dequeue returns nullopt.
    ASSERT(!q.try_dequeue().has_value());

    // Enqueue 3 items (capacity is 4, so all should succeed).
    ASSERT(q.try_enqueue(10));
    ASSERT(q.try_enqueue(20));
    ASSERT(q.try_enqueue(30));

    // Approximate size.
    ASSERT(q.size_approx() == 3);

    // Dequeue in FIFO order.
    ASSERT(q.try_dequeue().value() == 10);
    ASSERT(q.try_dequeue().value() == 20);
    ASSERT(q.try_dequeue().value() == 30);

    // Queue is now empty.
    ASSERT(!q.try_dequeue().has_value());
}

static void test_mpmc_full() {
    section("MPMCQueue — full queue");
    latch::MPMCQueue<int, 4> q;

    // Fill all 4 slots (capacity = 4, but the algorithm needs at least one
    // sentinel, so the effective capacity is Capacity - 1 == 3... wait,
    // Vyukov's algorithm: Capacity slots, each can hold one item.
    // Let's measure empirically.
    int inserted = 0;
    while (q.try_enqueue(inserted)) ++inserted;
    std::printf("  effective capacity: %d (template arg: %zu)\n",
                inserted, q.capacity());
    // The queue should accept exactly Capacity items before returning false.
    // (Vyukov's ring: when enqueue_pos wraps and meets dequeue_pos — the
    //  slot's sequence reveals fullness, not a simple pointer comparison.)
    ASSERT(inserted == static_cast<int>(q.capacity()));
}

static void test_mpmc_concurrent() {
    section("MPMCQueue — concurrent producers and consumers");

    // 4 producers, 4 consumers, 10 000 items each.
    constexpr int kPerProducer = 10'000;
    constexpr int kProducers   = 4;
    constexpr int kConsumers   = 4;
    constexpr int kTotal       = kPerProducer * kProducers;

    latch::MPMCQueue<int, 2048> q;
    std::atomic<int> produced{0}, consumed{0};
    std::atomic<long long> checksum_prod{0}, checksum_cons{0};

    auto producer = [&](int id) {
        for (int i = 0; i < kPerProducer; ++i) {
            int val = id * kPerProducer + i;
            while (!q.try_enqueue(val))
                std::this_thread::yield();
            produced.fetch_add(1, std::memory_order_relaxed);
            checksum_prod.fetch_add(val, std::memory_order_relaxed);
        }
    };

    auto consumer = [&]() {
        int local_count = 0;
        while (local_count < kTotal / kConsumers) {
            if (auto v = q.try_dequeue()) {
                checksum_cons.fetch_add(*v, std::memory_order_relaxed);
                ++local_count;
                consumed.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kProducers; ++i) threads.emplace_back(producer, i);
    for (int i = 0; i < kConsumers; ++i) threads.emplace_back(consumer);
    for (auto& t : threads) t.join();

    ASSERT(produced.load() == kTotal);
    ASSERT(consumed.load() == kTotal);
    ASSERT(checksum_prod.load() == checksum_cons.load());
}

/* ====================================================================== */
/*  SPSCQueue tests                                                         */
/* ====================================================================== */

static void test_spsc_basic() {
    section("SPSCQueue — basic");
    latch::SPSCQueue<std::string, 8> q;

    ASSERT(q.empty());
    ASSERT(q.try_enqueue("hello"));
    ASSERT(q.try_enqueue("world"));
    ASSERT(!q.empty());

    auto v1 = q.try_dequeue();
    ASSERT(v1.has_value() && *v1 == "hello");
    auto v2 = q.try_dequeue();
    ASSERT(v2.has_value() && *v2 == "world");
    ASSERT(q.empty());
    ASSERT(!q.try_dequeue().has_value());
}

static void test_spsc_concurrent() {
    section("SPSCQueue — concurrent 1P/1C");
    constexpr int kItems = 100'000;
    latch::SPSCQueue<int, 1024> q;

    std::atomic<long long> sum_prod{0}, sum_cons{0};

    std::thread prod([&]() {
        for (int i = 0; i < kItems; ++i) {
            while (!q.try_enqueue(i))
                std::this_thread::yield();
            sum_prod.fetch_add(i, std::memory_order_relaxed);
        }
    });

    std::thread cons([&]() {
        for (int i = 0; i < kItems; ) {
            if (auto v = q.try_dequeue()) {
                sum_cons.fetch_add(*v, std::memory_order_relaxed);
                ++i;
            } else {
                std::this_thread::yield();
            }
        }
    });

    prod.join();
    cons.join();

    ASSERT(sum_prod.load() == sum_cons.load());
}

/* ====================================================================== */
/*  Spinlock tests                                                          */
/* ====================================================================== */

static void test_spinlock() {
    section("Spinlock — concurrent counter");

    latch::Spinlock lock;
    long long counter = 0;
    constexpr int kThreads = 8;
    constexpr int kIter    = 50'000;

    auto worker = [&]() {
        for (int i = 0; i < kIter; ++i) {
            std::lock_guard<latch::Spinlock> lg(lock);
            ++counter;
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) threads.emplace_back(worker);
    for (auto& t : threads) t.join();

    ASSERT(counter == static_cast<long long>(kThreads) * kIter);
}

/* ====================================================================== */
/*  Seqlock tests                                                           */
/* ====================================================================== */

struct Point { int x, y, z; };

static void test_seqlock() {
    section("Seqlock — read/write coherency");

    latch::Seqlock<Point> sl(Point{0, 0, 0});

    constexpr int kReaders  = 4;
    constexpr int kWrites   = 10'000;
    constexpr int kReadIter = 50'000;

    std::atomic<int> incoherent{0};

    // Writer thread: writes coherent points (x == y == z).
    std::thread writer([&]() {
        for (int i = 1; i <= kWrites; ++i) {
            sl.write(Point{i, i, i});
        }
    });

    // Reader threads: verify each read is coherent.
    auto reader_fn = [&]() {
        for (int i = 0; i < kReadIter; ++i) {
            Point p = sl.read();
            if (p.x != p.y || p.y != p.z)
                incoherent.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> readers;
    for (int i = 0; i < kReaders; ++i) readers.emplace_back(reader_fn);
    writer.join();
    for (auto& t : readers) t.join();

    ASSERT(incoherent.load() == 0);
}

/* ====================================================================== */
/*  ThreadPool tests                                                        */
/* ====================================================================== */

static void test_thread_pool_futures() {
    section("ThreadPool — futures and return values");

    latch::ThreadPool pool(4);

    auto f1 = pool.submit([]() { return 42; });
    auto f2 = pool.submit([](int a, int b) { return a + b; }, 10, 32);
    auto f3 = pool.submit([]() -> std::string { return "latch"; });

    ASSERT(f1.get() == 42);
    ASSERT(f2.get() == 42);
    ASSERT(f3.get() == std::string("latch"));
}

static void test_thread_pool_parallel_sum() {
    section("ThreadPool — parallel sum");

    latch::ThreadPool pool(std::thread::hardware_concurrency());
    constexpr int kChunks = 64;
    constexpr int kChunkSize = 1000;

    std::vector<std::future<long long>> futs;
    futs.reserve(kChunks);

    for (int c = 0; c < kChunks; ++c) {
        futs.push_back(pool.submit([c]() -> long long {
            long long s = 0;
            for (int i = c * kChunkSize; i < (c + 1) * kChunkSize; ++i)
                s += i;
            return s;
        }));
    }

    long long total = 0;
    for (auto& f : futs) total += f.get();

    long long expected = static_cast<long long>(kChunks * kChunkSize - 1) *
                         (kChunks * kChunkSize) / 2;
    ASSERT(total == expected);
}

/* ====================================================================== */
/*  Main                                                                    */
/* ====================================================================== */

int main() {
    std::printf("=== latch test suite ===\n");

    // MPMCQueue
    test_mpmc_basic();
    test_mpmc_full();
    test_mpmc_concurrent();

    // SPSCQueue
    test_spsc_basic();
    test_spsc_concurrent();

    // Spinlock
    test_spinlock();

    // Seqlock
    test_seqlock();

    // ThreadPool
    test_thread_pool_futures();
    test_thread_pool_parallel_sum();

    std::printf("\n=== Results: %d/%d passed", g_passed, g_total);
    if (g_failed)
        std::printf(", %d FAILED", g_failed);
    std::printf(" ===\n");

    return g_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
