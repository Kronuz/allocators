# Architecture

The internals of `allocators`: how the STL allocator template forwards to a
backend, how the byte accounting and the global `new` / `delete` override work,
and how the memory pool recycles slots. For usage see `README.md`; for the repo
map and invariants see `AGENTS.md`.

## Shape

A header of allocators and a single compiled translation unit:

```
  allocators.h    the allocator template, two backends, the memory pool, the accounting accessors
  allocators.cc   the backend definitions, the counters, and the global new/delete override
```

The header declares everything; `allocators.cc` defines the non-template parts:
the `malloc` / `free` wrappers, the `vanilla_allocator` and `tracked_allocator`
member functions, the counters, and (when tracking is on) the global operator
overrides. The two allocator templates (`allocator<T, Backend>` and
`memory_pool_allocator<T>`) are header-only; they are instantiated in the
consumer's translation unit.

## The allocator template

`allocator<T, Backend>` is a thin, stateless, standard-conforming allocator. Its
`allocate(n)` multiplies `n * sizeof(T)`, calls `Backend::allocate(bytes)`, and
casts the result; `deallocate` forwards to `Backend::deallocate`. Everything
else (`construct`, `destroy`, `address`, `max_size`, `rebind`) is the textbook
allocator boilerplate. Because it holds no state, any two instances compare
equal, so containers using it can move and swap freely.

The `Backend` is a static interface: two functions, `allocate(size_t)` and
`deallocate(void*)`, both `static`. Two backends ship.

`vanilla_allocator` is plain `malloc` / `free`, wrapped so that a failed
`malloc` runs the installed `new_handler` (and retries) before giving up with
`bad_alloc`, matching what the global `operator new` is required to do. A zero
size is bumped to one byte so every allocation returns a distinct pointer.

`tracked_allocator` is the accounting backend, described below.

## Byte accounting

Accounting is a build-time feature, gated on `XAPIAND_TRACKED_MEM`. With it off,
`tracked_allocator` is just a passthrough to the `malloc` / `free` wrappers, the
counters return `0`, and the global operators are left alone. The rest of this
section is the tracking-on build.

Two counters hold the live byte totals. The process-wide total is a
`std::atomic_llong` so concurrent allocations across threads stay consistent.
The per-thread total is a `thread_local long long` when `HAS_THREAD_LOCAL` is
set (the normal case on C++20); on a toolchain without usable `thread_local` it
falls back to a `std::unordered_map<thread::id, long long>` behind a mutex. The
map's own nodes are allocated through `allocator<..., vanilla_allocator>` so the
bookkeeping doesn't recurse back into the tracked path.

`tracked_allocator::allocate(sz)` over-allocates by `alignment` bytes
(`alignof(std::max_align_t)`), writes the requested size into that leading slot
as a header, bumps both counters by `sz`, and returns a pointer advanced past
the header. `deallocate` reverses it: step the pointer back by `alignment`, read
the stored size, subtract it from both counters, and `free` the original block.
The size header is what lets `delete` — which is handed only a bare pointer —
know how many bytes to subtract.

## The global new / delete override

When tracking is on, `allocators.cc` defines the global `operator new`,
`operator new[]`, their `nothrow` forms, and the matching `operator delete` /
`operator delete[]` (including the sized-delete overloads). Each one forwards to
`tracked_allocator::allocate` / `deallocate`. The effect is that *every* heap
allocation in the linked program is counted, not only the ones that go through
these allocators — which is the point: it gives a true process-wide live-bytes
figure. The cost is that every allocation carries the one-word size header and
an atomic increment.

This is why tracking is opt-in. Overriding global `new` / `delete` is a
whole-program decision, so a library that is merely linked must not impose it;
the build flag makes the choice explicit and the default leaves the program's
allocator untouched.

## The memory pool

`memory_pool_allocator<T>` is a different shape: a stateful, single-element
pool, not a thin forwarder. An allocator holds a `shared_ptr` to a **pool family**;
the family holds one *size class* per distinct block size (`sizeof(T)` rounded up to
`max_align_t`, at least a pointer). A size class is chunked storage (16 blocks,
doubling to about 64 KiB per chunk), a bump pointer into the newest chunk and a LIFO
free list. Chunks are never returned before the family dies.

