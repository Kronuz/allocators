// Built against the tracked library: every global new/delete moves the counters,
// and allocators::allocator<T, tracked_allocator> accounts a container explicitly.
#include "allocators.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

int main() {
	long long before = allocators::total_allocated();

	std::vector<std::string> words;
	for (int i = 0; i < 1000; ++i) {
		words.push_back("a string long enough to live on the heap #" + std::to_string(i));
	}
	std::printf("1,000 heap strings: +%lld bytes (this thread: %lld)\n",
		allocators::total_allocated() - before, allocators::local_allocated());

	words.clear();
	words.shrink_to_fit();
	std::printf("after clearing:     +%lld bytes\n", allocators::total_allocated() - before);

	using Tracked = allocators::allocator<std::pair<const int, double>, allocators::tracked_allocator>;
	std::map<int, double, std::less<int>, Tracked> squares;
	for (int i = 0; i < 100; ++i) {
		squares[i] = i * i;
	}
	std::printf("100-entry map:      +%lld bytes\n", allocators::total_allocated() - before);
}
