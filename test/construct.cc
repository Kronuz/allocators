// construct/destroy forwarding: containers using vanilla_allocator and
// memory_pool_allocator must copy and move exactly as with std::allocator.
// The oracle is std::allocator: same operation counts for the same program.
// CONSTRUCT_REVERT_CHECK drops the checks that need the forwarding construct to even compile, so
// the rest can be built against the previous header and shown to fail there.
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "allocators.h"

#define CHECK(...) do { if (!(__VA_ARGS__)) { std::fprintf(stderr, "CHECK failed: %s (line %d)\n", #__VA_ARGS__, __LINE__); std::abort(); } } while (0)

template <bool Nothrow>
struct Counted {
	static inline int copies = 0, moves = 0, dtors = 0;
	static void reset() { copies = moves = dtors = 0; }
	int v;
	Counted(int x) : v(x) {}
	Counted(int a, int b) : v(a + b) {}
	Counted(const Counted& o) : v(o.v) { ++copies; }
	Counted(Counted&& o) noexcept(Nothrow) : v(o.v) { ++moves; }
	Counted& operator=(const Counted& o) { v = o.v; ++copies; return *this; }
	Counted& operator=(Counted&& o) noexcept(Nothrow) { v = o.v; ++moves; return *this; }
	~Counted() { ++dtors; }
};

struct Result { int copies, moves, dtors; };
template <class C> Result snap() { return {C::copies, C::moves, C::dtors}; }
static bool same(Result a, Result b) { return a.copies == b.copies && a.moves == b.moves && a.dtors == b.dtors; }

template <class T> using Std = std::allocator<T>;
template <class T> using Vanilla = allocators::allocator<T, allocators::vanilla_allocator>;
template <class T> using Pool = allocators::memory_pool_allocator<T>;

template <class C, template <class> class A>
Result run_vector() {
	C::reset();
	{ std::vector<C, A<C>> v; for (int i = 0; i < 1000; ++i) v.push_back(C(i)); for (int i = 0; i < 1000; ++i) CHECK(v[i].v == i); }
	return snap<C>();
}
template <class C, template <class> class A>
Result run_list() {
	C::reset();
	{ std::list<C, A<C>> l; for (int i = 0; i < 100; ++i) { l.push_back(C(i)); l.emplace_front(i); } }
	return snap<C>();
}
template <class C, template <class> class A>
Result run_map() {
	C::reset();
	{
		std::map<int, C, std::less<int>, A<std::pair<const int, C>>> m;
		for (int i = 0; i < 100; ++i) { m.emplace(i, C(i)); m.insert({i + 1000, C(i)}); }
	}
	return snap<C>();
}

template <class C, template <class> class A>
Result run_deque() {
	C::reset();
	{ std::deque<C, A<C>> d; for (int i = 0; i < 300; ++i) { d.push_back(C(i)); d.emplace_front(i, 1); } }
	return snap<C>();
}
// emplace_back with several arguments, insert in the middle, resize, assign and copy.
template <class C, template <class> class A>
Result run_vector_ops() {
	C::reset();
	{
		std::vector<C, A<C>> v;
		for (int i = 0; i < 50; ++i) { v.emplace_back(i, 2); }
		v.insert(v.begin() + 10, C(7));
		v.insert(v.begin() + 20, 5, C(9));
		v.resize(120, C(3));
		std::vector<C, A<C>> w(v);
		w.erase(w.begin() + 3, w.begin() + 30);
		v.assign(w.begin(), w.end());
		v.shrink_to_fit();
	}
	return snap<C>();
}
template <class C, template <class> class A>
Result run_umap() {
	C::reset();
	{
		std::unordered_map<int, C, std::hash<int>, std::equal_to<int>, A<std::pair<const int, C>>> m;
		for (int i = 0; i < 200; ++i) { m.emplace(i, C(i)); m.try_emplace(i + 500, i, 1); }
	}
	return snap<C>();
}

// A type whose copies and moves throw once a shared budget runs out: the container must end up
// in the same state (size, live objects, operation counts) with our allocator as with std's.
template <bool Nothrow>
struct Boom {
	static inline int budget = 0, live = 0, copies = 0, moves = 0;
	int v;
	static void tick() { if (budget-- <= 0) throw std::runtime_error("boom"); }
	explicit Boom(int x) : v(x) { ++live; }
	Boom(const Boom& o) : v(o.v) { tick(); ++copies; ++live; }
	Boom(Boom&& o) noexcept(Nothrow) : v(o.v) { if constexpr (!Nothrow) { tick(); } ++moves; ++live; }
	~Boom() { --live; }
};
struct Outcome { std::size_t size; int live, copies, moves; bool threw; };
template <class C, template <class> class A>
Outcome run_boom(int budget) {
	C::budget = budget; C::live = C::copies = C::moves = 0;
	Outcome o{};
	try {
		std::vector<C, A<C>> v;
		try { for (int i = 0; i < 100; ++i) { v.push_back(C(i)); } } catch (const std::runtime_error&) { o.threw = true; }
		o.size = v.size();
		o.live = C::live;
		for (std::size_t i = 0; i < v.size(); ++i) { CHECK(v[i].v == int(i)); }
	} catch (...) { std::abort(); }
	CHECK(C::live == 0);  // nothing leaked, nothing destroyed twice
	o.copies = C::copies; o.moves = C::moves;
	return o;
}
template <class C>
void compare_boom() {
	for (int budget = 0; budget < 400; budget += 7) {
		Outcome a = run_boom<C, Vanilla>(budget), b = run_boom<C, Std>(budget);
		CHECK(a.size == b.size && a.live == b.live && a.copies == b.copies && a.moves == b.moves && a.threw == b.threw);
	}
}


