# allocators

A small **memory-allocation toolkit** for C++20: an STL-compatible allocator
template, a deque-backed memory pool, and an optional global `new` / `delete`
override that accounts every byte. Extracted from
[Xapiand](https://github.com/Kronuz/Xapiand).

## What it is

The header `allocators.h` gives you three things:

- **`allocator<T, Backend>`** — a standard-conforming allocator you can hand to
  any STL container. It forwards allocation to a static `Backend`. Two backends
  ship: `vanilla_allocator` (plain `malloc` / `free` with `new_handler`
  retries) and `tracked_allocator` (the byte-accounting backend).
- **`memory_pool_allocator<T>`** — a single-element pool. It hands out one `T`
  at a time from a `std::deque`-backed buffer and recycles freed slots through a
  LIFO free list. It is what you reach for behind a node-based container
  (`std::list`, `std::map`) where every allocation is exactly one element.
- **`total_allocated()` / `local_allocated()`** — process-wide and per-thread
  byte counters. They report live tracked bytes when the library is built with
  tracking on, and a constant `0` otherwise.

When tracking is enabled, the library also overrides the global `operator new` /
`operator delete` (and the array and `nothrow` forms) so that *every* heap
allocation in the program, not just the ones going through these allocators, is
counted. Each tracked block carries a small size header so `delete` knows how
many bytes to subtract.

Tracking is **off by default** and is a build-time choice (`malloc`/`free`
overhead with no accounting in the default build). Turn it on only when you want
the global accounting; overriding global `new`/`delete` is a program-wide
decision and shouldn't be made by a library you merely link.

## Install

This is not header-only. It ships a compiled translation unit, `allocators.cc`,
plus the header `allocators.h`. You build and link the `.cc`; the header alone
only declares the allocators and the accounting accessors. It requires C++20 and
has no third-party dependencies.

CMake with `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(
  allocators
  GIT_REPOSITORY https://github.com/Kronuz/allocators.git
  GIT_TAG        main
)
# Turn on byte accounting + global new/delete override (default OFF):
set(ALLOCATORS_TRACKED_MEM ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(allocators)

target_link_libraries(your_target PRIVATE allocators::allocators)
```

`CMakeLists.txt` requests `cxx_std_20` `PUBLIC` and puts the header on your
include path, so:

```cpp
#include "allocators.h"
```

The header keeps its original filename, so a codebase that already
`#include "allocators.h"` just needs this repo on its include path.

## Usage

```cpp
#include <vector>
#include "allocators.h"

// An STL container backed by the plain malloc/free allocator.
std::vector<int, allocators::allocator<int, allocators::vanilla_allocator>> v;
v.push_back(42);

// The same template over the tracking backend.
std::vector<int, allocators::allocator<int, allocators::tracked_allocator>> t;

// One-element pool, the backend for a node-based container.
allocators::memory_pool_allocator<long> pool;

// Process-wide and per-thread live bytes (0 unless built with tracking on).
long long total = allocators::total_allocated();
long long mine  = allocators::local_allocated();
```

With tracking on, the global override means even untyped allocations move the
counters:

```cpp
long long before = allocators::total_allocated();
auto* p = new char[1024];
// allocators::total_allocated() - before >= 1024 here
delete[] p;
// ...and total_allocated() is back to `before`
```

## Tracking: the build option

`ALLOCATORS_TRACKED_MEM` (CMake option, default `OFF`) selects the build:

- **OFF** — `vanilla_allocator` and `memory_pool_allocator` work as usual,
  `tracked_allocator` is a plain `malloc`/`free` passthrough, the global
  `new`/`delete` are *not* overridden, and `total_allocated()` /
  `local_allocated()` return `0`.
- **ON** — the global `operator new` / `delete` are overridden to account every
  byte, `tracked_allocator` stamps a size header on each block, and the counters
  report live tracked bytes.

The option defines the macro `XAPIAND_TRACKED_MEM` in a generated
`allocators_config.h` (the macro name is kept verbatim from Xapiand). A second
generated macro, `HAS_THREAD_LOCAL`, decides whether the per-thread counter uses
`thread_local` or a mutex-guarded map; CMake probes for it, and C++20 always has
it. See [ARCHITECTURE.md](ARCHITECTURE.md) for the accounting internals.

## Build & test

```sh
cmake -B build -DALLOCATORS_TRACKED_MEM=ON && cmake --build build && ctest --test-dir build
```

No network is needed; there are no dependencies to fetch. The test detects at
runtime whether the build tracks memory and asserts the right contract for that
mode, so the same `test/test.cc` covers both `ON` and the default `OFF` build.
It exercises the counters moving with `new`/`delete`, `allocator<int, Backend>`
driving a `std::vector` over both backends, and the `memory_pool_allocator`
recycling freed blocks. It prints `all allocators tests passed` and exits 0.

## Provenance

Extracted from [Xapiand](https://github.com/Kronuz/Xapiand). `allocators.h` is
verbatim; `allocators.cc` is nearly so. The extraction severed its one
coupling — the `#include "config.h"` it used for the `XAPIAND_TRACKED_MEM` and
`HAS_THREAD_LOCAL` macros — and replaced it with the library's own generated
`allocators_config.h`, following the sibling `times` library's pattern. Two
small source deltas were needed to make the API actually linkable standalone;
see [AGENTS.md](AGENTS.md) for the exact list. There is no `log.h`, `opts`, or
`THROW` coupling.

## License

MIT, Copyright (c) 2015-2019 Dubalu LLC. The header also carries a BSD-2-Clause
notice from moya-lang.org for the memory-pool allocator. See [LICENSE](LICENSE).
