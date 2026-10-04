#pragma once
#include <cstdlib>
#include <cstddef>
constexpr unsigned MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
inline unsigned materialAllocations = 0;
inline bool materialAllocationFailure = false;
inline void* heap_caps_malloc(size_t bytes, unsigned caps)
{
    if (caps != (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) || materialAllocationFailure) return nullptr;
    void* pointer = std::malloc(bytes);
    if (pointer) ++materialAllocations;
    return pointer;
}
inline void heap_caps_free(void* pointer)
{
    if (pointer) { --materialAllocations; std::free(pointer); }
}
