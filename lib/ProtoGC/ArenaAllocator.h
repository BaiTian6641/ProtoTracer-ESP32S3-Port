#pragma once
// ProtoGC ArenaAllocator — bump-pointer allocator for scoped allocations.
// All allocations come from a single contiguous PSRAM buffer. Individual
// frees are NOT supported — the entire arena is reset at once.
//
// Ideal for JSON parsing, temporary String building, and any workload
// where many small allocations share a lifetime.
//
// Usage:
//   ArenaAllocator arena(65536);          // 64 KB arena
//   auto* jsonBuf = arena.alloc(32768);   // 32 KB for JSON doc
//   auto* strBuf  = arena.alloc(1024);    // 1 KB for temp string
//   arena.reset();                         // free everything at once

#include <cstddef>
#include <cstdint>
#include <esp_heap_caps.h>

namespace protogc {

class ArenaAllocator {
    uint8_t* mBuffer;     // backing PSRAM buffer
    size_t   mCapacity;   // total size in bytes
    size_t   mOffset;     // current bump-pointer offset
    size_t   mPeakOffset; // high-water mark
    size_t   mAllocCount; // number of allocations since last reset

public:
    ArenaAllocator() : mBuffer(nullptr), mCapacity(0), mOffset(0),
                       mPeakOffset(0), mAllocCount(0) {}

    explicit ArenaAllocator(size_t capacityBytes)
        : mBuffer(nullptr), mCapacity(0), mOffset(0),
          mPeakOffset(0), mAllocCount(0) {
        begin(capacityBytes);
    }

    ~ArenaAllocator() {
        if (mBuffer) heap_caps_free(mBuffer);
    }

    // Allocate backing buffer from PSRAM. Returns false on failure.
    bool begin(size_t capacityBytes) {
        if (mBuffer) heap_caps_free(mBuffer);
        mBuffer = static_cast<uint8_t*>(
            heap_caps_malloc(capacityBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!mBuffer) {
            mCapacity = 0;
            return false;
        }
        mCapacity   = capacityBytes;
        mOffset     = 0;
        mPeakOffset = 0;
        mAllocCount = 0;
        return true;
    }

    bool ensureCapacity(size_t capacityBytes) {
        if (mBuffer && mCapacity >= capacityBytes) {
            reset();
            return true;
        }
        return begin(capacityBytes);
    }

    // Allocate from the arena. Returns nullptr if out of space.
    // Alignment defaults to 4 bytes (suitable for float/int/pointer).
    void* alloc(size_t sizeBytes, size_t alignment = 4) {
        if (!mBuffer) return nullptr;

        if ((alignment & (alignment - 1)) != 0) {
            alignment = 4;
        }

        // Align offset
        const size_t mask = alignment - 1;
        const size_t alignedOffset = (mOffset + mask) & ~mask;

        if (alignedOffset + sizeBytes > mCapacity) return nullptr;

        void* ptr = mBuffer + alignedOffset;
        mOffset = alignedOffset + sizeBytes;
        if (mOffset > mPeakOffset) mPeakOffset = mOffset;
        ++mAllocCount;
        return ptr;
    }

    // Reset the arena — all previous allocations are invalidated.
    size_t reset() {
        const size_t reclaimed = mOffset;
        mOffset     = 0;
        mAllocCount = 0;
        return reclaimed;
    }

    // Release backing buffer entirely.
    size_t end() {
        const size_t released = mBuffer ? mCapacity : 0;
        if (mBuffer) { heap_caps_free(mBuffer); mBuffer = nullptr; }
        mCapacity   = 0;
        mOffset     = 0;
        mPeakOffset = 0;
        mAllocCount = 0;
        return released;
    }

    bool owns(void* ptr) const {
        if (!mBuffer || !ptr) return false;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mBuffer);
        const uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
        return p >= base && p < base + mCapacity;
    }

    // Statistics
    size_t capacity()    const { return mCapacity; }
    size_t used()        const { return mOffset; }
    size_t peakUsed()    const { return mPeakOffset; }
    size_t remaining()   const { return mCapacity - mOffset; }
    size_t allocCount()  const { return mAllocCount; }
    bool   isReady()     const { return mBuffer != nullptr; }
};

} // namespace protogc
