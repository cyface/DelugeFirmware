// NeuralAmpModelerCore's sources pull in <iostream>/<sstream> and JSON float parsing,
// which reference newlib's reentrant allocator (_malloc_r and friends, e.g. from strtod's
// arbitrary-precision buffers). The firmware never links newlib's own malloc - route these
// to the Deluge allocator, like src/malloc.c already does for plain malloc/free.
// The file-descriptor syscalls that <iostream> also drags in come from src/sys_stubs.c,
// which is added to the nam_core build alongside this file.

#include "memory/general_memory_allocator.h"

#include <cstdlib>
#include <cstring>
#include <reent.h>

extern "C" {

void* _malloc_r(struct _reent*, size_t size) {
	return malloc(size);
}

void _free_r(struct _reent*, void* ptr) {
	free(ptr);
}

void* _calloc_r(struct _reent*, size_t num, size_t size) {
	void* ptr = malloc(num * size);
	if (ptr != nullptr) {
		memset(ptr, 0, num * size);
	}
	return ptr;
}

void* _realloc_r(struct _reent*, void* old, size_t newSize) {
	if (old == nullptr) {
		return malloc(newSize);
	}
	if (newSize == 0) {
		free(old);
		return nullptr;
	}
	uint32_t oldSize = GeneralMemoryAllocator::get().getAllocatedSize(old);
	void* fresh = malloc(newSize);
	if (fresh != nullptr) {
		memcpy(fresh, old, (oldSize < newSize) ? oldSize : newSize);
		free(old);
	}
	return fresh;
}
}
