# AGENTS.md

Working notes for agents modifying this repository. For the design read
`ARCHITECTURE.md`; for usage read `README.md`. This file covers the repo layout,
how to build and test, the invariants you must not break, and the traps that are
easy to fall into.

## Repo map

```
allocators.h            Declares allocator<T, Backend>, vanilla_allocator, tracked_allocator, memory_pool_allocator<T>, and total_allocated()/local_allocated(). Verbatim from Xapiand.
allocators.cc           Defines the malloc/free wrappers, the two backends, the byte counters, and (tracking on) the global new/delete override. Nearly verbatim; see "Standalone vs. Xapiand".
allocators_config.h.in  CMake template for the generated allocators_config.h: #cmakedefine's XAPIAND_TRACKED_MEM and HAS_THREAD_LOCAL. Replaces Xapiand's "config.h".
test/pool.cc            Memory pool: rebind identity, equality matrix, propagation traits, no-allocation conversion, cross-thread copy, overlap detection (its own ctest target, TSan-clean).
test/propagation.cc     Memory pool through container copy, move and swap (node and array containers, two-thread churn after a copy assignment), plus the stateless allocator over five containers; run under ASan and TSan.
test/test.cc            Runnable smoke test: detects tracking mode at runtime, checks the counters, the allocator over both backends, and the memory pool.
examples/demo.cc        The counters following strings and a tracked std::map (allocators_demo, tracked build).
CMakeLists.txt          STATIC library `allocators` (+ alias allocators::allocators); ALLOCATORS_TRACKED_MEM option; thread_local probe; configure_file; CTest test `allocators`.
LICENSE                 MIT, Copyright (c) 2015-2019 Dubalu LLC.
README.md                    What it is, install, quick start.
docs/API.md                  Usage, the backends, and the tracking build option.
docs/allocators.d2           The README diagram's source (docs/allocators.svg is rendered from it).
ARCHITECTURE.md         Internal design: the allocator template, byte accounting, the global override, the memory pool.
```

This is not header-only. `allocators.cc` must be compiled and linked; the header
only declares the allocators and the accessors. The CMake target is a `STATIC`
library.

## Build and run the test

```sh
cmake -B build -DALLOCATORS_TRACKED_MEM=ON && cmake --build build && ctest --test-dir build
```

No network is needed (no dependencies to fetch). Expected output ends with
`all allocators tests passed`, exit 0. The test target is `allocators_test`; the
registered CTest name is `allocators`. The test is only added when this repo is
the top-level project (`CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR`), so
consumers vendoring it via `FetchContent` won't build it.

Always verify **both** option states before committing a change:

```sh
cmake -B build-on  -DALLOCATORS_TRACKED_MEM=ON  && cmake --build build-on  && ctest --test-dir build-on  --output-on-failure
cmake -B build-off                              && cmake --build build-off && ctest --test-dir build-off --output-on-failure
```

`test/test.cc` covers both: it probes at runtime whether the build tracks memory
and asserts the contract for that mode. CTest runs it twice, as `allocators`
(the configured mode) and `allocators_tracked` (a second library built with
tracking), and `ALLOCATORS_EXPECT_TRACKED` makes each fail if the probe disagrees.

## The config decouple

`allocators.cc` reads exactly two macros that Xapiand's project-wide `"config.h"`
supplies upstream:

