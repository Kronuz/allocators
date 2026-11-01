// memory_pool_allocator through container copy, move and swap.
//
// The oracle is the C++ Allocator requirements and [container.reqmts] plus the README
// contract: a container copy gets its own pool (copy construction through
// select_on_container_copy_construction, copy assignment because
// propagate_on_container_copy_assignment is false, so the target keeps its allocator);
// move assignment and swap move the nodes together with the allocator
// (propagate_on_container_{move_assignment,swap} are true, is_always_equal is false);
// `a == b` holds exactly when storage obtained from one can be freed through the other.
//
// Pool identity is observed without looking inside the pool: the free list is LIFO, so a
// block freed through x is the next one handed out by y exactly when x and y draw on one
// pool. Every comparison below checks `==` against that observation.
//
// The pool serves one element per allocation, so node containers (list, forward_list,
// set, multiset, map, multimap) get the full matrix; vector, deque and unordered_map ask
// for arrays and are refused with bad_alloc beyond one element, which they must survive.
// The stateless allocator<T, vanilla_allocator> is run through the same five containers.
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <forward_list>
#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "allocators.h"

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "CHECK failed: %s (line %d)\n", #cond, __LINE__); std::abort(); } } while (0)

template <class T> using Pool = allocators::memory_pool_allocator<T>;
template <class T> using Vanilla = allocators::allocator<T, allocators::vanilla_allocator>;

// True when x and y draw on one pool; also checks that `==` and `!=` say the same.
template <class A>
static bool shares(A x, A y) {
	auto* p = x.allocate(1);
	x.deallocate(p, 1);
	auto* q = y.allocate(1);
	y.deallocate(q, 1);
	const bool same = static_cast<void*>(p) == static_cast<void*>(q);
	CHECK((x == y) == same);
	CHECK((x != y) == !same);
	return same;
}

using List = std::list<int, Pool<int>>;
using Fwd = std::forward_list<int, Pool<int>>;
using Set = std::set<int, std::less<int>, Pool<int>>;
using MSet = std::multiset<int, std::less<int>, Pool<int>>;
using Map = std::map<int, int, std::less<int>, Pool<std::pair<const int, int>>>;
using MMap = std::multimap<int, int, std::less<int>, Pool<std::pair<const int, int>>>;

// One small vocabulary over all six node containers.
static void add(List& c, int v) { c.push_back(v); }
static void add(Fwd& c, int v) { c.push_front(v); }
static void add(Set& c, int v) { c.insert(v); }
static void add(MSet& c, int v) { c.insert(v); }
static void add(Map& c, int v) { c.emplace(v, v); }
static void add(MMap& c, int v) { c.emplace(v, v); }

static void drop_first(List& c) { c.pop_front(); }
static void drop_first(Fwd& c) { c.pop_front(); }
template <class C> static void drop_first(C& c) { c.erase(c.begin()); }

// Move every node of `from` into `to` (splice and merge need equal allocators).
static void join(List& to, List& from) { to.splice(to.end(), from); }
static void join(Fwd& to, Fwd& from) { to.splice_after(to.before_begin(), from); }
template <class C> static void join(C& to, C& from) { to.merge(from); }

static int key(int v) { return v; }
static int key(const std::pair<const int, int>& v) { return v.first; }

template <class C> static std::vector<int> values(const C& c) {
	std::vector<int> out;
	for (const auto& v : c) out.push_back(key(v));
	return out;
}
template <class C> static std::vector<const void*> nodes(const C& c) {
	std::vector<const void*> out;
	for (const auto& v : c) out.push_back(std::addressof(v));
	return out;
}
template <class C> static C make(int base, int n) {
	C c;
	for (int i = 0; i < n; ++i) add(c, base + i);
	return c;
}
// Nodes come and go: add one, drop the oldest/smallest, keeping the size constant.
template <class C> static void churn(C& c, int rounds, int& next) {
	for (int i = 0; i < rounds; ++i) { add(c, next++); drop_first(c); }
}

