#include "memory/general_memory_allocator.h"
#include "memory/memory_placement.h"
#include "util/exceptions.h"
#include <new>

namespace deluge::memory {
bool preferInternalNextAllocs = false;
} // namespace deluge::memory

// todo - make this work in unit tests, need to remove hard coded addresses in GMA
#if !IN_UNIT_TESTS
void* operator new(std::size_t n) noexcept(false) {
	// allocate on external RAM (or internal first, when the placement experiment switch is on -
	// allocMaxSpeed falls back to external itself if internal is full)
	void* addr = deluge::memory::preferInternalNextAllocs ? GeneralMemoryAllocator::get().allocMaxSpeed(n)
	                                                      : GeneralMemoryAllocator::get().allocExternal(n);
	if (addr == nullptr) [[unlikely]] {
		// The throwing operator new must never return null: std::string / std::vector and friends assume a non-null
		// buffer, so returning null silently corrupts them. Throw the same lightweight exception the custom STL
		// allocators (sdram_allocator / external_allocator) do, so existing `catch (deluge::exception)` out-of-memory
		// handlers can deal with it. Code that wants graceful failure uses the GeneralMemoryAllocator directly.
		throw deluge::exception::BAD_ALLOC;
	}
	return addr;
}

void operator delete(void* p) {
	delugeDealloc(p);
}

// C++14 sized deallocation. libstdc++ containers (std::string, std::vector, ...) call the sized form directly. On the
// device the default sized delete defers to the unsized one above, but a sanitizer substitutes its own sized-delete
// that does NOT defer - so without this override, std::string's buffer (allocated via our operator new) is freed
// through the sanitizer and reported as a bad free. Route it to the custom allocator like the unsized form.
void operator delete(void* p, std::size_t) noexcept {
	delugeDealloc(p);
}
#endif
