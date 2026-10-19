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
#include <chrono>
#include <iostream>
#include <optional>
#include <thread>
#include <vector>
namespace d = allocators;
struct JoiningThread : std::thread {
	using std::thread::thread;
	~JoiningThread() { if (joinable()) join(); }
};
void require(bool value) {
	if (!value)
		throw std::runtime_error("retirement invariant failed");
}
template <class Predicate> void eventually(Predicate predicate) {
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!predicate()) {
		if (std::chrono::steady_clock::now() >= deadline)
			throw std::runtime_error("retirement deadline exceeded");
		std::this_thread::yield();
	}
}
struct Owner : d::RetirementNode {
	d::RetirementRuntime &runtime;
	std::atomic<unsigned> &destroyed;
	std::atomic<bool> &wrong_thread;
	Owner(d::RetirementRuntime &runner, std::atomic<unsigned> &count, std::atomic<bool> &wrong)
		: runtime(runner), destroyed(count), wrong_thread(wrong) {}
	~Owner() {
		if (!runtime.on_worker())
			wrong_thread.store(true);
		destroyed.fetch_add(1);
	}
};
struct Family : Owner {
	std::array<d::RetirementHandle<Owner>, 16> children;
	std::size_t index = 0;
	using Owner::Owner;
	bool retire_step() noexcept override {
		if (index == children.size())
			return true;
		children[index++].reset();
		return false;
	}
};
struct Pause {
	std::atomic<bool> entered{false}, release{false};
	static void hook(void *context) noexcept {
		auto &pause = *static_cast<Pause *>(context);
		pause.entered.store(true, std::memory_order_release);
		while (!pause.release.load(std::memory_order_acquire))
			std::this_thread::yield();
	}
};
int main() {
	try {
		auto baseline = std::make_shared<d::ByteAccount>(1024 * 1024);
		auto account = std::make_shared<d::ByteAccount>(1024 * 1024);
		std::atomic<unsigned> destroyed{0};
		std::atomic<bool> wrong_thread{false};
		{
			d::RetirementRuntime runtime(baseline, 2);
			require(baseline->used() == runtime.fixed_heap_capacity());
			require(runtime.configured_stack_bytes() == 1024 * 1024);
			{
				auto domain = runtime.domain(account, 17);
				auto family = domain.make<Family>(runtime, destroyed, wrong_thread);
				for (auto &child : family->children)
					child = domain.make<Owner>(runtime, destroyed, wrong_thread);
				require(domain.residents() == 17);
				bool pressure = false;
				try {
					domain.make<Owner>(runtime, destroyed, wrong_thread);
				} catch (const d::ResourcePressure &) {
					pressure = true;
				}
				require(pressure);
				auto copy = family;
				family.reset();
				auto drain = domain.close();
				require(!drain.done() && destroyed == 0);
				bool closed = false;
				try {
					domain.make<Owner>(runtime, destroyed, wrong_thread);
				} catch (const std::logic_error &) {
					closed = true;
				}
				require(closed);
				copy.reset();
				drain.wait();
				require(domain.residents() == 0 && destroyed == 17 && !wrong_thread);
			}
			eventually([&] { return account->used() == 0; });
			{
				auto domain = runtime.domain(account);
				Pause pause;
				d::detail::RetirementTesting::before_ready(domain, Pause::hook, &pause);
				auto owner = domain.make<Owner>(runtime, destroyed, wrong_thread);
				auto drain = domain.close();
				JoiningThread releaser([held = std::move(owner)]() mutable { held.reset(); });
				struct Unpause {
					Pause &pause;
					~Unpause() { pause.release.store(true, std::memory_order_release); }
				} unpause{pause};
				eventually([&] { return pause.entered.load(std::memory_order_acquire); });
				// Give the worker several ticks to consume the wake and detach the
				// unready mailbox head. It must retain the node and its domain.
				std::this_thread::sleep_for(std::chrono::milliseconds(80));
				require(!drain.done() && domain.residents() == 1 && destroyed == 17);
				pause.release.store(true, std::memory_order_release);
				drain.wait();
				require(destroyed == 18 && !wrong_thread);
			}
			eventually([&] { return account->used() == 0; });
			{
				auto first = runtime.domain(account), second = runtime.domain(account);
				auto a = first.make<Owner>(runtime, destroyed, wrong_thread);
				auto b = second.make<Owner>(runtime, destroyed, wrong_thread);
				auto first_drain = first.close(), second_drain = second.close();
				require(runtime.occupied_domains() == 2);
				bool pressure = false;
				try {
					runtime.domain(account);
				} catch (const d::ResourcePressure &) {
					pressure = true;
				}
				require(pressure);
				a.reset();
				first_drain.wait();
				auto replacement = runtime.domain(account);
				auto replacement_drain = replacement.close();
				replacement_drain.wait();
				require(!second_drain.done());
				// The second owner remains valid long after facade close.
				auto late_copy = b;
				b.reset();
				require(!second_drain.done());
				JoiningThread original([held = std::move(late_copy)]() mutable { held.reset(); });
				second_drain.wait();
			}
			eventually([&] { return account->used() == 0 && runtime.occupied_domains() == 0; });
			{
				auto zero = std::make_shared<d::ByteAccount>(0);
				bool pressure = false;
				try {
					runtime.domain(zero);
				} catch (const d::ResourcePressure &) {
					pressure = true;
				}
				require(pressure && runtime.occupied_domains() == 0);
				auto domain = runtime.domain(account);
				const auto controls = account->used();
				struct Failing : d::RetirementNode {
					Failing() { throw std::runtime_error("owner constructor failure"); }
				};
				bool failed = false;
				try {
					domain.make<Failing>();
				} catch (const std::runtime_error &) {
					failed = true;
				}
				require(failed && domain.residents() == 0 && account->used() == controls);
				auto drain = domain.close();
				drain.wait();
			}
			// Closing while an admitted constructor is blocked must retain the
			// runtime through either successful construction or rollback.
			for (bool fail : {false, true}) {
				auto domain = runtime.domain(account);
				Pause pause;
				struct Constructing : d::RetirementNode {
					Constructing(Pause &pause, bool fail) {
						Pause::hook(&pause);
						if (fail)
							throw std::runtime_error("blocked constructor failure");
					}
				};
				std::atomic<bool> completed{false};
				JoiningThread constructor([&] {
					try {
						auto owner = domain.make<Constructing>(pause, fail);
					} catch (const std::runtime_error &) {
						if (!fail)
							std::terminate();
					}
					completed.store(true);
				});
				struct Unpause {
					Pause &pause;
					~Unpause() { pause.release.store(true); }
				} unpause{pause};
				eventually([&] { return pause.entered.load(); });
				auto drain = domain.close();
				require(domain.residents() == 1 && !drain.done());
				pause.release.store(true);
				drain.wait();
				eventually([&] { return completed.load(); });
				require(domain.residents() == 0);
			}
			{
				// A deferred cleanup retains its charge and registry slot while
				// allowing another domain to finish its own retirement.
				auto first = runtime.domain(account), second = runtime.domain(account);
				std::atomic<bool> allow{false};
				std::atomic<unsigned> attempts{0};
				struct Retrying : d::RetirementNode {
					std::atomic<bool> &allow;
					std::atomic<unsigned> &attempts;
					Retrying(std::atomic<bool> &ready, std::atomic<unsigned> &count)
						: allow(ready), attempts(count) {}
					bool retire_step() noexcept override {
						attempts.fetch_add(1);
						return allow.load();
					}
					bool retry_later() const noexcept override { return true; }
				};
				auto blocked = first.make<Retrying>(allow, attempts);
				auto ordinary = second.make<Owner>(runtime, destroyed, wrong_thread);
				auto blocked_drain = first.close(), ordinary_drain = second.close();
				struct Permit {
					std::atomic<bool> &allow;
					~Permit() { allow.store(true); }
				} permit{allow};
				blocked.reset();
				ordinary.reset();
				ordinary_drain.wait();
				eventually([&] { return attempts.load() != 0; });
				require(!blocked_drain.done() && first.residents() == 1 && account->used() != 0);
				allow.store(true);
				blocked_drain.wait();
				require(first.residents() == 0);
			}
			for (unsigned i = 0; i < 100; ++i) {
				auto domain = runtime.domain(account);
				auto owner = domain.make<Owner>(runtime, destroyed, wrong_thread);
				auto drain = domain.close();
				owner.reset();
				drain.wait();
				require(baseline->used() == runtime.fixed_heap_capacity());
			}
			runtime.shutdown();
			require(account->used() == 0 && !wrong_thread);
			bool stopped = false;
			try {
				runtime.domain(account);
			} catch (const std::logic_error &) {
				stopped = true;
			}
			require(stopped);
		}
		// A drained facade may remain as a closed admission capability after
		// its application's runtime has been joined and destroyed.
		std::optional<d::RetirementDomain> closed_domain;
		{
			d::RetirementRuntime runtime(baseline, 1);
			closed_domain.emplace(runtime.domain(account));
			closed_domain->close().wait();
			runtime.shutdown();
		}
		struct Empty : d::RetirementNode {};
		bool closed = false;
		try {
			closed_domain->make<Empty>();
		} catch (const std::logic_error &) {
			closed = true;
		}
		require(closed);
		closed_domain.reset();
		require(baseline->used() == 0 && account->used() == 0);
		{
			d::RetirementRuntime runtime(baseline, 512);
			std::vector<d::RetirementDomain> domains;
			std::vector<d::RetirementHandle<Owner>> owners;
			domains.reserve(512);
			owners.reserve(512);
			for (unsigned index = 0; index < 512; ++index) {
				domains.push_back(runtime.domain(account, 1));
				owners.push_back(domains.back().make<Owner>(runtime, destroyed, wrong_thread));
			}
			require(runtime.occupied_domains() == 512);
			bool full = false;
			try {
				runtime.domain(account, 1);
			} catch (const d::ResourcePressure &) {
				full = true;
			}
			require(full);
			for (auto &domain : domains)
				domain.close();
			require(runtime.occupied_domains() == 512);
			std::array<std::thread, 8> releases;
			for (unsigned index = 0; index < releases.size(); ++index)
				releases[index] = std::thread([&, index] {
					for (std::size_t slot = index; slot < owners.size(); slot += releases.size())
						owners[slot].reset();
				});
			for (auto &release : releases)
				release.join();
			for (auto &domain : domains)
				domain.close().wait();
			require(runtime.occupied_domains() == 0 && !wrong_thread.load());
		}
		require(baseline->used() == 0 && account->used() == 0);
		{
			auto tiny = std::make_shared<d::ByteAccount>(512);
			bool refused = false;
			try {
				d::RetirementRuntime runtime(tiny, 512);
			} catch (const d::ResourcePressure &) {
				refused = true;
			}
			require(refused && tiny->used() == 0);
		}
		std::cout << "intrusive retirement, handoff races and bounded domains passed\n";
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
