/*
 * Copyright (c) 2026 Germán Méndez Bravo (Kronuz)
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

#pragma once
#include "storage.h"
#include <memory>

namespace allocators {
// Stateful allocator for component controls and standard-library adapters.
// The actual upstream request, including its header, stays charged even when
// an allocate_shared control block survives its object through weak owners.
template <class T> class ChargedAllocator {
	static_assert(alignof(T) <= alignof(std::max_align_t));
	struct alignas(std::max_align_t) Header {
		ByteCharge ticket;
	};

  public:
	using value_type = T;
	static constexpr std::size_t allocation_overhead() noexcept { return sizeof(Header); }
	explicit ChargedAllocator(std::shared_ptr<ByteAccount> account,
							  PagedStorage::Allocate allocate = std::malloc,
							  PagedStorage::Release release = std::free)
		: account_(std::move(account)), allocate_(allocate), release_(release) {
		if (!account_ || !allocate_ || !release_)
			throw std::invalid_argument("invalid storage allocator");
	}
	template <class U>
	ChargedAllocator(const ChargedAllocator<U> &other) noexcept
		: account_(other.account_), allocate_(other.allocate_), release_(other.release_) {}
	T *allocate(std::size_t count) {
		if (count > (std::numeric_limits<std::size_t>::max() - sizeof(Header)) / sizeof(T))
			throw std::bad_array_new_length();
		auto ticket = account_->admit(sizeof(Header) + count * sizeof(T));
		auto memory = allocate_(ticket.bytes());
		if (!memory)
			throw std::bad_alloc();
		auto header = new (memory) Header{std::move(ticket)};
		return reinterpret_cast<T *>(header + 1);
	}
	void deallocate(T *pointer, std::size_t) noexcept {
		auto header = reinterpret_cast<Header *>(pointer) - 1;
		auto ticket = std::move(header->ticket);
		header->~Header();
		release_(header);
		ticket.reset(); // Refund after the allocation is physically released.
	}
	template <class U> bool operator==(const ChargedAllocator<U> &other) const noexcept {
		return account_ == other.account_ && allocate_ == other.allocate_ && release_ == other.release_;
	}

  private:
	std::shared_ptr<ByteAccount> account_;
	PagedStorage::Allocate allocate_;
	PagedStorage::Release release_;
	template <class> friend class ChargedAllocator;
};
// Unique controls retain their account and upstream hooks through destruction.
// T may be incomplete in an owning public header; destruction is defined where
// its complete type is available.
template <class T> struct ChargedDelete {
	std::shared_ptr<ByteAccount> account;
	PagedStorage::Allocate allocate = std::malloc;
	PagedStorage::Release release = std::free;
	void operator()(T *object) const noexcept {
		ChargedAllocator<T> allocator(account, allocate, release);
		std::destroy_at(object);
		allocator.deallocate(object, 1);
	}
};
template <class T> using ChargedOwner = std::unique_ptr<T, ChargedDelete<T>>;
template <class T, class... Args>
ChargedOwner<T> make_charged_owner(std::shared_ptr<ByteAccount> account, PagedStorage::Allocate allocate,
								   PagedStorage::Release release, Args &&...args) {
	ChargedAllocator<T> allocator(account, allocate, release);
	auto object = allocator.allocate(1);
	try {
		std::construct_at(object, std::forward<Args>(args)...);
	} catch (...) {
		allocator.deallocate(object, 1);
		throw;
	}
	return ChargedOwner<T>(object, ChargedDelete<T>{std::move(account), allocate, release});
}
} // namespace allocators
