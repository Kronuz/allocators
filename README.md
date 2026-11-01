# allocators

Part of [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries), a collection of small, reusable C++ libraries.

[![CI](https://github.com/Kronuz/allocators/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/allocators/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)

Allocation toolkit for C++20: STL allocators, a node pool, byte-budgeted memory resources and
opt-in heap tracking. The `allocators::allocators` target gives containers a `malloc` or a
byte-counting backend and a pool that hands out one node at a time, and can override the global
`new` and `delete` to count every heap byte in the process. The header-only
`allocators::resources` target charges allocations to nested byte accounts and reserves
capacity before an operation runs. Reach for it to see where memory goes, to feed node-heavy
containers, or to hold a service to a memory budget.

## ✨ Highlights

- **`allocator<T, Backend>`.** A standard allocator over `vanilla_allocator` (`malloc` and
  `free`) or `tracked_allocator`, which stamps a size header on each block and counts it.
- **`memory_pool_allocator<T>`.** `std::list`, `std::map` and other node containers take one
  element at a time from a pool that reuses freed slots; every copy and rebind shares the pool.
- **Whole-process accounting, opt in.** `ALLOCATORS_TRACKED_MEM=ON` overrides the global
  `new` and `delete`; `total_allocated()` and `local_allocated()` report live bytes for the
  process and for the calling thread.
- **Byte accounts.** `make_charged_resource` charges each full upstream allocation, headers and
  alignment included, to a `ByteAccount` and its parents (up to eight levels), and throws
  `ResourcePressure` past a limit after rolling every level back.
- **Reservation books.** `ReservationBook<N>` reserves peak use over N dimensions before an
  external operation and settles the real use after it.
- **Deferred cleanup.** The optional `allocators::retirement` target (POSIX) runs final
  destruction on a cleanup worker the caller owns.
- **No dependencies.** Standard library only.

## 🚀 Quick start

```cpp
#include "allocators.h"

#include <cstdio>
#include <list>
#include <vector>

template <typename T>
using tracked = allocators::allocator<T, allocators::tracked_allocator>;

int main() {
	// A node container that takes one element at a time from a pool of reused slots.
	std::list<int, allocators::memory_pool_allocator<int>> nodes{1, 2, 3};

	// Live heap bytes, counted when the build sets ALLOCATORS_TRACKED_MEM=ON.
	long long before = allocators::total_allocated();
	std::vector<long long, tracked<long long>> samples;
	samples.reserve(1000);
	std::printf("%lld bytes\n", allocators::total_allocated() - before);   // 8000 bytes (0 untracked)
}
```

With tracking on, plain `new` and `std::allocator` move the counters too, for every
allocation the optimizer keeps. [`examples/demo.cc`](examples/demo.cc) shows the counters
and [`examples/resources.cc`](examples/resources.cc) the byte accounts.

## 📦 Installation

Requires a C++20 compiler and CMake 3.14 or newer, and nothing else:
`allocators::allocators` compiles one source file, `allocators::resources` is header-only, and
`allocators::retirement` needs POSIX threads. CI builds it on Linux (x86_64, arm64), macOS
(arm64, x86_64), and FreeBSD, plus ASan, UBSan, and TSan builds.

```cmake
include(FetchContent)
FetchContent_Declare(allocators
  GIT_REPOSITORY https://github.com/Kronuz/allocators.git
  GIT_TAG        f0dc466feff294bccea4c28d44c46677bbab9b9c)  # main tracks the tip
# set(ALLOCATORS_TRACKED_MEM ON CACHE BOOL "" FORCE)   # opt in to whole-process accounting
FetchContent_MakeAvailable(allocators)

target_link_libraries(my_app PRIVATE allocators::allocators)   # or allocators::resources, allocators::retirement
```

Include `"allocators.h"` for the allocators, the pool and the counters, and
`<allocators/resources.h>`, `<allocators/memory_resource.h>` or `<allocators/retirement.h>` for
the resource targets.

## 🎯 When to use it

It fits tests, benchmarks and debug builds that measure heap use, node containers that
allocate one element at a time, and services that admit memory against a byte budget.

Not for:

- **A faster general-purpose `malloc`.** Use jemalloc or mimalloc.
- **Tracking switched on by a library.** `ALLOCATORS_TRACKED_MEM` overrides the global
  `operator new` for the whole program, so it belongs in the executable's build.
- **One pool across threads.** A pool is not thread-safe: use all allocators of one pool from
  one thread at a time. Builds without `NDEBUG` detect overlapping use.
- **Array containers on the pool.** `memory_pool_allocator` allocates one element at a time and
  throws `std::bad_alloc` for `vector`, `deque` and the bucket array of `unordered_map`.

## ⚡ Performance

`allocators_compare_pmr` builds, works and destroys one container with each of four allocators and
times the whole run, creating the pool or resource included. The arms are `std::allocator`
(malloc, the baseline), `pool` (this library's `memory_pool_allocator`), `pmr pool`
(`std::pmr::unsynchronized_pool_resource`) and `pmr monotonic`
(`std::pmr::monotonic_buffer_resource`, which never frees, so only the list build rows have it).
The `std::pmr` arms are libc++ 20.1 from Apple clang 17.0.0. Every arm of a row computes the same
checksum, compared before timing. Each cell is the arm's time as a fraction of `std::allocator`'s
for that row (below 1 is faster than malloc), the median of 9 rounds, with a fresh pool or
resource per run; the baseline run twice differs from itself by at most 2%.

| Row | pool, M4 Pro | `pmr pool`, M4 Pro | pool, i9-9980HK | `pmr pool`, i9-9980HK |
|---|--:|--:|--:|--:|
| list build, 1,000 nodes | 0.68 | 0.50 | 0.65 | 0.24 |
| list build, 100,000 nodes | 0.59 | 0.34 | 0.66 | 0.27 |
| map churn, 1,000 nodes | 0.55 | 0.68 | 0.58 | 0.66 |
| map churn, 100,000 nodes | 0.82 | 0.99 | 0.70 | 0.78 |
| list steady, 1,000 nodes | 0.61 | 0.32 | 0.73 | 0.20 |
| list steady, 100,000 nodes | 0.56 | 0.29 | 0.72 | 0.20 |

The pool is ahead of `pmr pool` on map churn, behind it on list build and list steady. `pmr
monotonic` builds a list in 0.26 to 0.27 of malloc's time on the M4 Pro and 0.13 to 0.18 on the
i9. The list rows build, sum and destroy a `list<int>` of n nodes (steady adds 20n `push_back` and
`pop_front` pairs); the map rows insert n random keys into a `map<int, int>`, erase the n/2
smallest and insert n/2 more.

Apple M4 Pro (macOS 26.7) and Intel Core i9-9980HK (macOS 15.8), Release (`-O3 -DNDEBUG`) with
Apple clang 17.0.0:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DALLOCATORS_BUILD_BENCH=ON
cmake --build build --target allocators_compare_pmr && ./build/allocators_compare_pmr
```

[docs/perf/](docs/perf/) has the construction-forwarding measurements.

## 📚 Documentation

- [docs/API.md](docs/API.md): each allocator, the pool's sharing and thread rules, the tracking
  option, and the resource and retirement targets.
- [ARCHITECTURE.md](ARCHITECTURE.md): the size headers, the counters and the pool.
- [docs/perf/](docs/perf/): measurement method and results.
- [CHANGELOG.md](CHANGELOG.md): changes, newest first.
- [AGENTS.md](AGENTS.md): repository map, invariants and traps.
- [Catalog entry](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#allocators) in
  Kronuz C++ Libraries.

## 🛠️ Building and testing

```sh
cmake -B build && cmake --build build && ctest --test-dir build
cmake -B build-tracked -DALLOCATORS_TRACKED_MEM=ON && cmake --build build-tracked && ctest --test-dir build-tracked
cmake -B build-bench -DCMAKE_BUILD_TYPE=Release -DALLOCATORS_BUILD_BENCH=ON && cmake --build build-bench
```

The same tests check whichever contract the build has.

## 🤝 Contributing

Issues and pull requests are welcome. Read [AGENTS.md](AGENTS.md) and the catalog's
[rules of engagement](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#rules-of-engagement)
first; CI must pass on every platform.

## 📄 License

MIT, Copyright (c) 2026 Germán Méndez Bravo and (c) 2015-2020 Dubalu LLC; the memory pool in
`allocators.h` carries a BSD-2-Clause notice from moya-lang.org. See [LICENSE](LICENSE).
Extracted from [Xapiand](https://github.com/Kronuz/Xapiand).
