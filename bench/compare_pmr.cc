// The pool allocator against what the standard library offers for the same job: malloc through
// std::allocator, std::pmr::unsynchronized_pool_resource and std::pmr::monotonic_buffer_resource.
// Not a test. Build with -DALLOCATORS_BUILD_BENCH=ON, run `allocators_compare_pmr`.
//
// Rows (a run builds its container, works it, and destroys it, with a fresh pool or resource each
// time; the time includes creating the pool):
//   list build        n push_backs, then a sum over the list, then destruction
//   map churn         n random inserts, erase the n/2 smallest keys, n/2 more random inserts
//   list steady       n push_backs, then 20 n rounds of push_back + pop_front
//
// Every arm of a row computes the same checksum, which is compared before any timing: a row whose
// arms disagree is void. Arms run round-robin and in reverse order on odd rounds; the first arm
// runs twice, and the gap between its two columns is the noise floor (A/A).
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <list>
#include <map>
#include <memory_resource>
#include <random>
#include <string>
#include <vector>

#include "allocators.h"

namespace {

template <class T> using PoolAlloc = allocators::memory_pool_allocator<T>;
using MapNode = std::pair<const int, int>;

using StdList = std::list<int>;
using PoolList = std::list<int, PoolAlloc<int>>;
using PmrList = std::pmr::list<int>;
using StdMap = std::map<int, int>;
using PoolMap = std::map<int, int, std::less<int>, PoolAlloc<MapNode>>;
using PmrMap = std::pmr::map<int, int>;

template <class L> std::uint64_t list_build(L&& l, int n) {
	for (int i = 0; i < n; ++i) { l.push_back(i); }
	std::uint64_t sum = 0;
	for (int v : l) { sum += static_cast<std::uint64_t>(v); }
	return sum;
}

template <class M> std::uint64_t map_churn(M&& m, int n) {
	std::mt19937 g(1);
	for (int i = 0; i < n; ++i) { m[static_cast<int>(g())] = i; }
	for (int i = 0; i < n / 2; ++i) { m.erase(m.begin()); }
	for (int i = 0; i < n / 2; ++i) { m[static_cast<int>(g())] = i; }
	std::uint64_t sum = m.size();
	for (const auto& kv : m) { sum += static_cast<std::uint64_t>(kv.second); }
	return sum;
}

template <class L> std::uint64_t list_steady(L&& l, int n) {
	for (int i = 0; i < n; ++i) { l.push_back(i); }
	for (int i = 0; i < 20 * n; ++i) { l.push_back(i); l.pop_front(); }
	std::uint64_t sum = 0;
	for (int v : l) { sum += static_cast<std::uint64_t>(v); }
	return sum;
}

struct Arm {
	std::string name;
	std::function<std::uint64_t()> run;
};

double median(std::vector<double> v) {
	std::sort(v.begin(), v.end());
	return v[v.size() / 2];
}

void row(const std::string& title, std::vector<Arm> arms, int iters, int rounds) {
	const std::uint64_t expected = arms[0].run();
	for (const auto& arm : arms) {
		if (arm.run() != expected) {
			std::fprintf(stderr, "%s: arm \"%s\" disagrees with \"%s\"; the row is void\n", title.c_str(), arm.name.c_str(), arms[0].name.c_str());
			std::exit(1);
		}
	}
	arms.insert(arms.begin() + 1, Arm{arms[0].name + " (A/A)", arms[0].run});

	std::vector<std::vector<double>> samples(arms.size());
	for (int r = 0; r < rounds; ++r) {
		for (std::size_t k = 0; k < arms.size(); ++k) {
			const std::size_t i = r % 2 == 0 ? k : arms.size() - 1 - k;
			std::uint64_t sink = 0;
			const auto t0 = std::chrono::steady_clock::now();
			for (int it = 0; it < iters; ++it) { sink += arms[i].run(); }
			const auto t1 = std::chrono::steady_clock::now();
			if (sink == 1) { std::puts(""); }   // keep the work observable
			samples[i].push_back(std::chrono::duration<double, std::micro>(t1 - t0).count() / iters);
		}
	}

	std::printf("\n%s (median of %d rounds, microseconds per run)\n", title.c_str(), rounds);
	const double base = median(samples[0]);
	for (std::size_t i = 0; i < arms.size(); ++i) {
		const double mid = median(samples[i]);
		const double lo = *std::min_element(samples[i].begin(), samples[i].end());
		const double hi = *std::max_element(samples[i].begin(), samples[i].end());
		std::printf("  %-44s %11.1f  spread %.2f  %.2fx of malloc\n", arms[i].name.c_str(), mid, hi / lo, mid / base);
	}
}

}  // namespace

int main(int argc, char** argv) {
	const bool quick = argc > 1 && std::string(argv[1]) == "--quick";
	const int rounds = quick ? 3 : 9;

	for (int n : {1000, 100000}) {
		const int iters = n == 1000 ? (quick ? 200 : 2000) : (quick ? 3 : 20);
		const std::string size = " (n = " + std::to_string(n) + ")";

		row("list build" + size, {
			{"std::allocator (malloc)", [n] { return list_build(StdList(), n); }},
			{"memory_pool_allocator", [n] { return list_build(PoolList(), n); }},
			{"pmr unsynchronized_pool_resource", [n] {
				std::pmr::unsynchronized_pool_resource mr;
				return list_build(PmrList(&mr), n);
			}},
			{"pmr monotonic_buffer_resource", [n] {
				std::pmr::monotonic_buffer_resource mr;
				return list_build(PmrList(&mr), n);
			}},
		}, iters, rounds);

		row("map churn" + size, {
			{"std::allocator (malloc)", [n] { return map_churn(StdMap(), n); }},
			{"memory_pool_allocator", [n] { return map_churn(PoolMap(), n); }},
			{"pmr unsynchronized_pool_resource", [n] {
				std::pmr::unsynchronized_pool_resource mr;
				return map_churn(PmrMap(&mr), n);
			}},
		}, iters, rounds);

		const int steady_iters = std::max(1, iters / (n == 1000 ? 10 : 1));
		row("list steady" + size, {
			{"std::allocator (malloc)", [n] { return list_steady(StdList(), n); }},
			{"memory_pool_allocator", [n] { return list_steady(PoolList(), n); }},
			{"pmr unsynchronized_pool_resource", [n] {
				std::pmr::unsynchronized_pool_resource mr;
				return list_steady(PmrList(&mr), n);
			}},
		}, steady_iters, rounds);
	}
	return 0;
}