- **`XAPIAND_TRACKED_MEM`** — gates the byte accounting and the global
  `new`/`delete` override. Driven by the CMake option
  `ALLOCATORS_TRACKED_MEM` (default OFF), which `#cmakedefine`s the macro into
  the generated `allocators_config.h`. The macro **name is kept verbatim** so the
  source stays a faithful copy; only where it comes from differs (CMake here,
  Xapiand's config.h upstream).
- **`HAS_THREAD_LOCAL`** — picks the `thread_local` counter over the
  mutex-guarded map. Detected by CMake `check_cxx_source_compiles` and
  `#cmakedefine`d into the same header. `allocators.cc` also has an in-source
  compiler-feature probe, guarded by `#ifndef HAS_THREAD_LOCAL`, so a
  consumer that compiles the `.cc` without running this CMake still gets a value.

The include is guarded `#if __has_include("allocators_config.h")`, mirroring the
sibling `times` library, so a FetchContent-*populate* consumer (one that compiles
`allocators.cc` into its own build without running our `CMakeLists.txt`) still
compiles; it just defines the macros itself, or gets the untracked default plus
the in-source `thread_local` probe.

There is **no other coupling**: no `L_*` log calls, no `opts`, no `THROW`, no
other local include. Only `<config.h>` had to go.

## Conventions

- **C++20.** The target requests `cxx_std_20` `PUBLIC` to stay uniform with the
  sibling libraries. Don't drop the target below it.
- **Filenames are stable.** The header and source keep their original Xapiand
  names (`allocators.h`, `allocators.cc`) so a consumer that already
  `#include`s the header just needs this repo on the include path. Don't rename
  them.
- Tabs for indentation, double quotes in code, no em dashes in prose.
- MIT-licensed; keep the copyright headers on source files (the header also
  carries the moya-lang.org BSD-2-Clause notice for the memory pool).

## Load-bearing invariants

- **`tracked_allocator::allocate` / `deallocate` must agree on the size header.**
  In a tracking build, `allocate` over-allocates by `alignment` bytes, stores the
  size in the leading slot, and returns a pointer past it; `deallocate` steps back
  by the same `alignment`, reads the size, and frees the original pointer. If you
  change the header size or layout on one side, change both, or `delete` reads a
  bogus size and corrupts the counters (or the heap).
- **The global `new`/`delete` override is all-or-nothing and tracking-gated.**
  When `XAPIAND_TRACKED_MEM` is on, *all* of the `operator new` / `operator
  new[]` / `nothrow` / `operator delete` / sized-`delete` forms must be defined
  and all must route through `tracked_allocator`. A partial override (e.g. `new`
  tracked but `delete` not) mismatches the size header and corrupts accounting.
  When the macro is off, none of them are defined and the program's allocator is
  left untouched.
- **The accounting accessors always exist.** `total_allocated()` and
  `local_allocated()` are declared unconditionally in the header and must be
  defined in both build modes (real counters when tracking, constant `0`
  otherwise). Don't move their definition inside the tracking-only block.
- **The backends always exist.** `vanilla_allocator` and `tracked_allocator`
  member functions are defined in *both* build modes (in the untracked build,
  `tracked_allocator` is a plain malloc/free passthrough). The header advertises
  them as public API, so a consumer can instantiate `allocator<T, Backend>` in
  any build; the symbols must link.

## How to extend

- **Add a backend.** Write a class with static `allocate(size_t)` /
  `deallocate(void*)` and pass it as the second template arg to
  `allocator<T, Backend>`. Nothing else needs to change.
- **Always extend the smoke test.** `test/test.cc` is the only executable check,
  run in both modes. It must keep passing in **both** option states; a new assertion that only holds
  under tracking belongs behind the `tracked` runtime flag the test already
  computes.

## Traps

- **`tracked_allocator` is not interchangeable with `vanilla_allocator` across
  the global-override boundary.** In a tracking build, memory that came from the
  global `new` carries the size header and must be freed through the global
  `delete` (also tracked). Don't mix a tracked allocation with a vanilla free or
  vice versa; the header offset won't match.
- **Turning tracking on is a whole-program decision.** It overrides global
  `new`/`delete`, so every allocation in the final binary pays the size header
  and an atomic increment. It's a diagnostic build switch, not something to leave
  on in a latency-sensitive release.
- **The memory pool owns its storage.** `memory_pool_allocator` never returns
  freed slots to the OS; the shared pool family (chunks) lives until the last
  allocator copy or rebind is gone. Nodes stay valid while any allocator of the family
  lives, which a container's own allocator guarantees.
- **The pool is single-threaded per family.** Copies and rebinds share one pool; a
  container copy gets a fresh one (by construction, and by assignment because the
  target keeps its own allocator). A moved-from container keeps sharing with the
  container that took its nodes. Do not use two allocators of one family from two
  threads at once. `ALLOCATORS_POOL_CHECK` (on without `NDEBUG`) reports overlap.
- **The pool only allocates one element.** `allocate(n)` throws for `n != 1`. It
  is for node-based containers (`std::list`, `std::map`), not `std::vector`.

## Standalone vs. Xapiand

This is a standalone extraction from
[Xapiand](https://github.com/Kronuz/Xapiand). `allocators.h` is verbatim.
`allocators.cc` carries three small deltas, all forced by making the public API
linkable on its own rather than only from inside Xapiand's single translation
unit that used it:

1. **The `config.h` decouple** (above): the guarded
   `#include "allocators_config.h"` stands in for `#include "config.h"`, and this
   library's CMake supplies `XAPIAND_TRACKED_MEM` and `HAS_THREAD_LOCAL`.
2. **No `inline` on the backend member definitions.** Upstream marks
   `vanilla_allocator` / `tracked_allocator` `allocate`/`deallocate` `inline` in
   the `.cc`, which only links when every caller is in the same TU. The header
   declares them non-inline, so standalone they need external linkage to export.
3. **The backends and accessors are defined in both build modes.** Upstream wraps
   the entire body in `#ifdef XAPIAND_TRACKED_MEM` and defines only the two
   zero-returning accessor stubs in the `#else`, leaving the backends undefined
   in an untracked build. Nothing in Xapiand references them there, but a
   standalone consumer does. Here the malloc/free wrappers, both backends, and
   the accessors always compile; only the counters, the global override, and the
   tracked path's size-header bookkeeping stay gated.

Every delta is reconcilable with upstream as a plain edit; keep it that way.

## Explicit resource modules

`include/allocators/resources.h` owns checked resource vectors, concurrent owning byte charges and single-executor reservation books. `allocation.h` owns PMR allocation contexts, `charged_allocator.h` and `memory_resource.h` own charged adapters, and `storage.h`/`buffer.h` own page and buffer capacity. `retirement.h` plus `retirement.cc` own the optional POSIX cleanup runtime. These targets must never build or link `allocators.cc` merely through resource consumption.

Run `allocators_resources` and `allocators_retirement` alongside both legacy tracking modes. TSan skips only tracked allocation interception; it must still run resource and retirement tests. Counter-state construction is a separate fixed baseline; successful steady admission and normal reservation transitions allocate no bookkeeping storage. Preserve exact upstream extents, physical-free-before-refund and weak control charges. Reservation books outlive their capabilities, and unresolved destruction preserves debt. Runtime shutdown belongs to the application after all domains drain.
