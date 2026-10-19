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
#include "storage.h"
#include <cstring>
#include <memory>
#include <span>
#include <string_view>

namespace allocators {
// Fixed-capacity, single-writer bytes. Full backing capacity is admitted before
// allocation; exposing initialized bytes does not refund unused capacity.
class OwnedBuffer {
  public:
	OwnedBuffer(std::shared_ptr<ByteAccount> account, std::size_t capacity)
		: account_(std::move(account)), arena_(checked_account(), 1), capacity_(capacity) {
		if (capacity)
			data_ = static_cast<char *>(arena_.allocate(capacity, 1));
	}
	OwnedBuffer(const OwnedBuffer &) = delete;
	OwnedBuffer &operator=(const OwnedBuffer &) = delete;
	void resize(std::size_t size) {
		if (sealed_ || size < size_ || size > capacity_)
			throw std::logic_error("invalid owned buffer growth");
		if (size != size_)
			std::fill(data_ + size_, data_ + size, '\0');
		size_ = size;
	}
	void append(std::string_view bytes) {
		if (sealed_ || bytes.size() > capacity_ - size_)
			throw std::logic_error("invalid owned buffer append");
		if (!bytes.empty())
			std::memmove(data_ + size_, bytes.data(), bytes.size());
		size_ += bytes.size();
	}
	std::span<char> mutable_bytes() {
		if (sealed_)
			throw std::logic_error("owned buffer sealed");
		return {data_, size_};
	}
	void seal() {
		if (size_ != capacity_)
			throw std::logic_error("owned buffer incomplete");
		sealed_ = true;
	}
	// Streaming chunks may end before backing capacity. Retain its full charge
	// while exposing only initialized bytes through immutable ownership.
	void seal_prefix() noexcept { sealed_ = true; }
	// Native codecs may initialize a bounded maximum destination and produce
	// a shorter payload. Publish that initialized prefix without reallocating.
	void seal_initialized_prefix(std::size_t size) {
		if (sealed_ || size > size_)
			throw std::logic_error("invalid initialized buffer prefix");
		size_ = size;
		sealed_ = true;
	}
	std::size_t size() const noexcept { return size_; }
	bool sealed() const noexcept { return sealed_; }
	bool empty() const noexcept { return !size_; }
	const char *data() const noexcept { return data_ ? data_ : ""; }
	const char *begin() const noexcept { return data(); }
	const char *end() const noexcept { return data() + size_; }
	std::string_view view() const noexcept { return {data(), size_}; }
	operator std::string_view() const noexcept { return view(); }
	std::size_t storage_capacity() const noexcept { return arena_.used(); }
	const std::shared_ptr<ByteAccount> &account() const noexcept { return account_; }

  private:
	ByteAccount &checked_account() const {
		if (!account_)
			throw std::invalid_argument("owned buffer account missing");
		return *account_;
	}
	std::shared_ptr<ByteAccount> account_;
	PagedStorage arena_;
	char *data_ = nullptr;
	std::size_t capacity_ = 0, size_ = 0;
	bool sealed_ = false;
};
inline std::shared_ptr<OwnedBuffer> make_owned_buffer(std::shared_ptr<ByteAccount> account,
													  std::size_t capacity) {
	return std::allocate_shared<OwnedBuffer>(ChargedAllocator<OwnedBuffer>(account), account, capacity);
}
} // namespace allocators
