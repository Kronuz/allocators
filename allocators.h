/*
 * Copyright © 2026 Germán Méndez Bravo (Kronuz)
 * Copyright (c) 2015-2019 Dubalu LLC
 * Copyright (c) 2017 moya-lang.org
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <atomic>         // for std::atomic (pool use check)
#include <cstddef>        // for std::size_t
#include <cstdio>         // for std::fputs
#include <cstdlib>        // for std::abort
#include <memory>         // for std::shared_ptr, std::unique_ptr
#include <limits>         // for std::numeric_limits
#include <new>            // for std::bad_alloc, std::nothrow_t, std::new_handler
#include <type_traits>    // for std::true_type, std::false_type
#include <utility>        // for std::move
#include <vector>         // for std::vector

// Overlapping use of one memory pool from two threads is detected (and reported
// through set_pool_misuse_handler(); abort by default) when this is defined; it
// defaults on in builds without NDEBUG.
#if !defined(ALLOCATORS_POOL_CHECK) && !defined(NDEBUG)
#define ALLOCATORS_POOL_CHECK 1
#endif


//  _____               _            _
// |_   _| __ __ _  ___| | _____  __| |
//   | || '__/ _` |/ __| |/ / _ \/ _` |
//   | || | | (_| | (__|   <  __/ (_| |
//   |_||_|  \__,_|\___|_|\_\___|\__,_|
//
namespace allocators {
	class vanilla_allocator {
	public:
		static void* allocate(std::size_t __sz) noexcept;
		static void deallocate(void *__p) noexcept;
	};

	class tracked_allocator {
	public:
		static void* allocate(std::size_t __sz) noexcept;
		static void deallocate(void *__p) noexcept;
	};

	template <typename _Tp, typename _Allocator>
	class allocator {
	public:
		// type definitions
		typedef std::size_t size_type;
		typedef std::ptrdiff_t difference_type;
		typedef _Tp* pointer;
		typedef const _Tp* const_pointer;
		typedef _Tp& reference;
		typedef const _Tp& const_reference;
		typedef _Tp value_type;

		// constructors and destructor - deleted copy
		allocator() = default;
		template<typename _Tp1>
		allocator(const allocator<_Tp1, _Allocator>&) throw() { }

		// rebind allocator to type _Tp1
		template <typename _Tp1>
		struct rebind {
			using other = allocator<_Tp1, _Allocator>;
		};

		// allocate but don't initialize n elements
		pointer allocate(size_type __n, const void* __hint = static_cast<const void*>(0)) {
			if (__n > max_size() || __hint) {
				throw std::bad_alloc();
			}
			return static_cast<pointer>(_Allocator::allocate(__n * sizeof(value_type)));
		}

		// deallocate storage __p of deleted elements
		void deallocate(pointer __p, size_type) {
			_Allocator::deallocate(__p);
		}

		// initialize elements of allocated storage __p with value value
		// Perfect-forwards, so allocator_traits::construct(a, p, std::move(x)) moves.
		template <typename... _Args>
		void construct(pointer __p, _Args&&... __args) noexcept(std::is_nothrow_constructible_v<value_type, _Args...>) {
			::new(static_cast<void*>(__p)) value_type(std::forward<_Args>(__args)...);
		}

		// destroy elements of initialized storage __p
		void destroy(pointer __p) {
			__p->~value_type();
		}

		// return address of values
		pointer address(reference __x) const {
			return &__x;
		}

		const_pointer address(const_reference __x) const {
			return &__x;
		}

		// return maximum number of elements that can be allocated
		size_type max_size() const throw() {
			return std::numeric_limits<std::size_t>::max() / sizeof(value_type);
		}
	};

	long long total_allocated() noexcept;
	long long local_allocated() noexcept;

	template <typename _Tp, typename _Allocator>
	inline bool operator==(const allocator<_Tp, _Allocator>&, const allocator<_Tp, _Allocator>&) noexcept { return true; }
	template <typename _Tp, typename _Allocator>
	inline bool operator!=(const allocator<_Tp, _Allocator>&, const allocator<_Tp, _Allocator>&) noexcept { return false; }
}


//  __  __                                   ____             _
// |  \/  | ___ _ __ ___   ___  _ __ _   _  |  _ \ ___   ___ | |
// | |\/| |/ _ \ '_ ` _ \ / _ \| '__| | | | | |_) / _ \ / _ \| |
// | |  | |  __/ | | | | | (_) | |  | |_| | |  __/ (_) | (_) | |
// |_|  |_|\___|_| |_| |_|\___/|_|   \__, | |_|   \___/ \___/|_|
//                                   |___/
namespace allocators {
	// Called when the pool detects overlapping use from two threads (checked builds
	// only, see ALLOCATORS_POOL_CHECK). The default prints and aborts; a test may
	// install another handler. Never called in unchecked builds.
	using pool_misuse_handler = void (*)() noexcept;
	inline pool_misuse_handler set_pool_misuse_handler(pool_misuse_handler handler) noexcept;

	namespace detail {
		// One size class of a pool family: chunked storage, a bump pointer into the
		// newest chunk and a LIFO free list. Chunks are never returned until the
		// family dies, so a block stays valid for as long as any allocator that can
		// reach the family lives.
		class pool_class {
		public:
			explicit pool_class(std::size_t block) : block_size(block) {}
			pool_class(const pool_class&) = delete;
			pool_class& operator=(const pool_class&) = delete;
			~pool_class() {
				for (void* chunk : chunks) {
					::operator delete(chunk, std::align_val_t{alignof(std::max_align_t)});
				}
			}

			std::size_t size() const noexcept { return block_size; }

			void* allocate() {
				check_guard guard(*this);
				if (free_head) {
					free_node* block = free_head;
					free_head = block->next;
					return block;
				}
				if (bump == bump_end) {
					grow();
				}
				void* block = bump;
				bump += block_size;
				return block;
			}

			void deallocate(void* p) noexcept {
				check_guard guard(*this);
				auto* block = static_cast<free_node*>(p);
				block->next = free_head;
				free_head = block;
			}

#ifdef ALLOCATORS_POOL_CHECK
			// Marks the class in use for the guard's lifetime; a second user while it is
			// held is a thread-safety violation (see memory_pool_allocator).
			// Plain relaxed load and store, not an exchange: the check costs a couple of
			// instructions per call and still catches threads that overlap for more than a few
			// nanoseconds (it is a diagnostic, not a lock).
			bool try_enter() noexcept {
				if (busy.load(std::memory_order_relaxed)) {
					return false;
				}
				busy.store(true, std::memory_order_relaxed);
				return true;
			}
			void leave() noexcept { busy.store(false, std::memory_order_relaxed); }
#endif

		private:
			struct free_node { free_node* next; };

			struct check_guard {
#ifdef ALLOCATORS_POOL_CHECK
				pool_class& owner;
				explicit check_guard(pool_class& o) : owner(o) {
					if (!owner.try_enter()) {
						pool_misuse();
					}
				}
				~check_guard() { owner.leave(); }
#else
				explicit check_guard(pool_class&) noexcept {}
#endif
			};

			static void pool_misuse() noexcept;

			void grow() {
				// 16 blocks, doubling per chunk up to ~64 KiB.
				std::size_t blocks = 16;
				for (std::size_t n = chunks.size(); n > 0 && blocks * block_size < 65536; --n) {
					blocks *= 2;
				}
				chunks.reserve(chunks.size() + 1);   // cannot fail after the chunk is acquired
				void* chunk = ::operator new(blocks * block_size, std::align_val_t{alignof(std::max_align_t)});
				chunks.push_back(chunk);
				bump = static_cast<char*>(chunk);
				bump_end = bump + blocks * block_size;
			}

			std::size_t block_size;
			free_node* free_head = nullptr;
			char* bump = nullptr;
			char* bump_end = nullptr;
			std::vector<void*> chunks;
#ifdef ALLOCATORS_POOL_CHECK
			std::atomic<bool> busy{false};
#endif
		};

		// What every allocator copy and rebind of one pool shares: a size class per
		// distinct block size. Created lazily by the first allocate of that size.
		class pool_family {
		public:
			pool_class& get(std::size_t block) {
				for (auto& c : classes) {
					if (c->size() == block) {
						return *c;
					}
				}
				classes.reserve(classes.size() + 1);
				classes.push_back(std::make_unique<pool_class>(block));
				return *classes.back();
			}

		private:
			std::vector<std::unique_ptr<pool_class>> classes;
		};

		inline pool_misuse_handler& misuse_handler() noexcept {
			static pool_misuse_handler handler = nullptr;
			return handler;
		}

		inline void pool_class::pool_misuse() noexcept {
			if (auto h = misuse_handler()) {
				h();
				return;
			}
			std::fputs("allocators: memory_pool_allocator used from two threads at once\n", stderr);
			std::abort();
		}
	}

	inline pool_misuse_handler set_pool_misuse_handler(pool_misuse_handler handler) noexcept {
		auto& slot = detail::misuse_handler();
		auto old = slot;
		slot = handler;
		return old;
	}


	// A single-element node pool for std::list, std::map and other node-based
	// containers.
	//
	// Identity. An allocator holds a shared handle to a pool *family*; every copy and
	// every rebind of an allocator holds the same family, so `A<T>(A<U>(a)) == a`, any
	// two allocators of one family compare equal whatever their value types, and the
	// family (with all its blocks) lives until the last such allocator is gone. Blocks
	// are recycled, never returned to the system before that. Equal allocators can free
	// each other's blocks; allocators of different families cannot, and compare unequal.
	// A default-constructed allocator starts a new family (one allocation); the converting
	// constructor only copies the handle: it never allocates and cannot throw.
	//
	// Containers. A container *copy* gets its own pool, by construction
	// (select_on_container_copy_construction returns a new family) and by assignment
	// (propagate_on_container_copy_assignment is false: the target keeps its allocator
	// and allocates the copied elements from its own pool), so a copy handed to another
	// thread shares nothing with the original. Move assignment and swap exchange blocks
	// wholesale, so propagate_on_container_{move_assignment,swap} are true: the family
	// travels with the nodes and these operations never fall back to element-wise moves.
	// is_always_equal is false; equality is "same family". A moved-from container keeps
	// the family (a move copies the allocator, as the Allocator requirements demand), so
	// it shares the pool with the container that took its nodes.
	//
	// Thread safety. A pool is NOT thread-safe: all allocators of one family (the
	// container, its copies made through the allocator rather than the container, and
	// every node allocator rebound from it) must be used by at most one thread at a
	// time, with a happens-before edge between hand-overs. Copying a container is safe
	// across threads for exactly that reason (the copy gets its own pool); copying or
	// passing an *allocator* to another thread while the original is still in use is
	// not. Define ALLOCATORS_POOL_CHECK (the default when NDEBUG is not defined) to
	// have overlapping use of one pool detected and reported through the misuse handler
	// (abort by default); this is a best-effort diagnostic that finds overlap in time (not a
	// lock, and not a missing hand-over edge), so run such code under ThreadSanitizer as well.
	template <typename _Tp>
	class memory_pool_allocator {
		static_assert(alignof(_Tp) <= alignof(std::max_align_t), "memory_pool_allocator does not support over-aligned types");

		template <typename> friend class memory_pool_allocator;

	public:
		// type definitions
		typedef std::size_t size_type;
		typedef std::ptrdiff_t difference_type;
		typedef _Tp* pointer;
		typedef const _Tp* const_pointer;
		typedef _Tp& reference;
		typedef const _Tp& const_reference;
		typedef _Tp value_type;

		using propagate_on_container_copy_assignment = std::false_type;
		using propagate_on_container_move_assignment = std::true_type;
		using propagate_on_container_swap = std::true_type;
		using is_always_equal = std::false_type;

	private:
		static constexpr std::size_t block_size =
			((sizeof(value_type) < sizeof(void*) ? sizeof(void*) : sizeof(value_type)) + alignof(std::max_align_t) - 1)
			/ alignof(std::max_align_t) * alignof(std::max_align_t);

		std::shared_ptr<detail::pool_family> family = std::make_shared<detail::pool_family>();
		// This allocator's size class, found on first use (copies of the family handle
		// that never allocate do not pay for the lookup).
		detail::pool_class* size_class = nullptr;

		detail::pool_class& pool() {
			if (!size_class) {
				size_class = &family->get(block_size);
			}
			return *size_class;
		}

	public:
		// A new allocator starts a new pool family.
		memory_pool_allocator() = default;
		// A copy shares the family; a move copies too, so the source stays usable.
		memory_pool_allocator(const memory_pool_allocator&) noexcept = default;
		memory_pool_allocator& operator=(const memory_pool_allocator&) noexcept = default;
		// A rebound allocator serves another block size from the same family. It only
		// shares the handle: no allocation, no throw, and rebinding back is the identity.
		template <typename _Tp1>
		memory_pool_allocator(const memory_pool_allocator<_Tp1>& other) noexcept : family(other.family) { }

		// rebind allocator to type _Tp1
		template <typename _Tp1>
		struct rebind {
			using other = memory_pool_allocator<_Tp1>;
		};

		// A container copy starts its own pool.
		memory_pool_allocator select_on_container_copy_construction() const {
			return memory_pool_allocator();
		}

		// allocate but don't initialize n elements
		pointer allocate(size_type __n, const void* __hint = static_cast<const void*>(0)) {
			if (__n != 1 || __hint) {
				throw std::bad_alloc();
			}
			return static_cast<pointer>(pool().allocate());
		}

		// deallocate storage __p of deleted elements
		void deallocate(pointer __p, size_type) {
			pool().deallocate(__p);
		}

		// initialize elements of allocated storage __p with value value
		// Perfect-forwards, so allocator_traits::construct(a, p, std::move(x)) moves.
		template <typename... _Args>
		void construct(pointer __p, _Args&&... __args) noexcept(std::is_nothrow_constructible_v<value_type, _Args...>) {
			::new(static_cast<void*>(__p)) value_type(std::forward<_Args>(__args)...);
		}

		// destroy elements of initialized storage __p
		void destroy(pointer __p) {
			__p->~value_type();
		}

		// Equal when they share a pool family, whatever the value types.
		template <typename _Tp1>
		bool same_pool(const memory_pool_allocator<_Tp1>& other) const noexcept { return family == other.family; }
		template <typename _Tp1>
		friend bool operator==(const memory_pool_allocator& a, const memory_pool_allocator<_Tp1>& b) noexcept { return a.same_pool(b); }
		template <typename _Tp1>
		friend bool operator!=(const memory_pool_allocator& a, const memory_pool_allocator<_Tp1>& b) noexcept { return !a.same_pool(b); }
	};

}
