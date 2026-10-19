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
#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <cstdint>
#include <limits>
#include <optional>
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace allocators {
template <std::size_t N> struct ResourceVector {
	std::array<std::uint64_t, N> values{};
	bool operator==(const ResourceVector &) const = default;
	constexpr bool fits(const ResourceVector &ceiling) const noexcept {
		for (std::size_t i = 0; i < N; ++i)
			if (values[i] > ceiling.values[i]) return false;
		return true;
	}
	constexpr ResourceVector added(const ResourceVector &other) const {
		ResourceVector result;
		for (std::size_t i = 0; i < N; ++i) {
			if (other.values[i] > std::numeric_limits<std::uint64_t>::max() - values[i])
				throw std::overflow_error("resource vector addition overflow");
			result.values[i] = values[i] + other.values[i];
		}
		return result;
	}
	constexpr ResourceVector subtracted(const ResourceVector &other) const {
		if (!other.fits(*this)) throw std::invalid_argument("resource vector subtraction underflow");
		ResourceVector result;
		for (std::size_t i = 0; i < N; ++i) result.values[i] = values[i] - other.values[i];
		return result;
	}
};
template <std::size_t N> class ReservationBook;
template <std::size_t N> class Reservation {
  public:
	Reservation() = default;
	Reservation(const Reservation &) = delete;
	Reservation &operator=(const Reservation &) = delete;
	Reservation(Reservation &&other) noexcept
		: book_(std::exchange(other.book_, nullptr)), peak_(other.peak_) {}
	Reservation &operator=(Reservation &&other) noexcept {
		if (this != &other) {
			abandon();
			book_ = std::exchange(other.book_, nullptr);
			peak_ = other.peak_;
		}
		return *this;
	}
	~Reservation() { abandon(); }
	ResourceVector<N> peak() const noexcept { return peak_; }
	explicit operator bool() const noexcept { return book_ != nullptr; }
	void abandon() noexcept;
  private:
	Reservation(ReservationBook<N> &book, ResourceVector<N> peak) noexcept : book_(&book), peak_(peak) {}
	ReservationBook<N> *book_ = nullptr;
	ResourceVector<N> peak_{};
	friend class ReservationBook<N>;
};
// Single-executor bookkeeping. The book outlives all its reservation capabilities.
template <std::size_t N> class ReservationBook {
  public:
	using Vector = ResourceVector<N>;
	explicit ReservationBook(Vector hard, Vector census = {}) noexcept
		: hard_(hard), used_(census), peak_(census) {}
	ReservationBook(const ReservationBook &) = delete;
	ReservationBook &operator=(const ReservationBook &) = delete;
	std::optional<Reservation<N>> try_reserve(Vector peak, Vector spendable) {
		if (abandoned_ || !peak.fits(spendable) || !can_add(peak)) return std::nullopt;
		auto next = reserved_.added(peak);
		auto total = used_.added(next);
		reserved_ = next;
		observe(total);
		return Reservation<N>(*this, peak);
	}
	bool try_grow(Reservation<N> &reservation, Vector minimum, Vector spendable_delta) {
		check(reservation);
		Vector target;
		for (std::size_t i = 0; i < N; ++i)
			target.values[i] = std::max(minimum.values[i], reservation.peak_.values[i]);
		auto delta = target.subtracted(reservation.peak_);
		if (abandoned_ || !delta.fits(spendable_delta) || !can_add(delta)) return false;
		auto next = reserved_.added(delta);
		auto total = used_.added(next);
		reserved_ = next;
		reservation.peak_ = target;
		observe(total);
		return true;
	}
	void cancel(Reservation<N> &reservation) {
		check(reservation);
		auto next = reserved_.subtracted(reservation.peak_);
		reserved_ = next;
		reservation.book_ = nullptr;
	}
	void settle(Reservation<N> &reservation, Vector additions, Vector proved_credit = {}) {
		check(reservation);
		if (!additions.fits(reservation.peak_))
			throw std::invalid_argument("settlement exceeds reservation");
		auto used = used_.subtracted(proved_credit).added(additions);
		auto reserved = reserved_.subtracted(reservation.peak_);
		used_ = used;
		reserved_ = reserved;
		reservation.book_ = nullptr;
	}
	void reclaim(Vector proved_credit) { used_ = used_.subtracted(proved_credit); }
	Vector used() const noexcept { return used_; }
	Vector reserved() const noexcept { return reserved_; }
	Vector peak() const noexcept { return peak_; }
	Vector hard() const noexcept { return hard_; }
	bool abandoned() const noexcept { return abandoned_; }
  private:
	bool can_add(Vector extra) const noexcept {
		for (std::size_t i = 0; i < N; ++i)
			if (used_.values[i] > hard_.values[i] ||
				reserved_.values[i] > hard_.values[i] - used_.values[i] ||
				extra.values[i] > hard_.values[i] - used_.values[i] - reserved_.values[i]) return false;
		return true;
	}
	void check(const Reservation<N> &reservation) const {
		if (reservation.book_ != this) throw std::invalid_argument("foreign or inactive reservation");
	}
	void observe(Vector total) noexcept {
		for (std::size_t i = 0; i < N; ++i) peak_.values[i] = std::max(peak_.values[i], total.values[i]);
	}
	Vector hard_, used_, reserved_{}, peak_;
	bool abandoned_ = false;
	friend class Reservation<N>;
};
template <std::size_t N> void Reservation<N>::abandon() noexcept {
	if (book_) { book_->abandoned_ = true; book_ = nullptr; }
}

class ResourcePressure : public std::length_error {
  public:
	explicit ResourcePressure(const char *message) : std::length_error(message) {}
};
namespace detail {
struct ByteAccountState {
	const std::size_t limit;
	const std::shared_ptr<ByteAccountState> parent;
	const std::size_t depth;
	std::atomic<std::size_t> used{0}, peak{0};
	ByteAccountState(std::size_t capacity, std::shared_ptr<ByteAccountState> ancestor = {})
		: limit(capacity), parent(std::move(ancestor)), depth(parent ? parent->depth + 1 : 1) {}
};
} // namespace detail
class ByteAccount;
class ByteCharge {
  public:
	ByteCharge() = default;
	ByteCharge(const ByteCharge &) = delete;
	ByteCharge &operator=(const ByteCharge &) = delete;
	ByteCharge(ByteCharge &&other) noexcept { swap(other); }
	ByteCharge &operator=(ByteCharge &&other) noexcept {
		if (this != &other) { reset(); swap(other); }
		return *this;
	}
	~ByteCharge() { reset(); }
	std::size_t bytes() const noexcept { return bytes_; }
	void reset() noexcept {
		for (auto state = state_.get(); state; state = state->parent.get())
			state->used.fetch_sub(bytes_, std::memory_order_relaxed);
		state_.reset();
		bytes_ = 0;
	}
  private:
	ByteCharge(std::shared_ptr<detail::ByteAccountState> state, std::size_t bytes) noexcept
		: state_(std::move(state)), bytes_(bytes) {}
	void swap(ByteCharge &other) noexcept {
		state_.swap(other.state_);
		std::swap(bytes_, other.bytes_);
	}
	std::shared_ptr<detail::ByteAccountState> state_;
	std::size_t bytes_ = 0;
	friend class ByteAccount;
};
class ByteAccount {
  public:
	using Ticket = ByteCharge;
	static constexpr std::size_t maximum_depth = 8;
	explicit ByteAccount(std::size_t limit)
		: state_(std::make_shared<detail::ByteAccountState>(limit)) {}
	ByteAccount(std::shared_ptr<ByteAccount> parent, std::size_t limit) {
		if (!parent || limit > parent->limit() || parent->state_->depth == maximum_depth)
			throw std::invalid_argument("invalid byte account hierarchy");
		state_ = std::make_shared<detail::ByteAccountState>(limit, parent->state_);
	}
	ByteAccount(const ByteAccount &) = delete;
	ByteAccount &operator=(const ByteAccount &) = delete;
	ByteAccount share() const noexcept { return ByteAccount(state_); }
	ByteCharge admit(std::size_t bytes) {
		std::array<std::size_t, maximum_depth> observations;
		std::size_t count = 0;
		for (auto state = state_.get(); state; state = state->parent.get()) {
			auto used = state->used.load(std::memory_order_relaxed);
			for (;;) {
				if (bytes > state->limit - used) {
					auto rollback = state_.get();
					for (std::size_t i = 0; i < count; ++i) {
						rollback->used.fetch_sub(bytes, std::memory_order_relaxed);
						rollback = rollback->parent.get();
					}
					throw ResourcePressure("byte account pressure");
				}
				if (state->used.compare_exchange_weak(used, used + bytes, std::memory_order_relaxed))
					break;
			}
			observations[count++] = used + bytes;
		}
		auto state = state_.get();
		for (std::size_t i = 0; i < count; ++i) {
			auto peak = state->peak.load(std::memory_order_relaxed);
			while (peak < observations[i] &&
				!state->peak.compare_exchange_weak(peak, observations[i], std::memory_order_relaxed)) {}
			state = state->parent.get();
		}
		return ByteCharge(state_, bytes);
	}
	std::size_t used() const noexcept { return state_->used.load(std::memory_order_relaxed); }
	std::size_t peak() const noexcept { return state_->peak.load(std::memory_order_relaxed); }
	std::size_t limit() const noexcept { return state_->limit; }
  private:
	explicit ByteAccount(std::shared_ptr<detail::ByteAccountState> state) noexcept : state_(std::move(state)) {}
	std::shared_ptr<detail::ByteAccountState> state_;
};
} // namespace allocators
