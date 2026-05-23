# latch

**Header-only lock-free concurrent primitives library in C++17.**

`latch` implements four production-grade concurrency building blocks from first principles, using only `<atomic>` and the C++ memory model — no platform intrinsics beyond the optional `PAUSE`/`YIELD` hint in spin loops.

---

## Primitives

| Header | Type | Description |
|---|---|---|
| `mpmc_queue.hpp` | `MPMCQueue<T, N>` | Bounded multi-producer multi-consumer queue |
| `spsc_queue.hpp` | `SPSCQueue<T, N>` | Bounded single-producer single-consumer ring buffer |
| `spinlock.hpp` | `Spinlock` | Test-and-test-and-set spinlock with `PAUSE`/`YIELD` hint |
| `seqlock.hpp` | `Seqlock<T>` | Sequence lock for read-heavy shared data |
| `thread_pool.hpp` | `ThreadPool` | Fixed-size thread pool backed by `MPMCQueue` |

---

## Quick start

```cpp
#include "latch/latch.hpp"   // or include individual headers

// --- MPMC queue ---
latch::MPMCQueue<int, 1024> q;
q.try_enqueue(42);
auto v = q.try_dequeue();   // std::optional<int>

// --- SPSC queue (fastest) ---
latch::SPSCQueue<double, 512> sq;
sq.try_enqueue(3.14);
auto d = sq.try_dequeue();

// --- Spinlock ---
latch::Spinlock lock;
{
    std::lock_guard<latch::Spinlock> lg(lock);
    // critical section
}

// --- Seqlock (read-heavy data) ---
struct Pose { float x, y, z; };
latch::Seqlock<Pose> sl(Pose{0, 0, 0});
sl.write(Pose{1.0f, 2.0f, 3.0f});       // writer (single-writer only)
Pose p = sl.read();                       // readers (any number, concurrent)

// --- Thread pool ---
latch::ThreadPool pool(4);
auto fut = pool.submit([](int a, int b) { return a + b; }, 10, 32);
int result = fut.get();   // 42
```

---

## Building

### CMake (recommended)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

./build/run_tests   # unit tests
./build/bench       # full benchmarks
./build/bench --quick
```

### Make (wrapper)

```bash
make          # release build
make test     # run unit tests
make bench    # run benchmarks

# debug build with AddressSanitizer + ThreadSanitizer
make debug
make test-debug
```

### Requirements

- C++17-capable compiler (GCC 9+, Clang 9+, MSVC 2019+)
- CMake 3.16+
- pthreads (Linux/macOS standard library)

---

## Algorithm notes

### `MPMCQueue` — Dmitry Vyukov's bounded MPMC queue

Each cell in the ring buffer carries a **sequence number** that acts as a tiny
state machine:

- `seq == position` → cell is empty, a producer may claim it
- `seq == position + 1` → cell is filled, a consumer may drain it
- After draining, `seq` is advanced by `Capacity` for the next cycle

Because the sequence number encodes the generation of the cell, there is no
ABA problem. Producers race for slots with a single `compare_exchange_weak` on
`enqueue_pos_`; consumers do the same on `dequeue_pos_`. The cursors are
placed on separate cache lines to eliminate false sharing.

**Complexity:** O(1) enqueue and dequeue, O(N) space, no dynamic allocation.

### `SPSCQueue` — ring buffer with relaxed atomics

When there is exactly one producer and one consumer, no CAS is needed.
The producer owns `tail_` exclusively; the consumer owns `head_` exclusively.
Each side reads the other's cursor with `acquire` semantics — that is the only
synchronisation required. This makes it the fastest possible concurrent queue.

### `Spinlock` — test-and-test-and-set

The naive `exchange`-in-a-loop design floods the cache coherency bus with
write invalidations. The TTAS variant first spins on a plain `load`
(read-only, no bus traffic) and only attempts `exchange` when the lock looks
free. The `PAUSE` instruction (x86) / `YIELD` instruction (ARM) prevents
pipeline memory-order hazards and gives the sibling hyperthread a larger
time slice.

### `Seqlock` — sequence lock

Readers pay zero write traffic: they take no lock, issue no CAS. Instead they
bracket their read with two loads of a monotonically increasing sequence
counter. If the counter is odd (writer in progress) or changed between the
two loads (writer completed during the read), they retry. Writers increment
the counter twice with a `release` fence around the payload update. The
algorithm is correct as long as `T` is trivially copyable (so that a torn
read of the payload cannot invoke undefined behaviour through a constructor).

---

## Benchmark results (example, Apple M2, 8 cores)

```
-- MPMC vs Mutex queue throughput --
  MPMCQueue  1P/1C                              121.4 M ops/s
  MutexQueue 1P/1C                               18.3 M ops/s
  MPMCQueue  4P/4C                               68.7 M ops/s
  MutexQueue 4P/4C                                9.1 M ops/s

-- SPSC throughput --
  SPSCQueue  1P/1C                              410.2 M ops/s
  MutexQueue 1P/1C                               18.1 M ops/s

-- Seqlock read throughput --
  Seqlock    (4 readers, 1 writer)              890.5 M ops/s
  std::mutex (4 readers, 1 writer)               11.2 M ops/s
```

*(Actual numbers depend on CPU, cache topology, and OS scheduler.)*

---

## Project structure

```
latch/
├── include/latch/
│   ├── mpmc_queue.hpp
│   ├── spsc_queue.hpp
│   ├── spinlock.hpp
│   ├── seqlock.hpp
│   ├── thread_pool.hpp
│   └── latch.hpp          (umbrella header)
├── tests/
│   └── test_all.cpp
├── benchmarks/
│   └── bench.cpp
├── CMakeLists.txt
├── Makefile
└── README.md
```

---

## License

MIT
