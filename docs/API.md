# allocators API reference

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
it. See [ARCHITECTURE.md](../ARCHITECTURE.md) for the accounting internals.

## Explicit resource targets

Link `allocators::resources` for the headers under `allocators/`. `ByteAccount::admit` returns a move-only owning `ByteCharge`; `share()` captures the same counter state without allocating. `ChargedAllocator`, `ChargedMemoryResource`, `PagedStorage` and `OwnedBuffer` admit full upstream capacity. `AllocationContext`/`OwnedAllocator` retain PMR lifetime, including weak shared controls.

`ReservationBook<N>` uses `ResourceVector<N>` dimensions. Its operations are `try_reserve(peak, spendable)`, `try_grow(reservation, minimum_peak, spendable_delta)`, `cancel`, `settle(reservation, additions, proved_credit)` and `reclaim(proved_credit)`. Construction accepts hard capacity and an optional complete census, including over-quota debt. Accessors return committed use, reserved use, observed peak and abandonment. Operations belong to one executor; the book outlives every capability. Failed growth changes nothing. Unresolved destruction preserves debt and prevents further admission. A reservation capability can also `abandon()` explicitly; this preserves its peak metadata and clears its reference to the book after recording abandonment.

Link optional `allocators::retirement` for caller-owned `RetirementRuntime`, `RetirementDomain`, `RetirementFactory` and intrusive `RetirementHandle<T>`. A payload derives from `RetirementNode` and may implement bounded `retire_step()`/`retry_later()` cleanup. Close admission, release all accepted originals and handles, then wait for the drain before shutting down the runtime.
