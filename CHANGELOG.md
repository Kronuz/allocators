# Changelog

All notable changes to `allocators` are recorded here, newest first. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

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
