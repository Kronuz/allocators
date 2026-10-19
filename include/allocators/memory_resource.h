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
#include "charged_allocator.h"
#include <bit>
#include <memory_resource>

namespace allocators {
// An unpooled PMR adapter. Full upstream requests, including alignment slack,
// remain charged until the original allocation is actually released.
class ChargedMemoryResource final : public std::pmr::memory_resource {
	struct alignas(std::max_align_t) Header {
		ByteCharge ticket;
	};

  public:
	explicit ChargedMemoryResource(std::shared_ptr<ByteAccount> account,
								  PagedStorage::Allocate allocate = std::malloc,
								  PagedStorage::Release release = std::free)
		: account_(std::move(account)), allocate_(allocate), release_(release) {
		if (!account_ || !allocate_ || !release_)
			throw std::invalid_argument("invalid ledger memory resource");
	}

  private:
	void *do_allocate(std::size_t bytes, std::size_t alignment) override {
		if (!std::has_single_bit(alignment))
			throw std::bad_alloc();
		alignment = std::max(alignment, alignof(Header *));
		constexpr auto overhead = sizeof(Header) + sizeof(Header *);
		const auto padding = alignment - 1;
		const auto maximum = std::numeric_limits<std::size_t>::max();
		if (bytes > maximum - overhead || padding > maximum - overhead - bytes)
			throw std::bad_alloc();
		auto ticket = account_->admit(overhead + bytes + padding);
		auto memory = allocate_(ticket.bytes());
		if (!memory)
			throw std::bad_alloc();
		auto header = new (memory) Header{std::move(ticket)};
		void *payload = static_cast<char *>(memory) + overhead;
		auto space = bytes + padding;
		if (!std::align(alignment, bytes, payload, space)) {
			dispose(header);
			throw std::bad_alloc();
		}
		std::construct_at(reinterpret_cast<Header **>(payload) - 1, header);
		return payload;
	}
	void do_deallocate(void *pointer, std::size_t, std::size_t) noexcept override {
		dispose(*(reinterpret_cast<Header **>(pointer) - 1));
	}
	void dispose(Header *header) noexcept {
		auto ticket = std::move(header->ticket);
		std::destroy_at(header);
		release_(header);
		ticket.reset();
	}
	bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
		return this == &other;
	}
	std::shared_ptr<ByteAccount> account_;
	PagedStorage::Allocate allocate_;
	PagedStorage::Release release_;
};
inline std::shared_ptr<std::pmr::memory_resource>
make_charged_resource(std::shared_ptr<ByteAccount> account, PagedStorage::Allocate allocate = std::malloc,
					  PagedStorage::Release release = std::free) {
	return std::allocate_shared<ChargedMemoryResource>(
		ChargedAllocator<ChargedMemoryResource>(account, allocate, release), account, allocate, release);
}
} // namespace allocators
