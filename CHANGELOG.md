# Changelog

All notable changes to `allocators` are recorded here, newest first. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## 2026-11-01

### Fixed

- The tracked build's `operator new` and `tracked_allocator::allocate` returned blocks that were 8 modulo 16 on
  Apple arm64, where `std::max_align_t` is 8 bytes and `__STDCPP_DEFAULT_NEW_ALIGNMENT__` is 16: the size header in
  front of each block was `alignof(std::max_align_t)` bytes. UBSan reported every new-expression of a class (`constructor
  call on misaligned address ... for type 'allocators::detail::pool_class', which requires 16 byte alignment`) and
  `allocators_tracked` aborted under ASan and UBSan on an Apple M1 Pro. START had the same header (`allocators.cc` is
  unchanged since), so the blocks were misaligned there too: a new-expression of a 16-aligned type aborts under UBSan with START's
  `allocators.cc`; START's own test constructed no such object; since `085331c` the pool's size classes are made with
  `std::make_unique`, and the tracked test reaches one. The header is now the larger of the two alignments.
  `allocators_tracked` (`test/test.cc`) checks that every form of `operator new`, `tracked_allocator::allocate` and a
  new-expression of a type aligned to the default new alignment return blocks aligned to it: it fails on the M1 Pro with
  the old header (`CHECK failed: default_aligned(...)`, and the UBSan report) and passes with the new one, and the
  suite passes under ASan and UBSan there. The untracked build is the system's: its `operator new` is not checked
  (FreeBSD's hands out 8-aligned blocks for tiny sizes).

### Performance

- `memory_pool_allocator<T>`: an allocator that is the only handle of its family keeps the blocks it frees
  (`local`, newest first) and `allocate` takes them before it asks the pool; the first copy, rebind, copy
  assignment or move splices them in front of the family's free list in order, so the block freed last is still the
  next one handed out by any equal allocator, and a container's allocator, always a copy, takes the old path plus a
  load and a branch. It closes most of a regression that the shared pool had left: `allocate(1)` then `deallocate`
  of one block, over and over, took 2.03 ns on an Intel i9-9980HK against 0.74 ns at START (`e8fb173`, before the pool
  was shared), 0.37 of START's speed, because the free-list head moved to heap memory, the compiler reloads it after
  the caller's store and the next allocate waits on two chained store-to-load forwards where START's waited on one.
  Now it takes 1.00 ns on the i9 (0.75 of START's speed; the gap left is a fourth store per iteration, Apple clang
  keeps two writes of `local` where START's loop has one) and 0.66 ns on an Apple M4 Pro against START's 1.16 (1.8
  times faster; HEAD's was at START's median). Every other `pool_bench` row and `compare_pmr` stay at their previous
  values within the spread on both machines (Apple clang 17.0.0, interleaved rounds with an A/A copy, one
  `quiet strict` hold per machine), and every row with more than one live block is at or above START's. A slot for the
  newest freed block was tried first and not landed: it makes the i9's refill rows slower than START's.
  [docs/perf/pool-fast-path.md](docs/perf/pool-fast-path.md) has the assembly, the variants and the tables; the
  remaining gap is [#1](https://github.com/Kronuz/allocators/issues/1).

### Tests

- `test/propagation.cc` checks, over `list`, `forward_list`, `set`, `multiset`,
  `map` and `multimap`, that `a = {}` and `clear()` keep a moved-from
  container's pool, that it can `splice` or `merge` into the container that took
  its nodes and keep churning, and that a copy, a copy-assigned container and
  its source have unequal allocators.
- `test/pool.cc` checks the pool's free list against a model: every operation string of up to six steps over
  allocate, free-oldest, free-newest and a copy of the allocator (made once, anywhere in the string), through
  the original and the copy, and 400,000 random steps over two
  block sizes, require the block freed last to be the next one handed out and no block to be lost, served twice or
  overlapped. `tools/pool_mutants.py` breaks the push and pop and the handoff thirteen ways; each is caught by `test/pool.cc`.

### Added

- `bench/compare_pmr.cc` (`-DALLOCATORS_BUILD_BENCH=ON`, target
  `allocators_compare_pmr`): the pool against `std::allocator`,
  `std::pmr::unsynchronized_pool_resource` and `std::pmr::monotonic_buffer_resource`
  on list builds, map churn and steady list churn. The README states its numbers on an
  Apple M4 Pro and an Intel i9-9980HK: the pool takes 0.56 to 0.82 of malloc's time, and
  `pmr::unsynchronized_pool_resource` is 0.20 to 0.50 on lists and 0.65 to 0.99 on map churn.
- `docs/perf/construct-forwarding.md`: the method, counts, timings and mutants
  of the `construct` forwarding below.
- `bench/pool_bench.cc` (`allocators_pool_bench`): `allocate(1)` and `deallocate` called directly: steady one-block
  reuse (on one pool and as the median of nine placements), refills of 2, 1,000 and 100,000 blocks, fresh pools,
  random-order frees, with `std::allocator` as the control. It builds against any revision of `allocators.h`, so
  `ab_compare.py` can compare revisions.
- `docs/perf/pool-fast-path.md` and `tools/pool_mutants.py`: the one-block pair analysis above, and the six
  mutants of the pool's push and pop.

## 2026-10-31

### Fixed

- `memory_pool_allocator` set `propagate_on_container_copy_assignment` to
  `true_type`, so `e = d` made `e` use `d`'s pool: the copy shared an
  unsynchronized pool with its source, against the documented "a container copy
  gets its own pool". Two threads churning `d` and `e` raced on the free list
  and corrupted the heap (ASan SEGV in `list::pop_front`, TSan race between
  `pool_class::allocate` and `deallocate`). It is now `false_type`: the target
  keeps its allocator and allocates the copied elements from its own pool. Move
  assignment and swap still propagate, and `is_always_equal` is still `false_type`.
  A moved-from container keeps sharing the pool with the container that took its
  nodes, which the Allocator move requirement demands; the README says so.
  `test/propagation.cc` covers copy, move and swap over `list`, `forward_list`,
  `set`, `multiset`, `map` and `multimap`, the array containers the pool refuses
  (`vector`, `deque`, `unordered_map`), and the stateless allocator over five
  containers.
- `allocator<T, Backend>::construct` and `memory_pool_allocator<T>::construct`
  took only `const T&`, so `allocator_traits::construct(a, p, std::move(x))`
  copy-constructed. Every `vector` reallocation and every `push_back(T&&)` deep
  copied its elements (a `vector` of a counting type filled by 1000
  `push_back(T&&)` made 2023 copies and no moves, against no copies and 2023
  moves with `std::allocator`). `construct` now perfect-forwards its arguments, with
  `noexcept` following the element constructor, so copy, move and throwing-move
  behaviour match `std::allocator`. Direct calls such as `a.construct(p, v)`
  still compile. `allocators_bench` (median of 7 alternating runs, ns per
  element) for `vector<std::string>::push_back(T&&)` at n = 16 / 1000 / 20000:
  Apple M4 Pro 59 / 72 / 86 before, 25 / 39 / 42 after (`std::allocator` 25 / 38
  / 41); Intel i9-9980HK 187 / 227 / 284 before, 89 / 131 / 143 after
  (`std::allocator` 88 / 130 / 141). The `vector<int>` rows move by at most 2.6%
  (M4) and 0.1% (i9), inside the 5-12% A/A spread. What remains over
  `std::allocator` (up to 5% on the M4, 2% on the i9) is libc++'s bulk
  relocation, which applies only to allocators without a `construct` member;
  dropping the members gives string parity but slows `vector<int>` growth by up
  to 9% (M4) and 54% (i9).
- `memory_pool_allocator`'s converting constructor started a fresh pool, so
  `A<T>(A<U>(a)) != a`, the `noexcept` conversion allocated (a `bad_alloc` would
  terminate), container swap of lists with different pools was undefined
  (`propagate_on_container_*` and `is_always_equal` were left at their defaults),
  and a container copy shared the original's unsynchronized pool. Allocators now
  share a pool family: one size class per block size, with chunked storage, a
  bump pointer, and a LIFO free list. Rebinding copies the handle (`noexcept`, no
  allocation, `A<T>(A<U>(a)) == a`, `operator==` across value types).
  `propagate_on_container_copy/move_assignment` and `_swap` are `true_type`,
  `is_always_equal` is `false_type`, and a container copy gets a new pool. The
  type, template parameters, and `allocate`/`deallocate` signatures are
  unchanged.

### Added

- Overlapping use of one pool from two threads is detected in builds without
  `NDEBUG` or with `ALLOCATORS_POOL_CHECK` (abort by default, replaceable through
  `allocators::set_pool_misuse_handler`).

## 2026-10-30

### Fixed

- `std::list` rebinds the allocator to its node type and hands copies of it
  around. The pool lived inside the allocator value, so a copy allocated from its
  own deque and freed it when the temporary died, leaving the list nodes
  dangling and corrupting the heap after a few elements. The pool state is
  shared between copies, and copies compare equal by state (the old `operator==`
  compared object addresses). Regression tests cover `std::list` and `std::map`
  up to 5000 elements and a `std::vector` that must throw `bad_alloc`.
