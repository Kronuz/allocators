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

#include "allocators/retirement.h"
#include <array>
#include <condition_variable>
#include <exception>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace allocators::detail {
namespace {
constexpr std::uint64_t closed = std::uint64_t{1} << 63;
constexpr std::size_t registry_capacity = 512;
struct Descriptor {
	int fd = -1;
	~Descriptor() {
		if (fd >= 0)
			::close(fd);
	}
};
void system_check(int error, const char *message) {
	if (error)
		throw std::system_error(error, std::generic_category(), message);
}
struct Wake {
	Descriptor input, output;
	Wake() {
		int pair[2];
#if defined(__linux__) || defined(__FreeBSD__)
		if (::pipe2(pair, O_NONBLOCK | O_CLOEXEC) != 0)
			system_check(errno, "retirement wake pipe");
#else
		if (::pipe(pair) != 0)
			system_check(errno, "retirement wake pipe");
#endif
		input.fd = pair[0];
		output.fd = pair[1];
#if !defined(__linux__) && !defined(__FreeBSD__)
		for (auto fd : pair) {
			const auto flags = ::fcntl(fd, F_GETFL);
			if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
				::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
				system_check(errno, "retirement wake flags");
		}
#endif
	}
	void signal() noexcept {
		const char byte = 0;
		// No retry or wait. A full pipe already signals; the periodic tick also
		// covers EINTR and a wake consumed before a handoff becomes ready.
		(void)::write(output.fd, &byte, 1);
	}
	void tick(bool pending) noexcept {
		pollfd poller{input.fd, POLLIN, 0};
		(void)::poll(&poller, 1, pending ? 20 : -1);
		std::array<char, 256> bytes;
		(void)::read(input.fd, bytes.data(), bytes.size());
	}
};
} // namespace

struct RetirementState {
	RetirementRuntimeState *runtime;
	std::shared_ptr<ByteAccount> account;
	std::size_t limit;
	std::atomic<std::uint64_t> admission{0};
	std::atomic<std::size_t> rolling_back{0};
	std::atomic<bool> closing{false};
	std::atomic<RetirementNode *> incoming{nullptr};
	RetirementNode *waiting = nullptr, *waiting_tail = nullptr;
	RetirementNode *runnable = nullptr, *runnable_tail = nullptr;
	RetirementNode *retry = nullptr, *retry_tail = nullptr;
	std::mutex drain_mutex;
	std::condition_variable drain_signal;
	std::atomic<bool> drained{false};
	void (*handoff_hook)(void *) noexcept = nullptr;
	void *handoff_context = nullptr;
	RetirementState(RetirementRuntimeState *runner, std::shared_ptr<ByteAccount> ledger, std::size_t owners)
		: runtime(runner), account(std::move(ledger)), limit(owners) {}
	static void push(RetirementNode *node, RetirementNode *&head, RetirementNode *&tail) noexcept {
		node->worker_next_ = nullptr;
		if (tail)
			tail->worker_next_ = node;
		else
			head = node;
		tail = node;
	}
	static RetirementNode *pop(RetirementNode *&head, RetirementNode *&tail) noexcept {
		auto node = head;
		if (node) {
			head = node->worker_next_;
			if (!head)
				tail = nullptr;
		}
		return node;
	}
	bool step(bool &work) noexcept {
		for (unsigned i = 0; i < 8 && retry; ++i)
			push(pop(retry, retry_tail), runnable, runnable_tail);
		if (auto head = incoming.exchange(nullptr, std::memory_order_acquire)) {
			push(head, waiting, waiting_tail);
			work = true;
		}
		for (unsigned i = 0; i < 8 && waiting; ++i) {
			auto node = pop(waiting, waiting_tail);
			if (!node->ready_.load(std::memory_order_acquire)) {
				push(node, waiting, waiting_tail);
				continue;
			}
			work = true;
			// Only readiness publishes next_. Never traverse an incomplete push.
			if (auto next = node->next_)
				push(next, waiting, waiting_tail);
			push(node, runnable, runnable_tail);
		}
		for (unsigned i = 0; i < 8 && runnable; ++i) {
			auto node = pop(runnable, runnable_tail);
			if (!node->retire_step()) {
				if (node->retry_later())
					push(node, retry, retry_tail);
				else {
					work = true;
					push(node, runnable, runnable_tail);
				}
				continue;
			}
			work = true;
			auto destroy = node->destroy_;
			destroy(node);
			admission.fetch_sub(1, std::memory_order_acq_rel);
		}
		return admission.load(std::memory_order_acquire) == closed &&
			   rolling_back.load(std::memory_order_acquire) == 0 && !waiting && !runnable && !retry &&
			   !incoming.load(std::memory_order_acquire);
	}
};

