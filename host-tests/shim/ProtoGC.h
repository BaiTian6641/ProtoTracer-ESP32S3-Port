#pragma once
// Host-test shim for ProtoGC (Windows x64, MSVC).
//
// The real ProtoGC (lib in .pio/libdeps) is a FreeRTOS/ESP-IDF cooperative
// garbage collector; on the host every allocation is a plain malloc and
// collect/poll/lock are no-ops. Only the API surface used by the render core
// is provided:
//
//   protogc::ProtoGC::psramAlloc / internalAlloc / heapFree / heapRealloc
//   protogc::ProtoGC::begin / poll / collectLight / collectFull / emergency
//   protogc::ProtoGC::lockMallocToPsram
//   protogc::HeapGuard::onWarning / onCritical
//
// Kept in sync with: src/Render/Camera.h, Scene.h, PixelGroup.h, QuadTree.h.

#include <cstddef>
#include <cstdlib>
#include <cstdint>

namespace protogc {

struct HeapGuard {
    enum class Level { OK, Warning, Critical };

    using Callback = void (*)(Level level, size_t freeBytes, size_t largestBlock);

    static void onWarning(Callback) {}
    static void onCritical(Callback) {}
};

struct ProtoGC {
    static void* psramAlloc(size_t sizeBytes) {
        return std::malloc(sizeBytes ? sizeBytes : 1);
    }

    static void* internalAlloc(size_t sizeBytes) {
        return std::malloc(sizeBytes ? sizeBytes : 1);
    }

    static bool heapFree(void* ptr) {
        std::free(ptr);
        return true;
    }

    static void* heapRealloc(void* ptr, size_t newSize, uint32_t caps = 0) {
        (void)caps;
        if (!ptr) return std::malloc(newSize ? newSize : 1);
        return std::realloc(ptr, newSize);
    }

    static void* heapAlloc(size_t sizeBytes, uint32_t caps = 0) {
        (void)caps;
        return std::malloc(sizeBytes ? sizeBytes : 1);
    }

    static void begin() {}
    static void poll() {}
    static void collectLight(const char* = nullptr) {}
    static void collectFull(const char* = nullptr) {}
    static void emergency(const char* = nullptr) {}
    static void lockMallocToPsram(size_t = 0) {}
    static bool isInitialized() { return true; }
};

} // namespace protogc
