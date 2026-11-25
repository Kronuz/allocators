// memory_pool_allocator<Node>::allocate(1) and deallocate called directly on one allocator: no
// container and no allocator copies. One row per line, "<row> <value> ns" (ns per operation, best
// of 9), so ab_compare.py reads it with --format unit; std::allocator is the control.
//
//   steady             allocate then deallocate of one block, over and over: the free list holds
//                      one block and every allocate takes what the previous deallocate pushed; once
//                      on one pool, once as the median of nine pools placed differently in memory
//   refill             n blocks allocated, then freed in allocation order, again (the free list
//                      refills; no new chunks after the first pass); n = 2 is the smallest list
//   fresh pool         a new pool per round: chunk growth, bump allocation, destruction
//   random order       100000 blocks freed in random order
//
// Uses only the allocate/deallocate pair of the pool, so it builds against any revision of the
// library (it compares revisions: build it once per source tree with -I <tree>).
//
//   c++ -std=c++20 -O3 -DNDEBUG -pthread -I <repo> bench/pool_bench.cc -o pool_bench
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <random>

// Benchmark, not a test: Xapiand's global operator new and delete under ALLOCATORS_TRACKED_MEM, many
// small allocations from several threads, one "row value ns" line per row (best of five, per
// new/delete pair, wall time over all threads).
#include "allocators.h"
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

static void *volatile sink;

int main() {
	constexpr int ops = 4000000;
	for (unsigned threads : {1u, 4u, 8u}) {
		const int each = ops / int(threads);
		double best = 1e18;
		for (int rep = 0; rep < 6; ++rep) {
			auto a = std::chrono::steady_clock::now();
			std::vector<std::thread> pool;
			for (unsigned t = 0; t < threads; ++t)
				pool.emplace_back([each, t] {
					std::vector<char *> live(64);
					for (int i = 0; i < each; ++i) {
						auto &slot = live[std::size_t(i) & 63];
						delete[] slot;
						slot = new char[8 + ((std::size_t(i) * 37 + t) & 247)];
						sink = slot;
					}
					for (auto p : live) delete[] p;
				});
			for (auto &thread : pool) thread.join();
			auto b = std::chrono::steady_clock::now();
			if (rep)
				best = std::min(best, std::chrono::duration<double, std::nano>(b - a).count() / ops);
		}
		std::printf("tracked new+delete small, %u threads %.3f ns\n", threads, best);
	}
	return allocators::total_allocated() < 0;
}