template <class C>
static void test_node_container(const char* name) {
	using A = typename C::allocator_type;
	using Traits = std::allocator_traits<A>;
	static_assert(!Traits::propagate_on_container_copy_assignment::value, "a copy-assigned container keeps its pool");
	static_assert(Traits::propagate_on_container_move_assignment::value, "move assignment takes the pool with the nodes");
	static_assert(Traits::propagate_on_container_swap::value, "swap exchanges the pools with the nodes");
	static_assert(!Traits::is_always_equal::value, "pool allocators are stateful");
	int next = 100000;

	{   // Copy construction: a new pool, the same elements, no coupling.
		C a = make<C>(0, 40);
		const auto before = values(a);
		C b(a);
		CHECK(!shares(a.get_allocator(), b.get_allocator()));
		CHECK(values(b) == before);
		churn(b, 300, next);
		CHECK(values(a) == before);
	}
	{   // Copy assignment into a container with another pool: the target keeps its own.
		C d = make<C>(0, 40), e = make<C>(1000, 5);
		const A d_alloc = d.get_allocator(), e_alloc = e.get_allocator();
		CHECK(!shares(d_alloc, e_alloc));
		e = d;
		CHECK(shares(e.get_allocator(), e_alloc));
		CHECK(!shares(e.get_allocator(), d_alloc));
		CHECK(shares(d.get_allocator(), d_alloc));
		CHECK(values(e) == values(d));
		const auto before = values(d);
		churn(e, 300, next);
		CHECK(values(d) == before);
		churn(d, 300, next);
		d = e;                      // and back, with the sizes the other way round
		CHECK(shares(d.get_allocator(), d_alloc));
		CHECK(values(d) == values(e));
		const C& self = e;
		const auto same = values(e);
		e = self;                   // self assignment
		CHECK(values(e) == same && shares(e.get_allocator(), e_alloc));
	}
	{   // Move construction: the new container owns the nodes and an equal allocator; the
		// source's allocator is unchanged and still equal (the Allocator move requirement).
		C a = make<C>(0, 40);
		const A a0 = a.get_allocator();
		const auto before = values(a);
		const auto addresses = nodes(a);
		C c(std::move(a));
		CHECK(shares(c.get_allocator(), a0));
		CHECK(shares(a.get_allocator(), a0));
		CHECK(nodes(c) == addresses && values(c) == before);
		a.clear();                  // the moved-from container is valid and keeps the family
		for (int i = 0; i < 20; ++i) add(a, i);
		churn(c, 100, next);
		churn(a, 100, next);
	}
	{   // Move assignment: the pool travels with the nodes; no element-wise fallback.
		C a = make<C>(0, 40), b = make<C>(1000, 5);
		const A a0 = a.get_allocator(), b0 = b.get_allocator();
		const auto before = values(a);
		const auto addresses = nodes(a);
		b = std::move(a);
		CHECK(shares(b.get_allocator(), a0));
		CHECK(!shares(b.get_allocator(), b0));
		CHECK(nodes(b) == addresses && values(b) == before);
		churn(b, 300, next);
	}
	{   // Swap: pools and nodes trade places.
		C a = make<C>(0, 40), b = make<C>(1000, 7);
		const A a0 = a.get_allocator(), b0 = b.get_allocator();
		const auto addresses_a = nodes(a), addresses_b = nodes(b);
		const auto values_a = values(a), values_b = values(b);
		a.swap(b);
		CHECK(shares(a.get_allocator(), b0) && shares(b.get_allocator(), a0));
		CHECK(!shares(a.get_allocator(), b.get_allocator()));
		CHECK(nodes(a) == addresses_b && nodes(b) == addresses_a);
		CHECK(values(a) == values_b && values(b) == values_a);
		churn(a, 300, next);
		churn(b, 300, next);
	}
	{   // A copy made from a moved-from container keeps that container's pool (it is a
		// copy assignment: the target's allocator stays), which it still shares with the
		// container that took the nodes. This is the documented hand-over rule.
		C a = make<C>(0, 40);
		C taken(std::move(a));
		C twin = make<C>(5000, 3);
		CHECK(!shares(twin.get_allocator(), taken.get_allocator()));
		a = twin;
		CHECK(shares(a.get_allocator(), taken.get_allocator()));
		CHECK(values(a) == values(twin));
		a = make<C>(7000, 3);       // a move assignment from a fresh container: a gets its own pool
		CHECK(!shares(a.get_allocator(), taken.get_allocator()));
	}
	{   // Neither `a = {}` nor clear() gives a moved-from container a pool of its own: both keep
		// the family it shares with the container that took its nodes. Containers that share a
		// pool have equal allocators, so splice and merge between them are allowed.
		C a = make<C>(0, 40);
		C taken(std::move(a));
		a = {};
		CHECK(a.empty() && shares(a.get_allocator(), taken.get_allocator()));
		add(a, 1);
		a.clear();
		CHECK(a.empty() && shares(a.get_allocator(), taken.get_allocator()));
		for (int i = 0; i < 3; ++i) add(a, 9000 + i);
		const std::size_t before = values(taken).size() + values(a).size();
		join(taken, a);
		CHECK(a.empty() && values(taken).size() == before);
		churn(taken, 100, next);
		CHECK(values(taken).size() == before);
	}
	{   // Separate pools compare unequal, so splice and merge between them are outside the
		// container requirements: a copy and its source, and a copy-assigned pair.
		C d = make<C>(0, 10), e = make<C>(1000, 5);
		CHECK(!(d.get_allocator() == e.get_allocator()));
		e = d;
		CHECK(!(d.get_allocator() == e.get_allocator()));
		C copy(d);
		CHECK(!(d.get_allocator() == copy.get_allocator()));
	}
	{   // The failure the contract rules out: after a copy assignment, two threads each
		// churn their own container. Run under ASan and TSan; with a shared pool this is a
		// data race on the free list and corrupts the heap.
		C d = make<C>(0, 1000), e = make<C>(50000, 3);
		e = d;
		int next_d = 1 << 24, next_e = 1 << 25;
		std::thread worker([&] { churn(d, 20000, next_d); });
		churn(e, 20000, next_e);
		worker.join();
		CHECK(values(d).size() == 1000 && values(e).size() == 1000);
	}
	std::printf("propagation OK: %s\n", name);
}

