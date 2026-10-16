// Smoke test for the standalone allocators library.
//
// Exercises allocators.h / allocators.cc: the tracked_allocator byte accounting
// exposed through total_allocated()/local_allocated(), the generic
// allocator<T, Allocator> used as a std::vector allocator, and the
// memory_pool_allocator. The accounting half adapts to how the library was
// built: with ALLOCATORS_TRACKED_MEM=ON the global operator new/delete is
// overridden and the counters move with every allocation; with it OFF the
// accessors are stubs that always return 0. The test detects which mode it is in
// at runtime (a probe allocation either moves total_allocated() or it doesn't)
// and asserts the contract for that mode, so one binary covers both builds.
//
// Build via CMake: cmake -B build -DALLOCATORS_TRACKED_MEM=ON && cmake --build build && ctest --test-dir build
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "allocators.h"


// ---------------------------------------------------------------------------
// Probe whether this build tracks memory. With ALLOCATORS_TRACKED_MEM=ON the
// global new/delete are overridden and total_allocated() moves; with it OFF the
// accessor is a stub returning 0. We allocate a chunky buffer through the global
// new (which the library may have hijacked) and see whether the counter reacts.
// ---------------------------------------------------------------------------

static bool tracking_enabled() {
	long long before = allocators::total_allocated();
	volatile char* p = new char[64 * 1024];
	p[0] = 1;  // touch it so the allocation can't be optimized away
	long long during = allocators::total_allocated();
	delete[] p;
	long long after = allocators::total_allocated();

	if (before == 0 && during == 0 && after == 0) {
		return false;  // untracked build: the accessor is a constant 0
	}
	// Tracked build: the counter grew while the buffer was live and dropped back.
	assert(during >= before + 64 * 1024);
	assert(after == before);
	return true;
}


// ---------------------------------------------------------------------------
// total_allocated() / local_allocated() accounting. In a tracked build a live
// allocation must raise both counters by at least its size, and freeing it must
// bring them back to where they started. In an untracked build both accessors
// stay pinned at 0.
// ---------------------------------------------------------------------------

static void test_accounting(bool tracked) {
	if (!tracked) {
		assert(allocators::total_allocated() == 0);
		assert(allocators::local_allocated() == 0);
		std::printf("allocators accounting OK: untracked build, counters constant 0\n");
		return;
	}

	const std::size_t n = 128 * 1024;

	long long total0 = allocators::total_allocated();
	long long local0 = allocators::local_allocated();

	volatile char* buf = new char[n];
	for (std::size_t i = 0; i < n; i += 4096) {
		buf[i] = static_cast<char>(i);  // fault in the pages, defeat DCE
	}

	long long total1 = allocators::total_allocated();
	long long local1 = allocators::local_allocated();

	assert(total1 - total0 >= static_cast<long long>(n));
	assert(local1 - local0 >= static_cast<long long>(n));

	delete[] buf;

	long long total2 = allocators::total_allocated();
	long long local2 = allocators::local_allocated();

	assert(total2 == total0);  // freeing returns the total to its starting value
	assert(local2 == local0);  // and the per-thread counter too

	std::printf("allocators accounting OK: tracked build, counters move with new/delete\n");
}


// ---------------------------------------------------------------------------
// allocators::allocator<T, Allocator> must behave as an ordinary std::allocator:
// a std::vector parameterized on it should grow, hold values, and free cleanly.
// We test it over both vanilla_allocator (plain malloc/free) and
// tracked_allocator (the accounting backend) so both allocate/deallocate paths
// run. The allocator comparison operators are stateless: any two instances are
// equal, so vectors can move/swap freely.
// ---------------------------------------------------------------------------

template <typename Backend>
static void exercise_vector(const char* name) {
	using Alloc = allocators::allocator<int, Backend>;
	std::vector<int, Alloc> v;

	for (int i = 0; i < 10000; ++i) {
		v.push_back(i * 3);
	}
	assert(v.size() == 10000);
	assert(v.front() == 0);
	assert(v.back() == 9999 * 3);

	long long sum = 0;
	for (int x : v) {
		sum += x;
	}
	// 3 * sum(0..9999) = 3 * (9999 * 10000 / 2)
	assert(sum == 3LL * (9999LL * 10000LL / 2));

	v.clear();
	v.shrink_to_fit();
	assert(v.empty());

	// Stateless equality: two allocators of the same kind compare equal.
	Alloc a;
	Alloc b;
	assert(a == b);
	assert(!(a != b));

	std::printf("allocators allocator<int, %s> OK: std::vector grows/sums/frees\n", name);
}


// ---------------------------------------------------------------------------
// memory_pool_allocator hands out one element at a time from a deque-backed pool
// and recycles freed blocks through a free list. A std::list-like single-element
// allocation pattern is its intended use; a std::vector won't work (it asks for
// n > 1), so we drive it directly: allocate a batch, write through the pointers,
// free them, then allocate again and confirm the freed blocks are reused.
// ---------------------------------------------------------------------------

static void test_memory_pool() {
	allocators::memory_pool_allocator<long> pool;
	using Traits = std::allocator_traits<allocators::memory_pool_allocator<long>>;

	long* slots[16];
	for (int i = 0; i < 16; ++i) {
		slots[i] = Traits::allocate(pool, 1);
		*slots[i] = i * 100;
	}
	for (int i = 0; i < 16; ++i) {
		assert(*slots[i] == i * 100);
	}

	// Free them back into the pool's free list, most-recent first.
	for (int i = 15; i >= 0; --i) {
		Traits::deallocate(pool, slots[i], 1);
	}

	// The next allocations must come straight off the free list (LIFO), so the
	// pointer handed back first is the one freed last.
	long* reused = Traits::allocate(pool, 1);
	assert(reused == slots[0]);
	Traits::deallocate(pool, reused, 1);

	std::printf("allocators memory_pool_allocator OK: pooled allocate/free, blocks recycled\n");
}


int main() {
	bool tracked = tracking_enabled();
	std::printf("allocators: tracking %s\n", tracked ? "ENABLED" : "disabled");
#if defined(ALLOCATORS_EXPECT_TRACKED)
	assert(tracked == (ALLOCATORS_EXPECT_TRACKED != 0));  // the build mode under test is the one running
#endif

	test_accounting(tracked);
	exercise_vector<allocators::vanilla_allocator>("vanilla_allocator");
	exercise_vector<allocators::tracked_allocator>("tracked_allocator");
	test_memory_pool();

	std::printf("all allocators tests passed\n");
	return 0;
}
