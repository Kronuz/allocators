#include <allocators/allocation.h>
#include <allocators/memory_resource.h>
#include <iostream>
int main() {
	auto account = std::make_shared<allocators::ByteAccount>(4096);
	auto resource = allocators::make_charged_resource(account);
	auto value = std::allocate_shared<int>(
		allocators::OwnedAllocator<int>(allocators::AllocationContext(resource)), 42);
	auto unique = allocators::make_owned_unique<int>(allocators::AllocationContext(resource), 7);
	std::cout << *value << ", " << *unique << ": " << account->used() << " admitted bytes\n";
}