// Containers that ask for arrays: the pool refuses them with bad_alloc, which must leave
// the container valid, and the allocator rules still hold on whatever can be built.
template <class C, class Seed, class Grow>
static void test_array_container(const char* name, bool must_build, Seed seed, Grow grow) {
	using A = typename C::allocator_type;
	using Traits = std::allocator_traits<A>;
	static_assert(!Traits::propagate_on_container_copy_assignment::value);
	static_assert(Traits::propagate_on_container_move_assignment::value);
	static_assert(Traits::propagate_on_container_swap::value);
	static_assert(!Traits::is_always_equal::value);

	std::unique_ptr<C> a, b, d;
	try {
		a = std::make_unique<C>();
		b = std::make_unique<C>();
		d = std::make_unique<C>();
	} catch (const std::bad_alloc&) {
		CHECK(!must_build);          // libstdc++'s deque allocates its map array on construction
		std::printf("propagation OK: %s (construction refused with bad_alloc)\n", name);
		return;
	}
	seed(*a); seed(*b); seed(*d);
	const A a0 = a->get_allocator(), b0 = b->get_allocator(), d0 = d->get_allocator();
	CHECK(!shares(a0, b0) && !shares(a0, d0) && !shares(b0, d0));
	{
		C c(*a);                                    // copy construction: a new pool
		CHECK(!shares(c.get_allocator(), a0));
	}
	*b = *a;                                        // copy assignment: b keeps its pool
	CHECK(shares(b->get_allocator(), b0) && !shares(b->get_allocator(), a0));
	{
		C c(std::move(*a));                         // move construction: the pool goes along
		CHECK(shares(c.get_allocator(), a0) && shares(a->get_allocator(), a0));
		*d = std::move(c);                          // move assignment: d takes it over
		CHECK(shares(d->get_allocator(), a0) && !shares(d->get_allocator(), d0));
	}
	d->swap(*b);                                    // swap: the pools trade places
	CHECK(shares(d->get_allocator(), b0) && shares(b->get_allocator(), a0));
	bool refused = false;
	try { grow(*d); } catch (const std::bad_alloc&) { refused = true; }
	CHECK(refused);
	CHECK(shares(d->get_allocator(), b0));          // still usable after the refusal
	std::printf("propagation OK: %s\n", name);
}

