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

// One-element pool, the backend for a node-based container. Copies and rebinds share
// the pool (A<T>(A<U>(a)) == a); it is single-threaded: one thread at a time per pool.
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

## Construction forwards its arguments

`construct(p, args...)` of both allocators perfect-forwards to the element constructor, so a
container relocating through `allocator_traits::construct(a, p, std::move(x))` moves, and a
throwing move falls back to a copy, exactly as with `std::allocator`. `noexcept` follows the
element constructor.

`test/construct.cc` compares copy, move and destroy counts with `std::allocator` for `vector`,
`deque`, `list`, `map`, `unordered_map` and a type whose copies and moves throw.
`allocators_bench` times `vector<int>` and `vector<std::string>` `push_back` against
`std::allocator` (A/B/A, best of 11); `allocators_bench --counts` prints only the counts.
[perf/construct-forwarding.md](perf/construct-forwarding.md) has the timings, the mutants and
the platforms.

## The memory pool: identity and threads

All copies and rebinds of a `memory_pool_allocator` share one pool, so
`A<T>(A<U>(a)) == a` and allocators of one pool compare equal whatever their value types. The
converting constructor never allocates. The pool hands out one element at a time: `allocate(n)`
with `n != 1` throws `std::bad_alloc`, so array containers (`vector`, `deque`, the bucket array
of `unordered_map`) cannot use it.

A container *copy* gets its own pool: copy construction takes a new one
(`select_on_container_copy_construction`), and copy assignment keeps the target's allocator
(`propagate_on_container_copy_assignment` is `false_type`), so the copied elements are
allocated from the target's pool. Move assignment and swap hand the nodes over together with
the pool (`propagate_on_container_move_assignment` and `_swap` are `true_type`), and
`is_always_equal` is `false_type`.

A move copies the allocator, so a moved-from container keeps sharing the pool with the
container that took its nodes. Use the pair as one unit, or give the moved-from one a pool of
its own before another thread touches either: move-assign a fresh container to it (`a = C{}`)
or swap it with one. Copy assignment (`a = fresh`), `a = {}` and `clear()` look like resets and
are not: the container keeps the shared pool. Moving a container to another thread is safe
when its source is not used again or is re-pooled first.

Containers with separate pools compare unequal, such as a copy and its source (including after
a copy assignment, which keeps the target's pool). `splice` and `merge` between them are not
allowed, because the standard requires equal allocators. Containers that share a pool, such as
a moved-from container and the one that took its nodes, can splice and merge.

**A pool is not thread-safe**: use all allocators of one pool from one thread at a time,
with a happens-before edge at each hand-over. Copying a container to another thread is
safe (the copy is independent); sharing an allocator, or using a container from two threads,
is not. Without `NDEBUG` (or with `ALLOCATORS_POOL_CHECK`) overlapping use is detected and
reported through `allocators::set_pool_misuse_handler` (abort by default).

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

The resource target neither builds nor links the allocator backends or the global allocation
overrides. Disable the retirement target with `ALLOCATORS_BUILD_RETIREMENT=OFF` when only
accounting is needed. The compiled targets follow `BUILD_SHARED_LIBS`.

```cpp
#include <allocators/memory_resource.h>
#include <allocators/allocation.h>

// Charge the complete upstream allocation, including headers and alignment.
auto account = std::make_shared<allocators::ByteAccount>(1024 * 1024);
auto resource = allocators::make_charged_resource(account);
auto value = std::allocate_shared<int>(
    allocators::OwnedAllocator<int>(allocators::AllocationContext(resource)), 42);
```

A `ByteCharge` retains the account's counter state, including its ancestors, after the
initiating facade is destroyed. Successful admission and release allocate no bookkeeping
storage; constructing an independent account allocates its counter state once, outside its own
payload quota. Pressure rolls back all ancestor charges before throwing `ResourcePressure`.
`PagedStorage` also retains state and can continue allocating after the initiating facade is
gone.

```cpp
#include <allocators/resources.h>

// Reserve simultaneous peak effects before beginning an external operation.
using Vector = allocators::ResourceVector<2>;
allocators::ReservationBook<2> book(Vector{{1000, 10}}, Vector{{200, 2}});
auto reservation = book.try_reserve(Vector{{300, 3}}, Vector{{800, 8}});
if (reservation) {
    book.settle(*reservation, Vector{{250, 2}}, Vector{{100, 1}});
}
```

Reservation books belong to one executor and outlive their move-only capabilities. Destroying
an unresolved reservation preserves its debt and marks abandonment; only explicit cancellation
or settlement releases reserved capacity. The caller proves reclaim credits and enforces
protected pools, original-I/O ownership and durable deletion. Those policies are outside the
shared arithmetic.

```cpp
#include <allocators/retirement.h>

// Final owner release enqueues cleanup; the application owns shutdown.
auto baseline = std::make_shared<allocators::ByteAccount>(1024 * 1024);
auto objects = std::make_shared<allocators::ByteAccount>(1024 * 1024);
allocators::RetirementRuntime runtime(baseline);
auto domain = runtime.domain(objects);
auto drain = domain.close();
drain.wait();
runtime.shutdown();
```

Retirement handles release through an intrusive queue. Payload destruction and control-block
release run on the fixed cleanup worker. Keep the runtime alive until every domain drains, and
retain accepted I/O owners until authentic completion before retiring them.
