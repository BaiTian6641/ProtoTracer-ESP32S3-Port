#pragma once
#include <cstdlib>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
inline unsigned debugAllocations = 0;
inline bool debugAllocationFailure = false;
inline void* heap_caps_malloc(size_t bytes, unsigned)
{
    if (debugAllocationFailure) return nullptr;
    void* ptr = malloc(bytes);
    if (ptr) ++debugAllocations;
    return ptr;
}
inline void heap_caps_free(void* ptr)
{
    if (ptr) --debugAllocations;
    free(ptr);
}
inline bool heap_caps_check_integrity_all(bool) { return true; }
