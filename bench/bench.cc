// vector push_back through std::allocator and through allocators::allocator<T, vanilla_allocator>.
//
//   allocators_bench           time both, interleaved A / B / A, best of 11 (the second A is the A/A spread)
//   allocators_bench --counts  only the copy and move counts of 1000 push_back(T&&), no timing
//
// For a trivially copyable element the two tie. For std::string the allocator must move elements
// when the vector reallocates, exactly as std::allocator does; the counts are the deterministic check.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "allocators.h"

template <class T>
using Vanilla = allocators::allocator<T, allocators::vanilla_allocator>;

struct Counted {
	static inline int copies = 0, moves = 0;
	int v;
	explicit Counted(int x) : v(x) {}
	Counted(const Counted& o) : v(o.v) { ++copies; }
	Counted(Counted&& o) noexcept : v(o.v) { ++moves; }
};

template <class A>
void count(const char* name) {
	Counted::copies = Counted::moves = 0;
	{
		std::vector<Counted, typename std::allocator_traits<A>::template rebind_alloc<Counted>> v;
		for (int i = 0; i < 1000; ++i) { v.push_back(Counted(i)); }
	}
	std::printf("  %-22s copies=%d moves=%d\n", name, Counted::copies, Counted::moves);
}

volatile std::size_t sink;

template <class F>
double best_ns_per_item(F&& f, std::size_t items, int rounds) {
	double best = 1e300;
	for (int i = 0; i < rounds; ++i) {
		auto t0 = std::chrono::steady_clock::now();
		f();
		auto t1 = std::chrono::steady_clock::now();
		double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / double(items);
		if (ns < best) { best = ns; }
	}
	return best;
}

template <class T, class Make>
void row(const char* what, std::size_t n, std::size_t reps, Make make) {
	auto ours = [&] { for (std::size_t r = 0; r < reps; ++r) { std::vector<T, Vanilla<T>> v; for (std::size_t i = 0; i < n; ++i) { v.push_back(make(i)); } sink = sink + v.size(); } };
	auto std_ = [&] { for (std::size_t r = 0; r < reps; ++r) { std::vector<T> v; for (std::size_t i = 0; i < n; ++i) { v.push_back(make(i)); } sink = sink + v.size(); } };
	std::size_t items = reps * n;
	double a1 = best_ns_per_item(ours, items, 11), s = best_ns_per_item(std_, items, 11), a2 = best_ns_per_item(ours, items, 11);
	std::printf("  %-26s n=%-7zu allocators %7.2f ns  std %7.2f ns  ratio %.2f  (A/A %.2f)\n", what, n, a1, s, a1 / s, a2 / a1);
}

int main(int argc, char** argv) {
	std::printf("copy and move counts, 1000 push_back(T&&):\n");
	count<std::allocator<Counted>>("std::allocator");
	count<Vanilla<Counted>>("vanilla_allocator");
	if (argc > 1 && std::strcmp(argv[1], "--counts") == 0) { return 0; }

	std::printf("push_back, ns per element (ratio above 1 means allocators is slower):\n");
	for (std::size_t n : {16, 1000, 100000}) {
		row<int>("vector<int>", n, 2000000 / n, [](std::size_t i) { return int(i); });
	}
	for (std::size_t n : {16, 1000, 20000}) {
		row<std::string>("vector<string> push_back(&&)", n, 200000 / n + 1, [](std::size_t i) { return std::string("a string long enough to leave small-string territory ") + std::to_string(i); });
	}
}
