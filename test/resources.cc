#include "allocators/resources.h"
#include "allocators/memory_resource.h"
#include "allocators/allocation.h"
#include <atomic>
#include <iostream>
#include <thread>
#include <vector>
#include <optional>
#include <array>

using namespace allocators;
namespace {
void check(bool condition) {
	if (!condition) throw std::runtime_error("resource invariant failed");
}
struct alignas(256) Aligned { char value[256]; };
struct Forward;
struct ForwardOwner { OwnedUnique<Forward> value; ~ForwardOwner(); };
struct Forward { unsigned *destroyed; ~Forward() { ++*destroyed; } };
ForwardOwner::~ForwardOwner() = default;
void unique_ownership() {
	auto account = std::make_shared<ByteAccount>(1024 * 1024);
	auto resource = make_charged_resource(account);
	std::weak_ptr<std::pmr::memory_resource> weak = resource;
	auto owned = make_owned_unique<Aligned>(AllocationContext(resource));
	check(reinterpret_cast<std::uintptr_t>(owned.get()) % alignof(Aligned) == 0);
	resource.reset();
	auto moved = std::move(owned);
	check(!owned && !weak.expired());
	moved.reset();
	check(!weak.expired());
	moved = OwnedUnique<Aligned>{};
	check(weak.expired() && account->used() > 0);
	weak.reset();
	check(account->used() == 0);
	resource = make_charged_resource(account);
	const auto fixed = account->used();
	struct Failure {};
	struct Failed { Failed() { throw Failure{}; } };
	try { auto value = make_owned_unique<Failed>(AllocationContext(resource)); check(false); }
	catch (const Failure &) {}
	check(account->used() == fixed);
	{
		auto full = account->admit(account->limit() - account->used());
		try { auto value = make_owned_unique<Aligned>(AllocationContext(resource)); check(false); }
		catch (const ResourcePressure &) {}
		check(account->used() == account->limit());
	}
	unsigned destroyed = 0;
	{
		ForwardOwner forward;
		forward.value = make_owned_unique<Forward>(AllocationContext(resource), &destroyed);
	}
	check(destroyed == 1 && account->used() == fixed);
	resource.reset();
	check(account->used() == 0);
}
void reservations() {
	using V = ResourceVector<2>;
	const V hard{{200, 20}};
	ReservationBook<2> book(hard, V{{20, 2}});
	std::array<std::optional<Reservation<2>>, 8> slots;
	V model_used{{20, 2}}, model_reserved{};
	std::uint32_t random = 17;
	for (unsigned turn = 0; turn < 10000; ++turn) {
		random = random * 1664525u + 1013904223u;
		auto &slot = slots[(random >> 8) % slots.size()];
		const V amount{{(random >> 16) % 25, (random >> 24) % 3}};
		if (!slot) {
			slot = book.try_reserve(amount, hard);
			if (slot) model_reserved = model_reserved.added(amount);
		} else if (random % 3 == 0) {
			const auto previous = slot->peak();
			if (book.try_grow(*slot, amount, hard)) {
				model_reserved = model_reserved.subtracted(previous).added(slot->peak());
			} else check(previous == slot->peak());
		} else if (random % 3 == 1) {
			model_reserved = model_reserved.subtracted(slot->peak());
			book.cancel(*slot);
			slot.reset();
		} else {
			const auto effects = slot->peak();
			model_reserved = model_reserved.subtracted(effects);
			book.settle(*slot, effects);
			model_used = model_used.added(effects);
			slot.reset();
		}
		if (random % 7 == 0) {
			const V credit{{model_used.values[0] / 2, model_used.values[1] / 2}};
			book.reclaim(credit);
			model_used = model_used.subtracted(credit);
		}
		check(book.used() == model_used && book.reserved() == model_reserved && !book.abandoned());
		check(book.used().added(book.reserved()).fits(hard));
	}
	for (auto &slot : slots) if (slot) { book.cancel(*slot); slot.reset(); }
	check(book.reserved() == V{});
	ReservationBook<2> over(hard, V{{201, 21}});
	check(!over.try_reserve(V{}, hard));
	over.reclaim(V{{1, 1}});
	{ auto debt = over.try_reserve(V{}, hard); check(debt.has_value()); }
	check(over.abandoned() && !over.try_reserve(V{}, hard));
	ReservationBook<2> atomic(hard, V{{100, 10}});
	auto atomic_held = atomic.try_reserve(V{{50, 5}}, hard);
	check(atomic_held.has_value());
	const auto atomic_used = atomic.used(), atomic_reserved = atomic.reserved();
	try { atomic.settle(*atomic_held, V{{1, 1}}, V{{1, 11}}); check(false); }
	catch (const std::invalid_argument &) {}
	check(atomic.used() == atomic_used && atomic.reserved() == atomic_reserved && bool(*atomic_held));
	atomic.cancel(*atomic_held);
	ReservationBook<2> explicit_abandon(hard);
	auto lost = explicit_abandon.try_reserve(V{{50, 5}}, hard);
	check(lost.has_value());
	const auto lost_peak = lost->peak();
	lost->abandon();
	check(!bool(*lost) && lost->peak() == lost_peak && explicit_abandon.reserved() == lost_peak && explicit_abandon.abandoned());
	ReservationBook<2> abandoned(hard);
	{ auto debt = abandoned.try_reserve(V{{50, 5}}, hard); check(debt.has_value()); }
	check(abandoned.abandoned() && abandoned.reserved() == V{{50, 5}});
	check(!abandoned.try_reserve(V{{1, 1}}, hard));
	ReservationBook<2> pressure(hard);
	auto held = pressure.try_reserve(V{{100, 10}}, hard);
	check(held.has_value());
	const auto before = pressure.reserved();
	check(!pressure.try_grow(*held, V{{150, 15}}, V{{49, 5}}));
	check(before == pressure.reserved() && before == held->peak());
	try { book.cancel(*held); check(false); } catch (const std::invalid_argument &) {}
	try { pressure.settle(*held, V{{101, 10}}); check(false); }
	catch (const std::invalid_argument &) {}
	check(pressure.reserved() == before);
	pressure.cancel(*held);
	try { pressure.cancel(*held); check(false); } catch (const std::invalid_argument &) {}
}
void hierarchy() {
	auto parent = std::make_shared<ByteAccount>(100);
	auto child = std::make_shared<ByteAccount>(parent, 80);
	auto held = parent->admit(90);
	try { auto failed = child->admit(11); check(false); }
	catch (const ResourcePressure &) {}
	check(child->used() == 0 && child->peak() == 0 && parent->used() == 90);
	held.reset();
	auto charge = child->admit(80);
	check(parent->used() == 80 && child->used() == 80 && child->peak() == 80);
	child.reset();
	check(parent->used() == 80);
	charge.reset();
	check(parent->used() == 0);
	ByteCharge survivor;
	{ ByteAccount local(16); survivor = local.admit(16); }
	survivor.reset();
	std::unique_ptr<PagedStorage> pages;
	{ ByteAccount local(4096); pages = std::make_unique<PagedStorage>(local, 128); }
	(void)pages->allocate(256);
	pages.reset();
}
void concurrency() {
	auto root = std::make_shared<ByteAccount>(32);
	auto left = std::make_shared<ByteAccount>(root, 32);
	auto right = std::make_shared<ByteAccount>(root, 32);
	std::atomic<bool> failed{false};
	std::vector<std::thread> workers;
	for (unsigned i = 0; i < 8; ++i) workers.emplace_back([&, i] {
		for (unsigned n = 0; n < 10000; ++n) {
			try { auto charge = (i % 2 ? left : right)->admit(8);
				if (root->used() > 32) failed = true;
			} catch (const ResourcePressure &) {}
		}
	});
	for (auto &worker : workers) worker.join();
	workers.clear();
	check(!failed && root->used() == 0 && left->used() == 0 && right->used() == 0);
	check(root->peak() <= 32);
}
void ownership() {
	auto account = std::make_shared<ByteAccount>(1024 * 1024);
	auto resource = make_charged_resource(account);
	std::weak_ptr<std::pmr::memory_resource> weak_resource = resource;
	auto owned = std::allocate_shared<Aligned>(OwnedAllocator<Aligned>(AllocationContext(resource)));
	check(reinterpret_cast<std::uintptr_t>(owned.get()) % alignof(Aligned) == 0);
	std::weak_ptr<Aligned> weak = owned;
	resource.reset();
	owned.reset();
	check(!weak_resource.expired() && account->used() > 0);
	weak.reset();
	check(weak_resource.expired() && account->used() > 0);
	weak_resource.reset();
	check(account->used() == 0);
	const auto previous = account->used();
	ChargedAllocator<int> allocator(account, [](std::size_t) -> void * { return nullptr; });
	try { (void)allocator.allocate(1); check(false); } catch (const std::bad_alloc &) {}
	check(account->used() == previous);
	struct ConstructorFailure {};
	struct Failed { Failed() { throw ConstructorFailure{}; } };
	try { auto failed = make_charged_owner<Failed>(account, std::malloc, std::free); check(false); }
	catch (const ConstructorFailure &) {}
	check(account->used() == previous);
	auto pmr = make_charged_resource(account);
	const auto bootstrap = account->used();
	try { (void)pmr->allocate(std::numeric_limits<std::size_t>::max(), 4096); check(false); }
	catch (const std::bad_alloc &) {}
	check(account->used() == bootstrap);
	pmr.reset();
	check(account->used() == previous);
}
}
int main() {
	reservations(); hierarchy(); concurrency(); ownership(); unique_ownership();
	std::cout << "resource hierarchy, concurrent rollback and ownership checks passed\n";
}
