# Pool: allocating and freeing one block

`memory_pool_allocator<T>::allocate(1)` followed by `deallocate` of the same block, over and over, was slower
after the pool became shared (`allocators:085331c`) than before it: the gate of 2026-11-01 measured 2.7 times
START's time on an Intel i9-9980HK (0.741 ns to 2.020 ns). Every row with more than one live block was faster.
This note has the cause, the change and the measurements.

## Cause

At START the pool lived in the allocator value: a local `Pool p` kept the free-list head in a register and the
loop was one dependent load and one store per block. Now the pool is shared by every copy and rebind of an
allocator, so the list head sits in heap memory, reached through `size_class`, and two stores to it and one to the
block's link are in flight per pair. Loop of `p.allocate(1)`, a volatile store of the block, `p.deallocate(n, 1)`:

START (`x86_64`, Apple clang 21: the head stays in `%rax`, one link load and one link store):

```
LBB0_2: movq (%rax), %rcx          ## next = block->next
LBB0_7: movq %rax, _sink(%rip)
        movq %rcx, (%rax)          ## block->next = next
        movq %rax, -2752(%rbp)     ## head (a stack slot, never loaded in the loop)
```

HEAD (same compiler: the head is reloaded after the volatile store, the compiler cannot prove it did not change):

```
LBB0_5: movq (%r15), %rcx          ## next = block->next
        movq %rcx, 8(%rbx)         ## free_head = next
        movq %r15, _sink(%rip)
LBB0_13: movq 8(%rbx), %rcx        ## reload free_head
        movq %rcx, (%r15)          ## block->next = free_head
        movq %r15, 8(%rbx)         ## free_head = block
```

The next allocate loads `block->next` from the store just above, which stores a value loaded from `free_head`,
which was stored by the allocate before it: two chained store-to-load forwards per pair against one at START.
On an arm64 core that renames memory (the Apple M4 Pro forwards a store to a load in about one cycle when the
address is fixed) the pointer-dependent forward is not renamed either, and the loop also runs 0.6 to 0.9 times
START's speed there.

What does not matter: a variant of HEAD with only a branch added to `deallocate` (both arms do the same) runs
at HEAD's speed, and so does one that marks `grow` and the size-class lookup cold.

## Change

The newest freed block lives in its own slot, `newest`, ahead of the list. `deallocate` links the block the slot
held onto the list and takes the new block into the slot; `allocate` takes the slot first, then the list, then
the bump pointer. The order stays exactly LIFO. A free followed by an allocate of the same block now reads and
writes one pointer and writes no link into the block.

## Method

TBD