struct RetirementRuntimeState {
	struct Slot {
		std::shared_ptr<RetirementState> domain;
		bool reserved = false;
	};
	Wake wake;
	mutable std::mutex registry_mutex;
	std::vector<Slot, ChargedAllocator<Slot>> slots;
	std::size_t limit, stack_bytes;
	pthread_t worker{};
	bool stopping = false;
	std::atomic<bool> joined{false};
	RetirementRuntimeState(std::shared_ptr<ByteAccount> account, std::size_t domains, std::size_t stack)
		: slots(ChargedAllocator<Slot>(std::move(account))), limit(domains), stack_bytes(stack) {
		slots.resize(domains);
		pthread_attr_t attr;
		system_check(::pthread_attr_init(&attr), "retirement thread attributes");
		auto error = ::pthread_attr_setstacksize(&attr, stack);
		if (!error)
			error = ::pthread_create(
				&worker, &attr,
				[](void *context) -> void * {
					static_cast<RetirementRuntimeState *>(context)->run();
					return nullptr;
				},
				this);
		::pthread_attr_destroy(&attr);
		system_check(error, "retirement cleanup worker");
	}
	bool on_worker() const noexcept {
		return !joined.load(std::memory_order_acquire) && ::pthread_equal(worker, ::pthread_self());
	}
	void run() noexcept {
		std::array<std::shared_ptr<RetirementState>, registry_capacity> pass;
		for (;;) {
			bool pending = false;
			{
				std::lock_guard lock(registry_mutex);
				bool occupied = false;
				for (std::size_t i = 0; i < limit; ++i) {
					pass[i] = slots[i].domain;
					occupied |= slots[i].reserved || static_cast<bool>(pass[i]);
					pending |= slots[i].reserved;
				}
				if (stopping && !occupied)
					return;
			}
			bool work = false;
			for (std::size_t i = 0; i < limit; ++i) {
				auto &domain = pass[i];
				if (!domain)
					continue;
				if (!domain->step(work)) {
					pending |= domain->waiting != nullptr || domain->retry != nullptr ||
							   domain->rolling_back.load(std::memory_order_acquire) != 0 ||
							   (domain->closing.load(std::memory_order_acquire) &&
								!(domain->admission.load(std::memory_order_acquire) & closed));
					continue;
				}
				work = true;
				{
					std::lock_guard lock(registry_mutex);
					slots[i].domain.reset();
				}
				{
					std::lock_guard lock(domain->drain_mutex);
					domain->drained.store(true, std::memory_order_release);
				}
				domain->drain_signal.notify_all();
			}
			for (std::size_t i = 0; i < limit; ++i)
				pass[i].reset();
			if (!work)
				wake.tick(pending);
		}
	}
	void shutdown() {
		if (joined.load(std::memory_order_acquire))
			return;
		if (on_worker())
			throw std::logic_error("retirement worker cannot wait for shutdown");
		{
			std::lock_guard lock(registry_mutex);
			stopping = true;
			for (auto &slot : slots)
				if (slot.domain)
					slot.domain->admission.fetch_or(closed, std::memory_order_acq_rel);
		}
		wake.signal();
		system_check(::pthread_join(worker, nullptr), "retirement worker join");
		joined.store(true, std::memory_order_release);
	}
};

void retain(RetirementNode *node) noexcept {
	auto refs = node->refs_.load(std::memory_order_relaxed);
	for (;;) {
		if (!refs || refs == std::numeric_limits<std::uint64_t>::max())
			std::terminate();
		if (node->refs_.compare_exchange_weak(refs, refs + 1, std::memory_order_relaxed))
			return;
	}
}
void release(RetirementNode *node) noexcept {
	if (node->refs_.fetch_sub(1, std::memory_order_acq_rel) != 1)
		return;
	auto domain = node->domain_;
	node->next_ = domain->incoming.exchange(node, std::memory_order_acq_rel);
	domain->runtime->wake.signal();
	if (domain->handoff_hook)
		domain->handoff_hook(domain->handoff_context);
	node->ready_.store(true, std::memory_order_release);
	// The worker can now reclaim the node and domain. Touch neither again.
}
void admit_resident(RetirementState &state) {
	auto admission = state.admission.load(std::memory_order_relaxed);
	for (;;) {
		if (admission & closed) {
			throw std::logic_error("retirement admission closed");
		}
		if (admission >= state.limit) {
			throw ResourcePressure("retirement owner pressure");
		}
		if (state.admission.compare_exchange_weak(admission, admission + 1, std::memory_order_acq_rel))
			return;
	}
}
void undo_resident(RetirementState &state) noexcept {
	// The resident protects the runtime until rollback is registered. Keep the
	// periodic fallback active across the wake and final handoff to drainage.
	state.rolling_back.fetch_add(1, std::memory_order_acq_rel);
	state.runtime->wake.signal();
	state.admission.fetch_sub(1, std::memory_order_acq_rel);
	state.rolling_back.fetch_sub(1, std::memory_order_release);
	// No runtime access follows the final rollback handoff.
}
std::shared_ptr<ByteAccount> retirement_account(RetirementState &state) { return state.account; }
void bind_retirement(RetirementNode &node, RetirementState &state,
					 void (*destroy)(RetirementNode *) noexcept) noexcept {
	node.domain_ = &state;
	node.destroy_ = destroy;
}
void RetirementTesting::before_ready(RetirementDomain &domain, void (*hook)(void *) noexcept, void *context) {
	if (!domain.state_ || domain.residents())
		throw std::logic_error("retirement hook requires an empty domain");
	domain.state_->handoff_hook = hook;
	domain.state_->handoff_context = context;
}
} // namespace allocators::detail

