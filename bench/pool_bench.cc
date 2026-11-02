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
#include <vector>

#include "allocators.h"

struct Node { void* a; void* b; long c; };
using Pool = allocators::memory_pool_allocator<Node>;
static void* volatile sink;

template <class F>
static double best_ns(F&& f, double ops, int reps = 9) {
	f();
	double best = 1e18;
	for (int i = 0; i < reps; ++i) {
		auto a = std::chrono::steady_clock::now();
		f();
		auto b = std::chrono::steady_clock::now();
		best = std::min(best, std::chrono::duration<double, std::nano>(b - a).count() / ops);
	}
	return best;
}

int main() {
	constexpr int steady_ops = 4000000;

	// steady state: one block reused through the free list
	{
		Pool p;
		double t = best_ns([&] { for (int i = 0; i < steady_ops; ++i) { Node* n = p.allocate(1); sink = n; p.deallocate(n, 1); } }, steady_ops);
		std::printf("alloc+free steady (free list) %.3f ns\n", t);
	}
	// the same on nine pools, each placed differently in memory by a padding allocation before it;
	// the median of the nine. A single pool's time depends on where its blocks land (an Apple M4 Pro
	// loop runs in 0.4 or 1.15 ns from one process to the next), so one pool is a coin flip.
	{
		std::vector<std::unique_ptr<char[]>> padding;
		std::vector<double> times;
		for (int k = 0; k < 9; ++k) {
			padding.emplace_back(new char[24 + 16 * k]);
			Pool p;
			times.push_back(best_ns([&] { for (int i = 0; i < steady_ops; ++i) { Node* n = p.allocate(1); sink = n; p.deallocate(n, 1); } }, steady_ops, 5));
		}
		std::sort(times.begin(), times.end());
		std::printf("alloc+free steady, median of 9 pool placements %.3f ns\n", times[4]);
	}
	// build n blocks, free them in allocation order, again (free list refill, no new chunks)
	for (int n : {2, 1000, 100000}) {
		Pool p;
		std::vector<Node*> v(n);
		int reps = 4000000 / n;
		double t = best_ns([&] { for (int r = 0; r < reps; ++r) { for (int i = 0; i < n; ++i) v[i] = p.allocate(1); for (int i = 0; i < n; ++i) p.deallocate(v[i], 1); } }, double(n) * reps);
		std::printf("alloc n=%d then free n, reused %.3f ns\n", n, t);
	}
	// fresh pool each time: chunk growth plus bump allocation, then destroy the pool
	for (int n : {1000, 100000}) {
		std::vector<Node*> v(n);
		int reps = 2000000 / n;
		double t = best_ns([&] { for (int r = 0; r < reps; ++r) { Pool p; for (int i = 0; i < n; ++i) v[i] = p.allocate(1); sink = v[0]; } }, double(n) * reps);
		std::printf("fresh pool, alloc n=%d (bump + destroy) %.3f ns\n", n, t);
	}
	// random order frees
	{
		const int n = 100000;
		Pool p;
		std::vector<Node*> v(n);
		std::mt19937 g(3);
		std::vector<int> order(n);
		for (int i = 0; i < n; ++i) order[i] = i;
		std::shuffle(order.begin(), order.end(), g);
		double t = best_ns([&] { for (int r = 0; r < 20; ++r) { for (int i = 0; i < n; ++i) v[i] = p.allocate(1); for (int i : order) p.deallocate(v[i], 1); } }, double(n) * 20);
		std::printf("alloc n=100000, free in random order %.3f ns\n", t);
	}
	// std::allocator control
	{
		std::allocator<Node> p;
		double t = best_ns([&] { for (int i = 0; i < steady_ops; ++i) { Node* n = p.allocate(1); sink = n; p.deallocate(n, 1); } }, steady_ops);
		std::printf("control std::allocator alloc+free steady %.3f ns\n", t);
	}
}
