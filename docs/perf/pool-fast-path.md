# Pool: allocating and freeing one block

`memory_pool_allocator<T>::allocate(1)` followed by `deallocate` of the same block, over and over, is slower
than at START (`allocators:e8fb173`) on an Intel i9-9980HK: 2.04 ns against 0.83 ns. Every row with more than
one live block is faster (1.1 to 1.7 times). This note has the cause, the variants tried and the numbers.
The change landed is the allocator-local head (see the last section): the i9 pair goes from 2.03 to 1.00 ns, the
M4 Pro's from 1.23 to 0.66 ns, and no other row moves. The i9 is still 0.75 of START's speed
([Kronuz/allocators#1](https://github.com/Kronuz/allocators/issues/1)). A slot for the newest block
(tag `archive/perf/pool-newest-slot`, `allocators:b724644`) was tried first and not landed: each variant of it that fixes part
of the row makes the refill rows slower than START on the i9.

## The row

`bench/pool_bench.cc` (`allocators_pool_bench`) calls `allocate(1)` and `deallocate` directly on one allocator, no
container, ns per operation, best of 9 per row. `alloc+free steady` is the loop

```cpp
for (int i = 0; i < 4000000; ++i) { Node* n = p.allocate(1); sink = n; p.deallocate(n, 1); }   // sink is void* volatile
```

and `alloc+free steady, median of 9 pool placements` is the same loop on nine pools, each placed differently in
memory by a padding allocation: an Apple M4 Pro runs the single-pool loop in 0.4 ns or in 1.15 ns from one
placement to the next, so the median is the stable row there. The other rows are refills (n blocks allocated,
then freed in allocation order, n = 2, 1,000, 100,000), fresh pools, frees in random order, and `std::allocator`
as the control.

## Cause

At START the pool lived in the allocator value. In the loop above the allocator is a local, so the free-list head
stays in a register and each pair is one load of the block's link and one store of it. Now every copy and rebind
shares one pool through a `shared_ptr`, so the head lives in heap memory reached through `size_class`, and the
compiler cannot keep it in a register across the loop. Apple clang 17.0.0 (`clang-1700.6.4.2`, the i9's), the
loop's assembly:

```
START                                      HEAD
LBB0_1: movq (%rax), %rcx                  LBB0_1: movq (%r15), %rcx      ## next = block->next
LBB0_2: movq %rax, __ZL4sink(%rip)                 movq %rcx, 8(%rbx)     ## free_head = next
        movq %rcx, (%rax)                          movq %r15, __ZL4sink(%rip)
        movq %rax, -2752(%rbp)             LBB0_2: movq 8(%rbx), %rcx     ## reload free_head
                                                   movq %rcx, (%r15)      ## block->next = free_head
                                                   movq %r15, 8(%rbx)     ## free_head = block
```

HEAD reloads `free_head` after the volatile store (it cannot prove the store did not change it), so the value that
the next allocate loads as `block->next` went through two store-to-load forwards, `free_head` and then the link.
START's goes through one. On the i9 a load and a store of one cell take 1.08 ns per round trip (4.4 register
adds, `hop_probe`), START's pair takes 0.83 ns and HEAD's 2.04 ns. Apple clang 21's x86-64 output has the same
reload; its arm64 output stores the loaded link back without one, and the pair is still 0.59 of START's speed on
an Apple M1 Pro. The Apple M4 Pro forwards a fixed address in one add (0.25 ns) and its HEAD pair is at START's
median.

Not the cause: a branch added to `deallocate` that does the same on both arms runs at HEAD's speed (the compiler
may fold the arms, so this says little about branches), and marking `grow` and the size-class lookup cold changes
nothing measurable on the VM it was tried on.

## Variants

Variants differ only in `pool_class::allocate` and `deallocate` (`newest` is a pointer slot, `flag` a bool that
says the slot's block is free):

| Variant | Free list |
|---|---|
| slot first (tag `archive/perf/pool-newest-slot`) | `newest` holds the block freed last; `deallocate` links the old one onto the list and takes the new one; `allocate` takes `newest`, then the list |
| slot, list first | `newest` holds the only free block, and the list is empty then; `allocate` takes the list, then the slot |
| flag first | `newest` plus a flag: a free of the block just handed out stores nothing but the flag |
| flag, list first | the flag variant with the list consulted first |

ns per operation, ratio START/variant in parentheses (above 1 is faster than START), Apple clang 17.0.0, 7 rounds
of every binary interleaved with an A/A copy of START, one `quiet strict` hold:

| Row, Intel i9-9980HK | START | HEAD | slot first | slot, list first | flag first | flag, list first |
|---|--:|--:|--:|--:|--:|--:|
| steady, one pool | 0.833 | 2.04 (0.39) | 1.21 (0.69) | 1.45 (0.57) | 1.19 (0.67) | 1.93 (0.42) |
| steady, median of 9 placements | 0.787 | 2.07 (0.38) | 1.21 (0.64) | 1.45 (0.54) | 1.94 (0.40) | 2.42 (0.32) |
| refill n=2 | 2.50 | 2.54 (0.99) | 2.25 (1.11) | 2.98 (0.84) | 3.30 (0.76) | 3.47 (0.72) |
| refill n=1,000 | 3.23 | 2.54 (1.27) | 3.34 (0.98) | 3.30 (0.99) | 3.70 (0.87) | 3.54 (0.92) |
| refill n=100,000 | 3.73 | 3.15 (1.19) | 3.96 (0.94) | 3.66 (1.01) | 4.03 (0.93) | 3.79 (0.99) |
| fresh pool n=1,000 | 3.72 | 3.05 (1.22) | 2.64 (1.41) | 2.62 (1.41) | 3.72 (1.00) | 2.87 (1.27) |
| fresh pool n=100,000 | 3.54 | 2.13 (1.66) | 1.72 (2.09) | 1.72 (2.08) | 2.75 (1.30) | 1.88 (1.88) |
| random-order frees | 17.7 | 15.7 (1.13) | 17.6 (1.01) | 16.3 (1.10) | 16.1 (1.10) | 16.3 (1.09) |
| `std::allocator` (control) | 39.8 | 39.7 (1.01) | 39.7 (1.00) | 39.9 (1.00) | 40.1 (0.99) | 40.0 (0.99) |

No variant reaches START on the steady row, and every variant makes the refill rows of 1,000 and 100,000 blocks 16
to 46 percent slower than HEAD (0.87 to 1.01 of START, from 1.27 and 1.19). `compare_pmr` (list and map workloads)
stays within the spread for the slot-first variant, 0.91 to 1.07 of HEAD's time on the i9.

| Row, Apple M4 Pro, 13 rounds | START | HEAD | slot first |
|---|--:|--:|--:|
| steady, one pool | 0.424 | 1.28 (0.39) | 0.879 (0.54) |
| steady, median of 9 placements | 1.27 | 1.30 (0.98) | 0.899 (1.42) |
| refill n=2 | 1.71 | 1.69 (0.99) | 1.99 (0.83) |
| refill n=1,000 | 1.66 | 0.973 (1.77) | 1.10 (1.53) |
| refill n=100,000 | 2.11 | 1.56 (1.34) | 1.69 (1.24) |
| fresh pool n=1,000 | 2.20 | 2.17 (1.01) | 2.24 (0.98) |
| fresh pool n=100,000 | 2.41 | 1.84 (1.31) | 1.85 (1.30) |
| random-order frees | 9.04 | 8.44 (1.08) | 8.19 (1.11) |
| `std::allocator` (control) | 11.7 | 12.1 (0.98) | 11.8 (1.00) |

On the M4 Pro HEAD's steady row is at START's median, and the slot makes the refill rows 8 to 13 percent and the
two-block row 18 percent slower than HEAD (`compare_pmr`'s map churn of 1,000 nodes, 5.6 percent). On an Apple M1
Pro (light hold, noisy) the slot variant's steady row is 1.96 times START's speed and its refill rows 1.13 and
1.15 times, against 0.59, 1.20 and 1.21 for HEAD.

## Why nothing is landed

The slot trades the one-block row against every row that has more than one free block, and on the i9 it leaves
the refill rows below START, which HEAD beats by 1.2 to 1.3 times. The extra work is one to three loads, stores
or branches on a path of 8 to 12 instructions; why that costs 30 percent on the i9's refill loops was not
isolated (on an Apple M1 Pro, HEAD's `allocate` with the slot's `deallocate` is as slow as the slot variant, and
the slot's `allocate` with HEAD's `deallocate` is as fast as HEAD, so the cost is in the free path). The one-block
pair stays 0.64 to 0.69 of START's speed even then: START's loop has the link as its only loop-carried memory,
and a pool shared by copies cannot keep its head in a register.

## Allocator-local head (landed)

The next step, built: while an allocator is the only handle of its family it keeps the blocks it frees in
the allocator itself (`local`, newest first), and `allocate` takes from it before it asks the pool; the first copy,
rebind, copy assignment or move hands the list to the family (`leave_alone`, which splices it in front of the
free list in order), so the block freed last is still the next one handed out by any equal allocator.
Containers' allocators are always copies, so they take today's path plus one load and one branch. `test/pool.cc`
runs the model with a copy made at every position of every operation string and has a test of each way a second
handle appears, `tools/pool_mutants.py` has seven mutants of the handoff (all caught), and the suite is clean
under ASan, UBSan and TSan on Linux x86-64 and passes the AL1 tests.

Apple clang 17.0.0, interleaved rounds with an A/A copy of START, one `quiet strict` hold per machine. ns per
operation, ratio START/variant in parentheses:

| Row | i9 START | i9 HEAD | i9 local head | M4 START | M4 HEAD | M4 local head |
|---|--:|--:|--:|--:|--:|--:|
| steady, one pool | 0.744 | 2.03 (0.37) | 0.999 (0.75) | 1.16 | 1.23 (0.96) | 0.663 (1.76) |
| steady, median of 9 placements | 0.751 | 2.05 (0.37) | 0.975 (0.78) | 1.19 | 1.24 (0.97) | 0.678 (1.77) |
| refill n=2 | 2.44 | 2.41 (1.01) | 2.13 (1.14) | 1.68 | 1.66 (0.99) | 1.58 (1.06) |
| refill n=1,000 | 3.11 | 2.52 (1.24) | 2.44 (1.28) | 1.62 | 0.974 (1.65) | 0.984 (1.68) |
| refill n=100,000 | 3.59 | 3.13 (1.14) | 3.10 (1.17) | 2.02 | 1.51 (1.34) | 1.52 (1.34) |
| fresh pool n=1,000 | 3.72 | 3.01 (1.23) | 3.21 (1.16) | 2.17 | 2.12 (1.03) | 2.10 (1.04) |
| fresh pool n=100,000 | 3.55 | 2.11 (1.68) | 2.38 (1.49) | 2.40 | 1.79 (1.35) | 1.79 (1.34) |
| random-order frees | 17.3 | 15.6 (1.14) | 15.3 (1.13) | 8.44 | 7.88 (1.07) | 8.07 (1.06) |
| `std::allocator` (control) | 39.8 | 39.6 (1.01) | 40.4 (0.98) | 11.6 | 11.9 (0.98) | 11.6 (1.01) |

`compare_pmr` against HEAD: 0.97 to 1.11 on the i9 (geomean 1.03) and 0.996 to 1.03 on the M4 Pro. On the M4 Pro
every row is at HEAD's within the spread and the one-block pair is 1.8 times START's speed. On the i9 the pair
is twice as fast as HEAD and still 0.75 to 0.78 of START's speed. What is left is store throughput: Apple clang 17's loop has four stores per iteration (the sink, the block's link, and two
writes of `local` to its stack slot, which it keeps because the allocator's address escapes on the cold paths),
START's has three, and the i9 retires one store per cycle (1.0 ns against 0.74). Merging the two writes of `local`
(the compiler does not, across the volatile store and the `alone` test) is the next thing to try.

## Reproduce

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target allocators_pool_bench
./build/allocators_pool_bench
```

To compare revisions build `bench/pool_bench.cc` once per tree (it uses only `allocate` and `deallocate`), with
`c++ -std=c++20 -O3 -DNDEBUG -pthread -I <tree> bench/pool_bench.cc`, and run the binaries interleaved with an A/A
copy through `cpp-libraries/tools/ab_compare.py` (`--format unit`). `test/pool.cc` pins the LIFO order against a
model (every operation string of up to six steps over allocate, free-oldest, free-newest and a copy of the
allocator, then 400,000 random steps over two block sizes), and `tools/pool_mutants.py` breaks the
push and pop and the handoff thirteen ways and requires that test to fail on each.
