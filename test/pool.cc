// memory_pool_allocator: identity across rebinds, allocator propagation, the
// no-allocation converting constructor, container copy across threads, and the
// overlapping-use check.
//
// The oracle is the C++ Allocator requirements ([allocator.requirements]) and the
// allocator_traits / container rules ([container.reqmts]): A<T>(A<U>(a)) == a,
// a == b iff storage from one can be freed through the other, propagate_on_* decide
// what container assignment and swap do with the allocator, and a noexcept
// conversion must not throw (so must not allocate).
//
// Build with -DALLOCATORS_POOL_CHECK=1 so the overlap detector is on whatever the
// build type is (the CMake target does).
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <list>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <thread>
#include <type_traits>
#include <vector>

#include "allocators.h"

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "CHECK failed: %s (line %d)\n", #cond, __LINE__); std::abort(); } } while (0)

// (TSan and ASan runtimes define operator new themselves.)
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#  define POOL_TEST_TSAN 1
#elif defined(__has_feature)
#  if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#    define POOL_TEST_TSAN 1
#  endif
#endif

// Count global allocations, so "the conversion does not allocate" is observed rather
// than assumed. (TSan's runtime defines operator new itself, so it is skipped there.)
static std::atomic<long> g_news{0};
#ifndef POOL_TEST_TSAN
void* operator new(std::size_t n) {
	g_news.fetch_add(1, std::memory_order_relaxed);
	if (void* p = std::malloc(n ? n : 1)) return p;
	throw std::bad_alloc();
}
void* operator new(std::size_t n, std::align_val_t al) {
	g_news.fetch_add(1, std::memory_order_relaxed);
	void* p = nullptr;
	if (posix_memalign(&p, static_cast<std::size_t>(al) < sizeof(void*) ? sizeof(void*) : static_cast<std::size_t>(al), n ? n : 1) == 0) return p;
	throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
#endif

using allocators::memory_pool_allocator;

struct Big { char bytes[200]; };

static_assert(std::is_nothrow_constructible_v<memory_pool_allocator<int>, const memory_pool_allocator<long>&>);
static_assert(std::is_nothrow_copy_constructible_v<memory_pool_allocator<int>>);
static_assert(std::is_nothrow_copy_assignable_v<memory_pool_allocator<int>>);
static_assert(std::is_same_v<std::allocator_traits<memory_pool_allocator<int>>::propagate_on_container_copy_assignment, std::true_type>);
static_assert(std::is_same_v<std::allocator_traits<memory_pool_allocator<int>>::propagate_on_container_move_assignment, std::true_type>);
static_assert(std::is_same_v<std::allocator_traits<memory_pool_allocator<int>>::propagate_on_container_swap, std::true_type>);
static_assert(std::is_same_v<std::allocator_traits<memory_pool_allocator<int>>::is_always_equal, std::false_type>);
static_assert(std::is_same_v<std::allocator_traits<memory_pool_allocator<int>>::rebind_alloc<Big>, memory_pool_allocator<Big>>);

// A<T>(A<U>(a)) == a, and equality is by pool, across value types.
static void test_rebind_identity() {
	memory_pool_allocator<int> a;
	memory_pool_allocator<Big> big(a);
	memory_pool_allocator<char> ch(big);
	memory_pool_allocator<int> back(ch);
	CHECK(back == a);
	CHECK(a == big && big == a);   // cross-type, both orders
	CHECK(big == ch && ch == back);
	CHECK(!(a != big));

	memory_pool_allocator<int> other;
	memory_pool_allocator<Big> other_big(other);
	CHECK(a != other && a != other_big && other_big != a);
	CHECK(!(a == other_big));

	// Storage allocated through one is freed through an equal one of another type.
	int* p = std::allocator_traits<memory_pool_allocator<int>>::allocate(a, 1);
	*p = 7;
	Big* q = std::allocator_traits<memory_pool_allocator<Big>>::allocate(big, 1);
	std::allocator_traits<memory_pool_allocator<int>>::deallocate(back, p, 1);
	std::allocator_traits<memory_pool_allocator<Big>>::deallocate(big, q, 1);
	// The freed int block is the next int handed out (LIFO), through any equal allocator.
	CHECK(std::allocator_traits<memory_pool_allocator<int>>::allocate(a, 1) == p);
	std::printf("pool OK: rebind round trips keep the pool; equality is by pool across types\n");
}

// A noexcept conversion cannot allocate: counted, not assumed.
static void test_conversion_does_not_allocate() {
	memory_pool_allocator<int> a;
	(void)std::allocator_traits<memory_pool_allocator<int>>::allocate(a, 1);
	long before = g_news.load();
	for (int i = 0; i < 1000; ++i) {
		memory_pool_allocator<Big> b(a);
		memory_pool_allocator<char> c(b);
		memory_pool_allocator<int> d(c);
		CHECK(d == a);
	}
#ifndef POOL_TEST_TSAN
	CHECK(g_news.load() == before);
	// A default-constructed allocator starts a pool: one allocation, and that is the only cost.
	before = g_news.load();
	{ memory_pool_allocator<int> fresh; (void)fresh; }
	CHECK(g_news.load() - before == 1);
#else
	(void)before;
#endif
	std::printf("pool OK: converting constructor allocates nothing\n");
}

// Allocation of several sizes from one family: aligned, disjoint, recycled per size.
static void test_sizes_share_family() {
	memory_pool_allocator<char> c;
	memory_pool_allocator<Big> big(c);
	memory_pool_allocator<double> dbl(c);
	std::set<std::uintptr_t> seen;
	std::vector<void*> ptrs;
	for (int i = 0; i < 2000; ++i) {
		void* p = c.allocate(1);
		void* q = big.allocate(1);
		void* r = dbl.allocate(1);
		for (void* x : {p, q, r}) {
			CHECK(reinterpret_cast<std::uintptr_t>(x) % alignof(std::max_align_t) == 0);
			CHECK(seen.insert(reinterpret_cast<std::uintptr_t>(x)).second);
			ptrs.push_back(x);
		}
		std::memset(q, i, sizeof(Big));
	}
	for (std::size_t i = 0; i < ptrs.size(); i += 3) {
		c.deallocate(static_cast<char*>(ptrs[i]), 1);
		big.deallocate(static_cast<Big*>(ptrs[i + 1]), 1);
		dbl.deallocate(static_cast<double*>(ptrs[i + 2]), 1);
	}
	// Each size class recycles its own blocks, never another's.
	void* again = big.allocate(1);
	CHECK(again == ptrs[ptrs.size() - 2]);
	std::printf("pool OK: one family serves several block sizes\n");
}

// Exhaustive on a small domain: every pair (pool, T) from {A, B} x {int, long, Big}.
static void test_equality_matrix() {
	memory_pool_allocator<int> ia, ib;
	memory_pool_allocator<long> la(ia), lb(ib);
	memory_pool_allocator<Big> ba(la), bb(lb);
	const bool eq[6][6] = {
		// ia ib la lb ba bb
		{1, 0, 1, 0, 1, 0},
		{0, 1, 0, 1, 0, 1},
		{1, 0, 1, 0, 1, 0},
		{0, 1, 0, 1, 0, 1},
		{1, 0, 1, 0, 1, 0},
		{0, 1, 0, 1, 0, 1},
	};
	auto compare = [&](int i, int j) {
		auto pick = [&](int k, auto&& f) {
			switch (k) {
				case 0: return f(ia); case 1: return f(ib); case 2: return f(la);
				case 3: return f(lb); case 4: return f(ba); default: return f(bb);
			}
		};
		return pick(i, [&](auto& x) { return pick(j, [&](auto& y) { return x == y; }); });
	};
	for (int i = 0; i < 6; ++i)
		for (int j = 0; j < 6; ++j)
			CHECK(compare(i, j) == eq[i][j]);
	std::printf("pool OK: equality matrix over 2 pools x 3 types\n");
}

// propagate_on_container_*: nodes travel with the allocator; no element-wise fallback.
static void test_container_propagation() {
	using List = std::list<int, memory_pool_allocator<int>>;
	List a{1, 2, 3}, b{10, 20, 30, 40};
	CHECK(a.get_allocator() != b.get_allocator());
	const int* first_a = &a.front();
	const int* first_b = &b.front();

	a.swap(b);   // POCS: allocators swap with the nodes
	CHECK(&b.front() == first_a && &a.front() == first_b);
	CHECK(a.size() == 4 && b.size() == 3 && a.front() == 10 && b.front() == 1);
	a.push_back(50); b.push_back(4);   // both still allocate and free from their own (swapped) pools
	a.pop_front(); b.pop_front();
	CHECK(a.front() == 20 && b.front() == 2);

	List c{7, 8, 9};
	const int* first_c = &c.front();
	CHECK(c.get_allocator() != a.get_allocator());
	a = std::move(c);   // POCMA: takes c's nodes and allocator, no element moves
	CHECK(&a.front() == first_c && a.get_allocator() == c.get_allocator());
	CHECK(a.size() == 3 && a.back() == 9);

	List d{5, 6};
	List e{1};
	CHECK(d.get_allocator() != e.get_allocator());
	e = d;   // POCCA: e takes d's allocator, then copies the elements
	CHECK(e.get_allocator() == d.get_allocator());
	CHECK(e.size() == 2 && e.front() == 5);

	std::map<int, int, std::less<int>, memory_pool_allocator<std::pair<const int, int>>> m1{{1, 1}}, m2{{2, 2}, {3, 3}};
	m1.swap(m2);
	m1[4] = 4; m2[5] = 5;
	CHECK(m1.size() == 3 && m2.size() == 2);
	std::printf("pool OK: swap, move- and copy-assignment propagate the pool\n");
}

// A container copy starts its own pool (and so can be used on another thread);
// handing a container over (move) shares the pool with a happens-before edge.
static void test_cross_thread() {
	using List = std::list<long, memory_pool_allocator<long>>;
	List original;
	for (long i = 0; i < 1000; ++i) original.push_back(i);

	List copy = original;
	CHECK(copy.get_allocator() != original.get_allocator());

	long checksum = 0;
	std::thread worker([&] {
		for (int round = 0; round < 200; ++round) {
			for (long i = 0; i < 100; ++i) copy.push_back(i);
			for (long i = 0; i < 100; ++i) copy.pop_front();
		}
		for (long v : copy) checksum += v;
	});
	for (int round = 0; round < 200; ++round) {   // the original is used concurrently
		for (long i = 0; i < 100; ++i) original.push_front(-i);
		for (long i = 0; i < 100; ++i) original.pop_back();
	}
	worker.join();
	// The copy is a queue: what is left is the last 1000 pushed, ten rounds of 0..99.
	CHECK(checksum == 10 * 4950);
	CHECK(original.size() == 1000);

	// Move hand-over: the receiving thread owns the pool after join/start edges.
	List moved = std::move(original);
	long total = 0;
	std::thread other([&] {
		moved.push_back(1);
		for (long v : moved) total += v;
	});
	other.join();
	CHECK(total == -10 * 4950 + 1);   // the newest 1000 pushed at the front, plus the 1
	std::printf("pool OK: a container copy is independent; a moved container is handed over cleanly\n");
}

// The overlap detector, driven deterministically: hold the class "in use" and enter it again.
static std::atomic<int> g_misuse{0};
static void count_misuse() noexcept { g_misuse.fetch_add(1); }

static void test_overlap_detection() {
#ifdef ALLOCATORS_POOL_CHECK
	auto previous = allocators::set_pool_misuse_handler(count_misuse);
	allocators::detail::pool_class cls(32);
	void* a = cls.allocate();
	CHECK(g_misuse.load() == 0);          // ordinary use is silent
	CHECK(cls.try_enter());               // another user is inside the class...
	void* b = cls.allocate();             // ...so this overlaps
	CHECK(g_misuse.load() == 1);
	cls.deallocate(b);                    // the overlapping call's guard cleared the flag on exit
	CHECK(g_misuse.load() == 1);
	cls.deallocate(a);
	allocators::set_pool_misuse_handler(previous);
	std::printf("pool OK: overlapping use is detected (%d report)\n", g_misuse.load());
#else
	std::printf("pool SKIP: overlap check not compiled in\n");
#endif
}

int main() {
	test_rebind_identity();
	test_conversion_does_not_allocate();
	test_sizes_share_family();
	test_equality_matrix();
	test_container_propagation();
	test_cross_thread();
	test_overlap_detection();
	std::printf("all pool tests passed\n");
	return 0;
}