template <class C, template <class> class A>
void compare() {
	if constexpr (!std::is_same_v<A<C>, Pool<C>>) {  // the pool serves single nodes only
		CHECK(same(run_vector<C, A>(), run_vector<C, Std>()));
	}
	CHECK(same(run_list<C, A>(), run_list<C, Std>()));
	CHECK(same(run_map<C, A>(), run_map<C, Std>()));
	if constexpr (!std::is_same_v<A<C>, Pool<C>>) {
		CHECK(same(run_vector_ops<C, A>(), run_vector_ops<C, Std>()));
		CHECK(same(run_deque<C, A>(), run_deque<C, Std>()));
		CHECK(same(run_umap<C, A>(), run_umap<C, Std>()));
	}
}

int main() {
	// Nothrow move: reallocation must move, never copy.
	compare<Counted<true>, Vanilla>();
	compare<Counted<true>, Pool>();
	CHECK(run_vector<Counted<true>, Vanilla>().copies == 0);
	CHECK(run_list<Counted<true>, Pool>().copies == 0);
	CHECK(run_map<Counted<true>, Pool>().copies == 0);
	// Throwing move: vector falls back to copy on reallocation, same as std.
	compare<Counted<false>, Vanilla>();
	compare<Counted<false>, Pool>();
	// A throw in the middle of a reallocation leaves the same vector as with std::allocator.
	compare_boom<Boom<true>>();
	compare_boom<Boom<false>>();

	// std::string: moved-from source is left empty (moved, not copied).
	{
		std::vector<std::string, Vanilla<std::string>> v;
		std::string s(100, 'x');
		v.push_back(std::move(s));
		CHECK(s.empty());
		std::list<std::string, Pool<std::string>> p;
		std::string t(100, 'y');
		p.push_back(std::move(t));
		CHECK(t.empty());
	}
	// Through allocator_traits, as vector does: an rvalue is moved, an lvalue copied. Node containers
	// rebind to a node type and construct through the default, so only these calls reach the pool's construct.
	{
		using C = Counted<true>;
		Pool<C> pool;
		using Traits = std::allocator_traits<Pool<C>>;
		C* p = pool.allocate(1);
		C::reset();
		C src(5);
		Traits::construct(pool, p, std::move(src));
		CHECK(C::moves == 1 && C::copies == 0 && p->v == 5);
		pool.destroy(p);
		Traits::construct(pool, p, static_cast<const C&>(src));
		CHECK(C::moves == 1 && C::copies == 1);
		pool.destroy(p);
		pool.deallocate(p, 1);

		Vanilla<C> v;
		using VTraits = std::allocator_traits<Vanilla<C>>;
		C* q = v.allocate(1);
		C::reset();
		VTraits::construct(v, q, std::move(src));
		CHECK(C::moves == 1 && C::copies == 0);
		v.destroy(q);
		v.deallocate(q, 1);
	}
#ifndef CONSTRUCT_REVERT_CHECK
	// noexcept follows the element's constructor.
	{
		using N = Counted<true>;
		using T = Counted<false>;
		static_assert(noexcept(std::declval<Vanilla<N>&>().construct(std::declval<N*>(), std::declval<N&&>())));
		static_assert(!noexcept(std::declval<Vanilla<T>&>().construct(std::declval<T*>(), std::declval<T&&>())));
		static_assert(noexcept(std::declval<Pool<N>&>().construct(std::declval<N*>(), std::declval<N&&>())));
		static_assert(!noexcept(std::declval<Pool<T>&>().construct(std::declval<T*>(), std::declval<T&&>())));
	}
#endif
	// Direct calls with a const T& and with constructor arguments still work.
	{
		Vanilla<std::string> a;
		std::string* p = a.allocate(2);
		const std::string src("abc");
		a.construct(p, src);
#ifndef CONSTRUCT_REVERT_CHECK
		a.construct(p + 1, 3, 'z');
		CHECK(p[0] == "abc" && p[1] == "zzz");
#else
		a.construct(p + 1, src);
#endif
		a.destroy(p); a.destroy(p + 1);
		a.deallocate(p, 2);
	}
	std::puts("construct OK");
}