**Identity.** Every copy and every rebind of an allocator holds the same family, so
`A<T>(A<U>(a)) == a`, any two allocators of one family compare equal whatever their
value types (`operator==` is defined across `T`), and blocks of one can be freed
through another. The converting constructor copies the handle only: `noexcept`, no
allocation (a size class is found on first `allocate`). A default-constructed
allocator starts a new family (one allocation). Containers copy and rebind their
allocator freely, which is why the pool lives in the shared family and not in the
allocator value.

**Propagation.** `propagate_on_container_copy_assignment`, `_move_assignment` and
`_swap` are `true_type` and `is_always_equal` is `false_type`: blocks are owned by
the family and travel with the allocator, so container swap and move-assign keep their
nodes (no element-wise fallback, no UB on swap). `select_on_container_copy_construction`
returns a **new** pool, so a copied container shares nothing with the original.

`allocate(1)` pops the free list, else bumps the current chunk, else adds a chunk. It
refuses any request other than exactly one element (`n != 1` throws `bad_alloc`),
because it is built for node-based containers. `deallocate` pushes the block on the free
list. The free list is LIFO, so the most recently freed block is the next one handed
out, good for cache locality in the node churn of a `std::list` or tree.

**Thread safety.** A pool is not thread-safe. All allocators of one family must be used
by one thread at a time, with a happens-before edge at each hand-over. Copying a
*container* across threads is safe (the copy has its own pool); copying an *allocator*,
or moving a container to another thread while the original is still in use, is not.
Builds without `NDEBUG`, or with `ALLOCATORS_POOL_CHECK`, detect overlapping use of one
size class and call the misuse handler (abort by default,
`allocators::set_pool_misuse_handler`); this finds overlap in time, not a missing
hand-over edge, so run such code under TSan as well.

## Why this shape

The library separates *what allocates* from *how it accounts*. The
`allocator<T, Backend>` template is a stable STL-facing shell; swapping
`vanilla_allocator` for `tracked_allocator` changes the bookkeeping without
touching the container code. The accounting lives entirely in the backend and
the optional global override, so a release build pays nothing for it. The memory
pool is a separate tool for the node-based-container case, where a general
allocator's per-call `malloc` is wasteful and a fixed-size recycling pool wins.

## Optional resource modules

The resource headers live under `include/allocators` and expose a separate interface target. They use no legacy backend, global override or tracking switch. The optional `retirement.cc` target owns its POSIX dependencies and fixed worker only when selected. The legacy target, its symbols and its tracking behavior do not depend on these modules.

`ByteAccount` owns a stable shared counter state. States retain immutable parent states; charges and page arenas retain state rather than borrowed facades. Successful admission uses bounded ancestor traversal, rolls back on pressure and records peaks only after successful complete admission. Account construction has a separate fixed allocation baseline; its own control block is not charged through itself. Charged allocation headers retain one owning charge, avoiding redundant facade ownership. Physical release precedes refund, and weak control blocks retain their charges until final release.

`ResourceVector<N>` implements checked componentwise arithmetic over unsigned 64-bit dimensions. `ReservationBook<N>` embeds nonmoving committed, reserved and peak counters. It retains complete over-quota census debt. Reserve and growth require both hard capacity and caller-authorized spendable capacity. Settlement computes all counters before mutation. An unresolved capability's destruction preserves reserved capacity and records abandonment. The book must outlive its capabilities; a domain wrapper retains that owner and supplies durable proofs.

`AllocationContext` and `OwnedAllocator` preserve an owning PMR resource through containers and weak shared controls. Forwarding through an already charged resource adds no second charge. Typed charged allocation keeps its maximum-alignment restriction, while the PMR adapter admits alignment padding and supports over-aligned payloads.

The retirement runtime owns the fixed registry, resident admission, rollback barrier and incoming/readiness handoff. Final release enqueues without allocation, destruction, free or thread join. A resident anchors its domain until cleanup finishes; the application retains the runtime until all domains drain. The runnable, waiting and retry queues are bounded, and the worker wakes periodically.
