/*
 * Copyright (c) 2015-2019 Dubalu LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// XAPIAND_TRACKED_MEM (enables the global operator new/delete byte accounting)
// and HAS_THREAD_LOCAL (thread-local vs. global counters) came from Xapiand's
// project-wide "config.h". Standalone, the library supplies them from its own
// CMake: ALLOCATORS_TRACKED_MEM defines XAPIAND_TRACKED_MEM, and a
// check_cxx_source_compiles probe defines HAS_THREAD_LOCAL, both #cmakedefine'd
// into the generated allocators_config.h. A consumer that compiles this .cc
// into its own build (e.g. via FetchContent populate, without running this
// library's CMakeLists) won't have allocators_config.h; it should define the
// macros itself. Guard the include so both paths work. When the macro isn't
// already defined, the in-source compiler-feature probe below still detects
// thread_local on its own.
#if __has_include("allocators_config.h")
#include "allocators_config.h"  // for XAPIAND_TRACKED_MEM, HAS_THREAD_LOCAL
#endif

#include "allocators.h"

#include <cstdlib>        // for std::malloc, std::free

#ifdef XAPIAND_TRACKED_MEM
#include <atomic>         // for std::atomic_llong
#include <functional>     // for std::hash
#include <mutex>          // for std::mutex
#include <thread>         // for std::this_thread
#include <unordered_map>  // for std::unordered_map
#include <utility>        // for std::make_pair


#ifndef HAS_THREAD_LOCAL
#if defined(_MSC_VER) || (defined(__GNUC__) && __GNUC__ > 4) || (__GNUC__ == 4 && __GNUC_MINOR__ >= 8) || (defined(__llvm__) && !defined(_LIBCPP_VERSION))
# define HAS_THREAD_LOCAL
#elif defined(__clang__)
# if __has_feature(cxx_thread_local)
#   define HAS_THREAD_LOCAL
# endif
#endif
#if defined(__FreeBSD__) && (__FreeBSD__ < 11)  //Bug in freeBSD lower that 11v. Use of thread_local produces linking error
# undef HAS_THREAD_LOCAL
#endif
#endif  // HAS_THREAD_LOCAL
#endif  // XAPIAND_TRACKED_MEM


// The malloc/free wrappers and the vanilla_allocator / tracked_allocator member
// definitions are always compiled, regardless of XAPIAND_TRACKED_MEM. Upstream
// kept the entire body behind the macro because in an untracked Xapiand build
// nothing referenced these allocators, so the missing symbols never surfaced.
// Standalone the header advertises them as public API, so a consumer can
// instantiate allocator<T, vanilla_allocator> in any build; the definitions must
// exist. Only the byte accounting (the counters, the global new/delete overrides,
// and the size-header bookkeeping inside tracked_allocator) is XAPIAND_TRACKED_MEM
// -specific. This, plus dropping the upstream `inline` so the symbols export, is
// the only source delta beyond the config.h decouple.
namespace allocators {
	// Allocating Aligned Memory Blocks
	// The address of a block returned by malloc or realloc is always a
	// multiple of eight (or sixteen on 64-bit systems).
	constexpr std::size_t alignment = alignof(std::max_align_t);

	// allocate using ::malloc
	static inline void* malloc(std::size_t __sz) {
		if (__sz == 0) {
			__sz = 1;
		}
		void* __p;
		while ((__p = std::malloc(__sz)) == nullptr) {
			// If malloc fails and there is a new_handler, call it to try free up memory.
			std::new_handler nh = std::get_new_handler();
			if (nh != nullptr) {
				nh();
			} else {
				throw std::bad_alloc();
			}
		}
		return __p;
	}

	// deallocate using ::free
	static inline void free(void* __p) noexcept {
		std::free(__p);
	}

	// vanilla_allocator

	void* vanilla_allocator::allocate(std::size_t __sz) noexcept {
		// fprintf(stderr, "{allocate %zu bytes}\n", __sz);
		return ::allocators::malloc(__sz);
	}

	void vanilla_allocator::deallocate(void* __p) noexcept {
		if (__p != nullptr) {
			// fprintf(stderr, "{deallocate}\n");
			::allocators::free(__p);
		}
	}


	// tracked_allocator

#ifdef XAPIAND_TRACKED_MEM

	static inline std::atomic_llong& _total_allocated() noexcept {
		static std::atomic_llong t_allocated;
		return t_allocated;
	}

#ifdef HAS_THREAD_LOCAL
	static inline long long& _local_allocated() noexcept {
		static thread_local long long l_allocated;
		return l_allocated;
	}

	long long local_allocated() noexcept {
		return _local_allocated();
	}

#else
	#pragma message ("Threading without thread_local support is not well supported.")

	static std::mutex tracked_mutex;
	static std::unordered_map<
		std::thread::id,
		long long,
		std::hash<std::thread::id>,
		std::equal_to<std::thread::id>,
		allocator<std::pair<const std::thread::id, long long>, vanilla_allocator>
	> tracked_sizes;

	static inline long long& __local_allocated() noexcept {
		const auto id = std::this_thread::get_id();
		auto it = tracked_sizes.find(id);
		if (it != tracked_sizes.end()) return it->second;
		auto& l_allocated = tracked_sizes.emplace(id, 0).first->second;
		return l_allocated;
	}

	static inline long long& _local_allocated() noexcept {
		std::lock_guard<std::mutex> lock(tracked_mutex);
		return __local_allocated();
	}

	long long local_allocated() noexcept {
		std::lock_guard<std::mutex> lock(tracked_mutex);
		std::size_t l_allocated = __local_allocated();
		return l_allocated;
	}
#endif

	long long total_allocated() noexcept {
		return _total_allocated();
	}

	void* tracked_allocator::allocate(std::size_t __sz) noexcept {
		// fprintf(stderr, "[allocate %zu bytes]\n", __sz);
		void* __p = ::allocators::malloc(__sz + alignment);
		auto& t_allocated = _total_allocated();
		auto& l_allocated = _local_allocated();
		t_allocated += __sz;
		l_allocated += __sz;
		*static_cast<std::size_t*>(__p) = __sz;
		// fprintf(stderr, "[allocated %zu bytes at %__p, %lld [%lld] are now allocated]\n", __sz, __p, l_allocated, t_allocated.load());
		__p = static_cast<char*>(__p) + alignment;
		return __p;
	}

	void tracked_allocator::deallocate(void* __p) noexcept {
		if (__p != nullptr) {
			__p = static_cast<char*>(__p) - alignment;
			// fprintf(stderr, "[deallocate %__p]\n", __p);
			std::size_t __sz = *static_cast<std::size_t*>(__p);
			auto& t_allocated = _total_allocated();
			auto& l_allocated = _local_allocated();
			t_allocated -= __sz;
			l_allocated -= __sz;
			// fprintf(stderr, "[deallocating %zu bytes at %__p, %lld [%lld] remain allocated]\n", __sz, __p, l_allocated, t_allocated.load());
			::allocators::free(__p);
		}
	}

#else  /* XAPIAND_TRACKED_MEM */

	// Untracked build: no counters, no size-header. tracked_allocator is a plain
	// malloc/free passthrough so it stays usable as a backend, and the accounting
	// accessors report zero.
	long long total_allocated() noexcept {
		return 0;
	}

	long long local_allocated() noexcept {
		return 0;
	}

	void* tracked_allocator::allocate(std::size_t __sz) noexcept {
		return ::allocators::malloc(__sz);
	}

	void tracked_allocator::deallocate(void* __p) noexcept {
		if (__p != nullptr) {
			::allocators::free(__p);
		}
	}

#endif  /* XAPIAND_TRACKED_MEM */
}

#ifdef XAPIAND_TRACKED_MEM

// Operators overload for tracking

void* operator new(std::size_t __sz) noexcept(false) {
	return allocators::tracked_allocator::allocate(__sz);
}


void* operator new(std::size_t __sz, const std::nothrow_t& /*unused*/) noexcept {
	return allocators::tracked_allocator::allocate(__sz);
}


void* operator new[](std::size_t __sz) noexcept(false) {
	return allocators::tracked_allocator::allocate(__sz);
}


void* operator new[](std::size_t __sz, const std::nothrow_t& /*unused*/) noexcept {
	return allocators::tracked_allocator::allocate(__sz);
}


void operator delete(void* __p) noexcept {
	allocators::tracked_allocator::deallocate(__p);
}


void operator delete[](void* __p) noexcept {
	allocators::tracked_allocator::deallocate(__p);
}

void operator delete(void* __p, std::size_t /*unused*/) noexcept {
	allocators::tracked_allocator::deallocate(__p);
}


void operator delete[](void* __p, std::size_t /*unused*/) noexcept {
	allocators::tracked_allocator::deallocate(__p);
}

#endif  /* XAPIAND_TRACKED_MEM */
