#pragma once
// ProtoGC PoolAllocator — fragmentation-free fixed-size block allocator.
// Uses a singly-linked free list. All backing memory lives in PSRAM so
// internal DRAM stays reserved for DMA (HUB75, BLE, Camera SIMD).
//
// Usage:
//   PoolAllocator<128, 64> pool;  // 64 blocks of 128 bytes each
//   void* p = pool.alloc();
//   pool.free(p);

#include <cstddef>
#include <cstdint>
#include <new>
#include <esp_heap_caps.h>

namespace protogc {

template <size_t BlockSize, size_t BlockCount>
class PoolAllocator {
    static_assert(BlockSize >= sizeof(void*), "BlockSize must be at least sizeof(void*)");
    static_assert(BlockCount > 0, "BlockCount must be > 0");

    struct FreeNode { FreeNode* next; };

    uint8_t*  mBuffer;       // backing PSRAM buffer
    FreeNode* mFreeList;     // singly-linked free list
    size_t    mAllocCount;   // currently allocated blocks
    size_t    mPeakCount;    // peak concurrent allocations

public:
    PoolAllocator() : mBuffer(nullptr), mFreeList(nullptr), mAllocCount(0), mPeakCount(0) {}

    ~PoolAllocator() {
        if (mBuffer) heap_caps_free(mBuffer);
    }

    // Allocate the backing buffer from PSRAM and build the free list.
    // Returns false if PSRAM allocation fails (pool will be disabled).
    bool begin() {
        if (mBuffer) return true; // already initialized

        const size_t totalBytes = BlockSize * BlockCount;
        mBuffer = static_cast<uint8_t*>(
            heap_caps_malloc(totalBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!mBuffer) return false;

        // Build free list: link all blocks
        mFreeList = reinterpret_cast<FreeNode*>(mBuffer);
        FreeNode* cur = mFreeList;
        for (size_t i = 0; i < BlockCount - 1; ++i) {
            FreeNode* next = reinterpret_cast<FreeNode*>(
                mBuffer + (i + 1) * BlockSize);
            cur->next = next;
            cur = next;
        }
        cur->next = nullptr;
        return true;
    }

    // Allocate one block. Returns nullptr if pool is exhausted.
    void* alloc() {
        if (!mFreeList) return nullptr;
        FreeNode* node = mFreeList;
        mFreeList = node->next;
        ++mAllocCount;
        if (mAllocCount > mPeakCount) mPeakCount = mAllocCount;
        return static_cast<void*>(node);
    }

    // Return a block to the pool. Passing nullptr or a non-pool pointer is safe (no-op).
    void free(void* ptr) {
        if (!ptr) return;
        // Bounds check — silently ignore foreign pointers
        const uintptr_t base = reinterpret_cast<uintptr_t>(mBuffer);
        const uintptr_t p    = reinterpret_cast<uintptr_t>(ptr);
        if (p < base || p >= base + BlockSize * BlockCount) return;
        // Alignment check
        if ((p - base) % BlockSize != 0) return;

        FreeNode* node = static_cast<FreeNode*>(ptr);
        node->next = mFreeList;
        mFreeList = node;
        if (mAllocCount > 0) --mAllocCount;
    }

    // Number of blocks currently allocated
    size_t usedBlocks()  const { return mAllocCount; }
    size_t peakBlocks()  const { return mPeakCount; }
    size_t totalBlocks() const { return BlockCount; }
    bool   isFull()      const { return mFreeList == nullptr; }
    bool   isReady()     const { return mBuffer != nullptr; }

    // Release the backing buffer. All outstanding pointers become invalid.
    void end() {
        if (mBuffer) { heap_caps_free(mBuffer); mBuffer = nullptr; }
        mFreeList   = nullptr;
        mAllocCount = 0;
        mPeakCount  = 0;
    }
};

} // namespace protogc