namespace allocators {
RetirementRuntime::RetirementRuntime(std::shared_ptr<ByteAccount> account, std::size_t domains,
									 std::size_t stack_bytes)
	: baseline_account_(std::move(account)),
	  storage_(*(baseline_account_ ? baseline_account_
								   : throw std::invalid_argument("retirement baseline account missing")),
			   1) {
	if (!domains || domains > detail::registry_capacity)
		throw std::invalid_argument("invalid retirement registry limit");
	auto memory = storage_.allocate(sizeof(detail::RetirementRuntimeState));
	state_ = std::construct_at(static_cast<detail::RetirementRuntimeState *>(memory), baseline_account_,
							   domains, stack_bytes);
}
RetirementRuntime::~RetirementRuntime() {
	if (state_) {
		state_->shutdown();
		std::destroy_at(state_);
	}
}
RetirementDomain RetirementRuntime::domain(std::shared_ptr<ByteAccount> account, std::size_t owners) {
	if (!account || !owners || owners >= detail::closed)
		throw std::invalid_argument("invalid retirement domain");
	std::size_t index = 0;
	{
		std::lock_guard lock(state_->registry_mutex);
		if (state_->stopping)
			throw std::logic_error("retirement runtime stopped");
		while (index < state_->limit && (state_->slots[index].domain || state_->slots[index].reserved))
			++index;
		if (index == state_->limit)
			throw ResourcePressure("retirement domain pressure");
		state_->slots[index].reserved = true;
	}
	try {
		auto domain = std::allocate_shared<detail::RetirementState>(
			ChargedAllocator<detail::RetirementState>(account), state_, account, owners);
		std::lock_guard lock(state_->registry_mutex);
		if (state_->stopping)
			throw std::logic_error("retirement runtime stopped");
		state_->slots[index].domain = domain;
		state_->slots[index].reserved = false;
		return RetirementDomain(std::move(domain));
	} catch (...) {
		state_->wake.signal();
		std::lock_guard lock(state_->registry_mutex);
		state_->slots[index].reserved = false;
		throw;
	}
}
void RetirementRuntime::shutdown() { state_->shutdown(); }
bool RetirementRuntime::on_worker() const noexcept { return state_->on_worker(); }
std::size_t RetirementRuntime::occupied_domains() const noexcept {
	std::lock_guard lock(state_->registry_mutex);
	std::size_t count = 0;
	for (auto &slot : state_->slots)
		count += slot.reserved || static_cast<bool>(slot.domain);
	return count;
}
std::size_t RetirementRuntime::configured_stack_bytes() const noexcept { return state_->stack_bytes; }
std::size_t RetirementRuntime::fixed_heap_capacity() const noexcept {
	return storage_.used() + state_->slots.capacity() * sizeof(detail::RetirementRuntimeState::Slot) +
		ChargedAllocator<detail::RetirementRuntimeState::Slot>::allocation_overhead();
}
RetirementDomain::~RetirementDomain() { (void)close(); }
RetirementDrain RetirementDomain::close() noexcept {
	if (state_) {
		if (!(state_->admission.load(std::memory_order_acquire) & detail::closed)) {
			state_->closing.store(true, std::memory_order_release);
			state_->runtime->wake.signal();
			state_->admission.fetch_or(detail::closed, std::memory_order_acq_rel);
		}
	}
	return RetirementDrain(state_);
}
std::size_t RetirementDomain::residents() const noexcept {
	return state_ ? state_->admission.load(std::memory_order_acquire) & ~detail::closed : 0;
}
std::shared_ptr<ByteAccount> RetirementDomain::storage_account() const {
	if (!state_)
		throw std::logic_error("moved retirement domain");
	return state_->account;
}
bool RetirementDrain::done() const noexcept {
	return !state_ || state_->drained.load(std::memory_order_acquire);
}
void RetirementDrain::wait() const {
	if (!state_ || done())
		return;
	if (state_->runtime->on_worker())
		throw std::logic_error("retirement worker cannot wait for drainage");
	std::unique_lock lock(state_->drain_mutex);
	state_->drain_signal.wait(lock, [&] { return done(); });
}
} // namespace allocators
