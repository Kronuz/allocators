# Construct forwarding: copy and move counts and timing

Measured 2026-10-31 at `allocators:ea0c5e6` (the fix is `allocators:7cdb0f8`, the benchmark `allocators:ea0c5e6`).

## Change

`construct(p, args...)` of `allocator<T, Backend>` and `memory_pool_allocator<T>` perfect-forwards its arguments to the element constructor, and its `noexcept` follows that constructor. A `construct(p, const T&)` overload copies instead of moving. `allocator_traits::construct(a, p, std::move(x))`, which `vector` calls when it relocates elements, then deep copies every `std::string`. The direct form `a.construct(p, v)` still compiles.

## Method

`allocators_bench` times `vector<int>` and `vector<std::string>` `push_back(T&&)` through `std::allocator` and through `allocators::allocator<T, vanilla_allocator>` for n = 16, 1,000 and 20,000. Each run interleaves A / B / A (the second A is the A/A spread) and keeps the best of 11 repetitions; the tables give the median of 7 runs, in nanoseconds per element. The "before" binary is the same benchmark built against the header at `allocators:3e3dfb2`.

Machines: Apple M4 Pro and Intel Core i9-9980HK (macOS 15 x86_64), Apple clang 17 with libc++, each run under a hold that keeps other work off the machine.

`allocators_bench --counts` prints the copy and move counts of 1,000 `push_back(T&&)` of a counting type and needs no timing.

## Results

### Copy and move counts, `vector<Counted>`, 1,000 `push_back(T&&)`

| Allocator | Header at `3e3dfb2` | Header at `7cdb0f8` |
|---|---|---|
| `std::allocator` | 0 copies, 2,023 moves | 0 copies, 2,023 moves |
| `allocators::allocator<T, vanilla_allocator>` | 2,023 copies, 0 moves | 0 copies, 2,023 moves |

### `vector<std::string>::push_back(T&&)`, ns per element

| Machine | Allocator | n = 16 | n = 1,000 | n = 20,000 |
|---|---|---|---|---|
| Apple M4 Pro | before | 59 | 72 | 86 |
| | after | 25 | 39 | 42 |
| | `std::allocator` | 25 | 38 | 41 |
| Intel i9-9980HK | before | 187 | 227 | 284 |
| | after | 89 | 131 | 143 |
| | `std::allocator` | 88 | 130 | 141 |

Against `std::allocator`, the ratio falls from 1.9-2.4 to 1.00-1.05 on the M4 Pro and from 1.75-2.14 to 1.01-1.02 on the i9.

### `vector<int>`

The rows move by at most +2.6% (M4 Pro) and +0.1% (i9), inside the 5% to 12% A/A spread.

### What remains over `std::allocator`

Up to 5% on the M4 Pro and 2% on the i9 for strings. libc++ relocates a vector's elements in bulk, but only for allocators without a `construct` member. Removing the `construct` members gives exact string parity and slows `vector<int>` growth by up to 9% (M4 Pro) and 54% (i9), so the members stay.

## Proof

`test/construct.cc` (CTest `allocators_construct`) compares the copy, move and destroy counts of the same program under `std::allocator` and under the library's allocators:

- `vector`, `deque`, `list`, `map` and `unordered_map` with a nothrow-move type, with a type whose copies and moves throw, and with `std::string` (the moved-from source is empty).
- `emplace_back` with several arguments, `insert` in the middle, `resize`, `assign` and copy.
- A type that throws once a shared budget runs out, at 58 budgets: the container ends in the same state (size, live objects, counts) with either allocator, and nothing leaks or is destroyed twice.
- `construct` of the pool allocator through `allocator_traits`, an rvalue moved and an lvalue copied.
- Static assertions that `noexcept` follows the element constructor.

Mutants, each of which a test catches:

| Mutant | Caught by |
|---|---|
| `std::forward` removed everywhere | the vector counts |
| `std::forward` removed in the pool allocator only | the direct `allocator_traits` check (no container test reaches the pool's `construct`) |
| `noexcept(true)` | a static assertion |
| `noexcept(false)` | a static assertion |

Built against the header at `3e3dfb2`, the vector count comparison fails.

Platforms: macOS arm64 with Apple clang 21 (tracked memory on and off, UBSan); macOS x86_64 under Rosetta; Linux x86_64 with GCC 11 under ASan and UBSan, and with Clang 12 under TSan; the CI matrix (Linux GCC and Clang on x86_64 and arm64, macOS arm64 and x86_64, FreeBSD, ASan with UBSan, TSan) passes at `ea0c5e6`.

## Decision and open items

- The members stay and forward; the 5% and 2% string gap is the price of keeping `vector<int>` growth fast.
- The mutants have not run against libstdc++ on x86_64; a libstdc++ run exists only through CI.
- Unrelated to this change: Clang 12 with ASan and `ALLOCATORS_TRACKED_MEM` fails to link (`operator new` is defined twice), and GCC 11's TSan crashes on every test.
