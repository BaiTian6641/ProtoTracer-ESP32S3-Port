// ProtoGC global operator new/delete override.
// Routes every C++ heap allocation through ProtoGC::heapAlloc so the
// PSRAM/internal-SRAM placement policy is consistent with the rest of the
// runtime instead of relying on libstdc++'s default malloc-only path.
//
// Behavior:
//   * Disabled by default. Caller (typically main.cpp during ProtoGC::begin)
//     must call protogc::ProtoGC::enableNewDeleteTakeover(true) to activate.
//     Until then the override falls back to heap_caps_malloc(MALLOC_CAP_8BIT),
//     which is identical to the libc default.
//   * When active: try PSRAM first (managed coalescing heap, then fallback),
//     then internal SRAM, then bail with std::bad_alloc / nullptr.
//   * DMA / EXEC capable objects are NOT created via operator new in this
//     codebase — those allocations stay on direct heap_caps_malloc calls.
//
// We define the global operator new in a single TU so the linker pulls this
// once and the override applies to every .cpp compiled into the firmware.

#include "ProtoGC.h"

#include <cstdlib>
#include <new>

#include <esp_heap_caps.h>

namespace {

inline void* protogc_global_alloc(std::size_t size) noexcept {
    if (size == 0) size = 1;

    if (protogc::ProtoGC::isNewDeleteTakeoverEnabled() && !protogc::ProtoGC::isInIsrContext()) {
        if (void* p = protogc::ProtoGC::heapAlloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) {
            return p;
        }
        if (void* p = protogc::ProtoGC::heapAlloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) {
            return p;
        }
    }

    if (void* p = heap_caps_malloc(size, MALLOC_CAP_8BIT)) return p;
    if (void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) return p;
    if (void* p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) return p;
    return nullptr;
}

inline void protogc_global_free(void* ptr) noexcept {
    if (!ptr) return;

    if (protogc::ProtoGC::isInIsrContext()) {
        protogc::ProtoGC::deferHeapFree(ptr);
        return;
    }

    // ProtoGC::heapFree handles managed-heap and fallback-list pointers.
    // For pointers minted by raw heap_caps_malloc (boot-time, takeover off),
    // it returns 0 and we fall back to heap_caps_free.
    if (!protogc::ProtoGC::heapFree(ptr)) {
        heap_caps_free(ptr);
    }
}

} // namespace

void* operator new(std::size_t size) {
    if (void* p = protogc_global_alloc(size)) return p;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    if (void* p = protogc_global_alloc(size)) return p;
    throw std::bad_alloc();
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return protogc_global_alloc(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return protogc_global_alloc(size);
}

void operator delete(void* ptr) noexcept { protogc_global_free(ptr); }
void operator delete[](void* ptr) noexcept { protogc_global_free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { protogc_global_free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { protogc_global_free(ptr); }
void operator delete(void* ptr, const std::nothrow_t&) noexcept { protogc_global_free(ptr); }
void operator delete[](void* ptr, const std::nothrow_t&) noexcept { protogc_global_free(ptr); }

// C-style helper for code that wants a glibc-malloc_trim-shaped entry point.
// ESP-IDF newlib already defines `malloc_trim` (returning 0), so we expose
// ours under a distinct name. Projects can map libc malloc_trim onto this by
// passing `-Wl,--wrap=malloc_trim` and providing __wrap_malloc_trim if needed.
extern "C" int protogc_malloc_trim(size_t pad) {
    return protogc::ProtoGC::mallocTrim(pad, false) > 0 ? 1 : 0;
}