// The stateless allocator: every instance is equal, so nothing propagates and nothing needs to.
template <class C, class Fill>
static void test_stateless_container(const char* name, Fill fill) {
	using A = typename C::allocator_type;
	static_assert(std::allocator_traits<A>::is_always_equal::value);
	C a, b;
	fill(a, 0, 300);
	fill(b, 1000, 50);
	CHECK(a.get_allocator() == b.get_allocator() && !(a.get_allocator() != b.get_allocator()));
	C c(a);
	CHECK(c == a);
	b = a;
	CHECK(b == a);
	C m(std::move(c));
	CHECK(m == a);
	C n;
	fill(n, 5000, 9);
	n = std::move(m);
	CHECK(n == a);
	C x, y;
	fill(x, 0, 120);
	fill(y, 7000, 11);
	C x0(x), y0(y);
	x.swap(y);
	CHECK(x == y0 && y == x0);
	std::printf("propagation OK: allocators::allocator over %s\n", name);
}

static void test_allocator_move_requirement() {
	// A a(std::move(a1)): a1 is unchanged and equal to a; the same for move assignment.
	Pool<int> a1;
	Pool<int> a(std::move(a1));
	CHECK(shares(a, a1));
	Pool<int> other;
	CHECK(!shares(other, a));
	other = std::move(a1);
	CHECK(shares(other, a) && shares(other, a1));
	Pool<long> converted(a1);
	Pool<int> back(converted);
	CHECK(back == a1 && back == converted && converted == a1);
	std::printf("propagation OK: allocator move and conversion keep the family\n");
}

int main() {
	test_allocator_move_requirement();
	test_node_container<List>("std::list");
	test_node_container<Fwd>("std::forward_list");
	test_node_container<Set>("std::set");
	test_node_container<MSet>("std::multiset");
	test_node_container<Map>("std::map");
	test_node_container<MMap>("std::multimap");

	using Vec = std::vector<int, Pool<int>>;
	using Deq = std::deque<int, Pool<int>>;
	using Umap = std::unordered_map<int, int, std::hash<int>, std::equal_to<int>, Pool<std::pair<const int, int>>>;
	test_array_container<Vec>("std::vector", true,
		[](Vec& v) { v.assign(1, 7); },
		[](Vec& v) { v.push_back(8); v.push_back(9); });
	test_array_container<Deq>("std::deque", false,
		[](Deq&) {},
		[](Deq& v) { v.push_back(1); });
	test_array_container<Umap>("std::unordered_map", false,
		[](Umap&) {},
		[](Umap& v) { v.emplace(1, 1); });

	using V = std::vector<int, Vanilla<int>>;
	using L = std::list<int, Vanilla<int>>;
	using M = std::map<int, int, std::less<int>, Vanilla<std::pair<const int, int>>>;
	using U = std::unordered_map<int, int, std::hash<int>, std::equal_to<int>, Vanilla<std::pair<const int, int>>>;
	using D = std::deque<int, Vanilla<int>>;
	test_stateless_container<V>("std::vector", [](V& c, int base, int n) { for (int i = 0; i < n; ++i) c.push_back(base + i); });
	test_stateless_container<L>("std::list", [](L& c, int base, int n) { for (int i = 0; i < n; ++i) c.push_back(base + i); });
	test_stateless_container<M>("std::map", [](M& c, int base, int n) { for (int i = 0; i < n; ++i) c.emplace(base + i, i); });
	test_stateless_container<U>("std::unordered_map", [](U& c, int base, int n) { for (int i = 0; i < n; ++i) c.emplace(base + i, i); });
	test_stateless_container<D>("std::deque", [](D& c, int base, int n) { for (int i = 0; i < n; ++i) c.push_back(base + i); });
	std::printf("all propagation tests passed\n");
	return 0;
}
