#pragma once
// Host-test shim for ESP-IDF esp_heap_caps.h (Windows x64, MSVC).
// All capability bits are accepted and everything maps to the host heap.

#include <cstddef>
#include <cstdlib>

#ifndef MALLOC_CAP_SPIRAM
#define MALLOC_CAP_SPIRAM 4
#endif
#ifndef MALLOC_CAP_INTERNAL
#define MALLOC_CAP_INTERNAL 8
#endif
#ifndef MALLOC_CAP_8BIT
#define MALLOC_CAP_8BIT 16
#endif
#ifndef MALLOC_CAP_DMA
#define MALLOC_CAP_DMA 32
#endif

inline void* heap_caps_malloc(size_t size, uint32_t caps) {
    (void)caps;
    return std::malloc(size);
}

inline void* heap_caps_calloc(size_t count, size_t size, uint32_t caps) {
    (void)caps;
    return std::calloc(count, size);
}

inline void* heap_caps_realloc(void* ptr, size_t size, uint32_t caps) {
    (void)caps;
    return std::realloc(ptr, size);
}

inline void* heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps) {
    (void)caps;
    // Host malloc already returns 16-byte-aligned pointers on x64, and the
    // pointer is freed with heap_caps_free() (= free), so plain malloc keeps
    // the alloc/free pair consistent.
    (void)alignment;
    return std::malloc(size);
}

inline void heap_caps_free(void* ptr) {
    std::free(ptr);
}

inline size_t heap_caps_get_free_size(uint32_t caps) {
    (void)caps;
    return static_cast<size_t>(1) << 30; // plenty; never a constraint on host
}

inline size_t heap_caps_get_largest_free_block(uint32_t caps) {
    (void)caps;
    return static_cast<size_t>(1) << 30;
}

inline void heap_caps_malloc_extmem_enable(size_t threshold) {
    (void)threshold;
}
