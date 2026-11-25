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

// Benchmark, not a test: the resource accounting hot paths, one "row value ns" line per row (best of
// nine repetitions, per operation).
#include "allocators/charged_allocator.h"
#include "allocators/memory_resource.h"
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

using allocators::ByteAccount;
template <class T> using Charged = allocators::ChargedAllocator<T>;
struct Node48 { void *a, *b, *c, *d, *e, *f; };
static void *volatile sink;

template <class F> static double best_ns(F &&f, double ops, int reps = 9) {
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
template <class T> static double charged(const std::shared_ptr<ByteAccount> &account, int ops) {
	Charged<T> allocator(account);
	return best_ns([&] {
		for (int i = 0; i < ops; ++i) {
			auto p = allocator.allocate(1);
			sink = p;
			allocator.deallocate(p, 1);
		}
	}, ops);
}

int main() {
	constexpr int ops = 2000000;
	constexpr std::size_t limit = std::size_t(1) << 40;
	auto root = std::make_shared<ByteAccount>(limit);
	std::printf("admit+release root %.3f ns\n", best_ns([&] {
		for (int i = 0; i < ops; ++i) { auto c = root->admit(64); sink = &c; }
	}, ops));
	auto leaf = root;
	for (std::size_t depth = 1; depth < ByteAccount::maximum_depth; ++depth)
		leaf = std::make_shared<ByteAccount>(leaf, limit);
	std::printf("admit+release depth 8 %.3f ns\n", best_ns([&] {
		for (int i = 0; i < ops; ++i) { auto c = leaf->admit(64); sink = &c; }
	}, ops));
	std::printf("charged allocate+deallocate int %.3f ns\n", charged<int>(root, ops));
	std::printf("charged allocate+deallocate 48 B %.3f ns\n", charged<Node48>(root, ops));
	std::printf("allocate_shared int %.3f ns\n", best_ns([&] {
		for (int i = 0; i < ops; ++i) sink = std::allocate_shared<int>(Charged<int>(root), i).get();
	}, ops));
	auto resource = allocators::make_charged_resource(root);
	std::printf("pmr allocate+deallocate 64 B %.3f ns\n", best_ns([&] {
		for (int i = 0; i < ops; ++i) {
			auto p = resource->allocate(64, 16);
			sink = p;
			resource->deallocate(p, 64, 16);
		}
	}, ops));
	resource.reset();
	std::printf("paged storage 512 B, 64 KiB pages %.3f ns\n", best_ns([&] {
		allocators::PagedStorage pages(*root);
		for (int i = 0; i < ops / 8; ++i) sink = pages.allocate(512);
	}, ops / 8));
	for (unsigned threads : {2u, 4u}) {
		const int each = ops / int(threads);
		std::printf("admit+release root, %u threads %.3f ns\n", threads, best_ns([&] {
			std::vector<std::thread> pool;
			for (unsigned t = 0; t < threads; ++t)
				pool.emplace_back([&] { for (int i = 0; i < each; ++i) { auto c = root->admit(64); sink = &c; } });
			for (auto &thread : pool) thread.join();
		}, double(each) * threads, 5));
		std::printf("charged allocate+deallocate 48 B, %u threads %.3f ns\n", threads, best_ns([&] {
			std::vector<std::thread> pool;
			for (unsigned t = 0; t < threads; ++t)
				pool.emplace_back([&] {
					Charged<Node48> allocator(root);
					for (int i = 0; i < each; ++i) {
						auto p = allocator.allocate(1);
						sink = p;
						allocator.deallocate(p, 1);
					}
				});
			for (auto &thread : pool) thread.join();
		}, double(each) * threads, 5));
	}
	return root->used() != 0;
}
