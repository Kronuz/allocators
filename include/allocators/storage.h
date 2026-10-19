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
#include "resources.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>

namespace allocators {
// Single-owner stable storage with intrusive page metadata. All component-owned
// page capacity, padding, and headers are admitted before upstream allocation.
// malloc overhead and process RSS are deliberately outside this capacity metric.
// Disposal can release one page per scheduler turn, including after pressure.
class PagedStorage {
  public:
	using Allocate = void *(*)(std::size_t);
	using Release = void (*)(void *);
	explicit PagedStorage(ByteAccount &ledger, std::size_t page_bytes = 65536,
						  Allocate allocate = std::malloc, Release release = std::free,
						  std::size_t capacity_limit = std::numeric_limits<std::size_t>::max())
		: ledger_(ledger.share()), page_bytes_(page_bytes), allocate_(allocate), release_(release),
		  limit_(capacity_limit) {
		if (!page_bytes || !allocate || !release)
			throw std::invalid_argument("invalid paged storage configuration");
	}
	PagedStorage(const PagedStorage &) = delete;
	PagedStorage &operator=(const PagedStorage &) = delete;
	PagedStorage(PagedStorage &&other) noexcept
		: ledger_(other.ledger_.share()), page_bytes_(other.page_bytes_), allocate_(other.allocate_),
		  release_(other.release_), first_(std::exchange(other.first_, nullptr)),
		  disposing_(other.disposing_), limit_(other.limit_), used_(std::exchange(other.used_, 0)),
		  peak_(other.peak_) {}
	PagedStorage &operator=(PagedStorage &&) = delete;
	~PagedStorage() {
		while (!release_one()) {
		}
	}

	void *allocate(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t)) {
		if (disposing_)
			throw std::logic_error("paged storage is retiring");
		if (!alignment || (alignment & (alignment - 1)) || alignment > alignof(std::max_align_t))
			throw std::invalid_argument("unsupported paged storage alignment");
		if (!bytes)
			bytes = 1;
		if (first_) {
			const auto padding = (-first_->used) & (alignment - 1);
			if (padding <= first_->capacity - first_->used &&
				bytes <= first_->capacity - first_->used - padding) {
				first_->used += padding;
				auto result = data(first_) + first_->used;
				first_->used += bytes;
				return result;
			}
		}
		if (bytes > std::numeric_limits<std::size_t>::max() - sizeof(Page))
			throw std::length_error("paged storage extent overflow");
		const auto capacity = std::max(page_bytes_, bytes);
		if (capacity > std::numeric_limits<std::size_t>::max() - sizeof(Page))
			throw std::length_error("paged storage page overflow");
		if (sizeof(Page) + capacity > limit_ - used_)
			throw ResourcePressure("paged storage pressure");
		auto ticket = ledger_.admit(sizeof(Page) + capacity);
		void *memory = allocate_(ticket.bytes());
		if (!memory)
			throw std::bad_alloc(); // ticket rolls admission back
		auto page = new (memory) Page{first_, std::move(ticket), capacity, bytes};
		first_ = page;
		used_ += sizeof(Page) + capacity;
		peak_ = std::max(peak_, used_);
		return data(page);
	}
	// True means disposal is complete. Remaining references must not be used
	// once disposal begins. Releasing a page refunds only its actual ownership.
	bool release_one() noexcept {
		disposing_ = true;
		if (!first_)
			return true;
		auto page = first_;
		first_ = page->next;
		auto ticket = std::move(page->ticket);
		page->~Page();
		release_(page);
		used_ -= ticket.bytes();
		ticket.reset();
		return !first_;
	}
	std::size_t front_capacity() const noexcept { return first_ ? first_->ticket.bytes() : 0; }
	std::size_t used() const noexcept { return used_; }
	std::size_t peak() const noexcept { return peak_; }
	static constexpr std::size_t page_overhead() noexcept;

  private:
	struct alignas(std::max_align_t) Page {
		Page *next;
		ByteCharge ticket;
		std::size_t capacity, used;
	};
	static std::byte *data(Page *page) noexcept { return reinterpret_cast<std::byte *>(page + 1); }
	ByteAccount ledger_;
	std::size_t page_bytes_;
	Allocate allocate_;
	Release release_;
	Page *first_ = nullptr;
	bool disposing_ = false;
	std::size_t limit_, used_ = 0, peak_ = 0;
};
inline constexpr std::size_t PagedStorage::page_overhead() noexcept { return sizeof(Page); }

} // namespace allocators
