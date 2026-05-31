#pragma once
// ProtoGC HeapAllocator - segmented coalescing heap for capability-tagged app data.
//
// This allocator owns one or more ESP-IDF heap_caps_malloc() segments. Each
// segment is internally split into blocks, freed blocks are coalesced, and
// trim() returns fully-free segments to the system heap. It does not move live
// allocations, so all returned pointers remain stable until freed.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <esp_heap_caps.h>

namespace protogc {

#ifndef PROTOGC_HEAP_SEGMENT_BYTES
#define PROTOGC_HEAP_SEGMENT_BYTES 65536
#endif

#ifndef PROTOGC_PSRAM_HEAP_SEGMENT_BYTES
#define PROTOGC_PSRAM_HEAP_SEGMENT_BYTES PROTOGC_HEAP_SEGMENT_BYTES
#endif

#ifndef PROTOGC_INTERNAL_HEAP_SEGMENT_BYTES
#define PROTOGC_INTERNAL_HEAP_SEGMENT_BYTES 8192
#endif

#ifndef PROTOGC_HEAP_MIN_SPLIT_BYTES
#define PROTOGC_HEAP_MIN_SPLIT_BYTES 32
#endif

class HeapAllocator {
public:
    struct Stats {
        size_t segmentCount = 0;
        size_t segmentBytes = 0;
        size_t usedBytes = 0;
        size_t freeBytes = 0;
        size_t largestFreeBlock = 0;
        size_t allocationCount = 0;
        size_t peakUsedBytes = 0;
        size_t trimReleasedBytes = 0;
    };

    HeapAllocator() = default;

    void begin(size_t defaultSegmentBytes = PROTOGC_HEAP_SEGMENT_BYTES) {
        begin(defaultSegmentBytes,
              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
              defaultPsramForbiddenCaps());
    }

    void begin(size_t defaultSegmentBytes, uint32_t requiredCaps, uint32_t forbiddenCaps) {
        mDefaultSegmentBytes = alignUp(defaultSegmentBytes > minimumSegmentBytes()
                                           ? defaultSegmentBytes
                                           : minimumSegmentBytes());
        mRequiredCaps = requiredCaps;
        mForbiddenCaps = forbiddenCaps;
    }

    void* allocate(size_t sizeBytes, uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) {
        if (!isEligibleCaps(caps)) return nullptr;
        if (sizeBytes == 0) sizeBytes = 1;
        sizeBytes = alignUp(sizeBytes);

        BlockHeader* block = findFreeBlock(sizeBytes, caps);
        if (!block) {
            SegmentHeader* segment = createSegment(sizeBytes, caps);
            if (!segment) return nullptr;
            linkSegment(segment);
            block = segment->first;
        }

        splitBlock(block, sizeBytes);
        block->free = false;
        mUsedBytes += block->size;
        if (mFreeBytes >= block->size) mFreeBytes -= block->size;
        else mFreeBytes = 0;
        ++mAllocationCount;
        if (mUsedBytes > mPeakUsedBytes) mPeakUsedBytes = mUsedBytes;
        return payload(block);
    }

    void* reallocate(void* ptr,
                     size_t newSize,
                     uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) {
        if (!ptr) return allocate(newSize, caps);
        if (newSize == 0) {
            deallocate(ptr);
            return nullptr;
        }

        newSize = alignUp(newSize);
        BlockHeader* block = findBlockByPayload(ptr);
        if (!block) return nullptr;

        if (block->size >= newSize) {
            shrinkBlock(block, newSize);
            return ptr;
        }

        const size_t oldSize = block->size;
        void* replacement = allocate(newSize, caps);
        if (!replacement) return nullptr;
        std::memcpy(replacement, ptr, oldSize < newSize ? oldSize : newSize);
        deallocate(ptr);
        return replacement;
    }

    bool deallocate(void* ptr) {
        if (!ptr) return true;

        BlockHeader* block = findBlockByPayload(ptr);
        if (!block || block->free) return false;

        block->free = true;
        if (mUsedBytes >= block->size) mUsedBytes -= block->size;
        else mUsedBytes = 0;
        mFreeBytes += block->size;
        if (mAllocationCount > 0) --mAllocationCount;

        coalesce(block);
        return true;
    }

    bool owns(void* ptr) const {
        return findBlockByPayload(ptr) != nullptr;
    }

    size_t usableSize(void* ptr) const {
        BlockHeader* block = findBlockByPayload(ptr);
        return block ? block->size : 0;
    }

    size_t trim(size_t padBytes = 0) {
        size_t released = 0;
        size_t releasableFree = freeBytes();
        SegmentHeader* segment = mHead;

        while (segment) {
            SegmentHeader* next = segment->next;
            if (segmentIsCompletelyFree(segment) && releasableFree > padBytes) {
                const size_t payloadBytes = segment->first ? segment->first->size : 0;
                if (releasableFree >= payloadBytes && releasableFree - payloadBytes >= padBytes) {
                    unlinkSegment(segment);
                    released += segment->totalBytes;
                    releasableFree -= payloadBytes;
                    heap_caps_free(segment);
                }
            }
            segment = next;
        }

        mTrimReleasedBytes += released;
        return released;
    }

    Stats stats() const {
        Stats result;
        result.segmentCount = mSegmentCount;
        result.segmentBytes = mSegmentBytes;
        result.usedBytes = mUsedBytes;
        result.freeBytes = freeBytes();
        result.largestFreeBlock = largestFreeBlock();
        result.allocationCount = mAllocationCount;
        result.peakUsedBytes = mPeakUsedBytes;
        result.trimReleasedBytes = mTrimReleasedBytes;
        return result;
    }

        uint32_t requiredCaps() const { return mRequiredCaps; }

        static constexpr uint32_t defaultPsramForbiddenCaps() {
        uint32_t caps = 0;
    #ifdef MALLOC_CAP_INTERNAL
        caps |= MALLOC_CAP_INTERNAL;
    #endif
    #ifdef MALLOC_CAP_DMA
        caps |= MALLOC_CAP_DMA;
    #endif
    #ifdef MALLOC_CAP_EXEC
        caps |= MALLOC_CAP_EXEC;
    #endif
        return caps;
        }

        static constexpr uint32_t defaultInternalForbiddenCaps() {
        uint32_t caps = 0;
    #ifdef MALLOC_CAP_SPIRAM
        caps |= MALLOC_CAP_SPIRAM;
    #endif
    #ifdef MALLOC_CAP_DMA
        caps |= MALLOC_CAP_DMA;
    #endif
    #ifdef MALLOC_CAP_EXEC
        caps |= MALLOC_CAP_EXEC;
    #endif
        return caps;
        }

private:
    struct SegmentHeader;

    struct BlockHeader {
        uint32_t magic;
        size_t size;
        bool free;
        SegmentHeader* segment;
        BlockHeader* prev;
        BlockHeader* next;
    };

    struct SegmentHeader {
        uint32_t magic;
        size_t totalBytes;
        uint32_t caps;
        SegmentHeader* prev;
        SegmentHeader* next;
        BlockHeader* first;
    };

    static constexpr uint32_t kSegmentMagic = 0x50475347UL;
    static constexpr uint32_t kBlockMagic = 0x5047424CUL;
    static constexpr size_t kAlignment = sizeof(void*) < 8 ? 8 : sizeof(void*);

    SegmentHeader* mHead = nullptr;
    size_t mDefaultSegmentBytes = PROTOGC_HEAP_SEGMENT_BYTES;
    size_t mSegmentCount = 0;
    size_t mSegmentBytes = 0;
    size_t mUsedBytes = 0;
    size_t mFreeBytes = 0;
    size_t mAllocationCount = 0;
    size_t mPeakUsedBytes = 0;
    size_t mTrimReleasedBytes = 0;
    uint32_t mRequiredCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    uint32_t mForbiddenCaps = defaultPsramForbiddenCaps();

    static size_t alignUp(size_t value, size_t alignment = kAlignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    static size_t segmentHeaderBytes() { return alignUp(sizeof(SegmentHeader)); }
    static size_t blockHeaderBytes() { return alignUp(sizeof(BlockHeader)); }
    static size_t minimumSegmentBytes() {
        return segmentHeaderBytes() + blockHeaderBytes() + PROTOGC_HEAP_MIN_SPLIT_BYTES;
    }

        bool isEligibleCaps(uint32_t caps) const {
        if ((caps & mRequiredCaps) != mRequiredCaps) return false;
        if ((caps & mForbiddenCaps) != 0) return false;
        return true;
    }

        static bool segmentSatisfiesCaps(const SegmentHeader* segment, uint32_t requestedCaps) {
        return segment && ((segment->caps & requestedCaps) == requestedCaps);
        }

    static void* payload(BlockHeader* block) {
        return reinterpret_cast<uint8_t*>(block) + blockHeaderBytes();
    }

    static const void* payload(const BlockHeader* block) {
        return reinterpret_cast<const uint8_t*>(block) + blockHeaderBytes();
    }

    SegmentHeader* createSegment(size_t requestedPayloadBytes, uint32_t caps) {
        size_t payloadBytes = alignUp(requestedPayloadBytes > mDefaultSegmentBytes
                                          ? requestedPayloadBytes
                                          : mDefaultSegmentBytes);
        size_t totalBytes = segmentHeaderBytes() + blockHeaderBytes() + payloadBytes;
        void* raw = heap_caps_malloc(totalBytes, caps);
        if (!raw && payloadBytes > requestedPayloadBytes) {
            payloadBytes = alignUp(requestedPayloadBytes);
            totalBytes = segmentHeaderBytes() + blockHeaderBytes() + payloadBytes;
            raw = heap_caps_malloc(totalBytes, caps);
        }
        if (!raw) return nullptr;

        SegmentHeader* segment = static_cast<SegmentHeader*>(raw);
        segment->magic = kSegmentMagic;
        segment->totalBytes = totalBytes;
        segment->caps = caps;
        segment->prev = nullptr;
        segment->next = nullptr;

        BlockHeader* block = reinterpret_cast<BlockHeader*>(
            reinterpret_cast<uint8_t*>(segment) + segmentHeaderBytes());
        block->magic = kBlockMagic;
        block->size = payloadBytes;
        block->free = true;
        block->segment = segment;
        block->prev = nullptr;
        block->next = nullptr;
        segment->first = block;
        return segment;
    }

    void linkSegment(SegmentHeader* segment) {
        segment->next = mHead;
        if (mHead) mHead->prev = segment;
        mHead = segment;
        ++mSegmentCount;
        mSegmentBytes += segment->totalBytes;
        mFreeBytes += segment->first ? segment->first->size : 0;
    }

    void unlinkSegment(SegmentHeader* segment) {
        if (segment->prev) segment->prev->next = segment->next;
        else mHead = segment->next;
        if (segment->next) segment->next->prev = segment->prev;
        if (mSegmentCount > 0) --mSegmentCount;
        if (mSegmentBytes >= segment->totalBytes) mSegmentBytes -= segment->totalBytes;
        if (segment->first && mFreeBytes >= segment->first->size) mFreeBytes -= segment->first->size;
    }

    BlockHeader* findFreeBlock(size_t sizeBytes, uint32_t caps) const {
        SegmentHeader* segment = mHead;
        while (segment) {
            if (segmentSatisfiesCaps(segment, caps)) {
                BlockHeader* block = segment->first;
                BlockHeader* best = nullptr;
                while (block) {
                    if (block->free && block->size >= sizeBytes &&
                        (!best || block->size < best->size)) {
                        best = block;
                    }
                    block = block->next;
                }
                if (best) return best;
            }
            segment = segment->next;
        }
        return nullptr;
    }

    BlockHeader* findBlockByPayload(const void* ptr) const {
        if (!ptr) return nullptr;
        SegmentHeader* segment = mHead;
        while (segment) {
            const uintptr_t begin = reinterpret_cast<uintptr_t>(segment);
            const uintptr_t end = begin + segment->totalBytes;
            const uintptr_t value = reinterpret_cast<uintptr_t>(ptr);
            if (value > begin && value < end) {
                BlockHeader* block = segment->first;
                while (block) {
                    if (block->magic == kBlockMagic && payload(block) == ptr) return block;
                    block = block->next;
                }
                return nullptr;
            }
            segment = segment->next;
        }
        return nullptr;
    }

    void splitBlock(BlockHeader* block, size_t wantedBytes) {
        if (!block || !block->free) return;
        if (block->size < wantedBytes + blockHeaderBytes() + PROTOGC_HEAP_MIN_SPLIT_BYTES) return;

        const size_t originalSize = block->size;
        uint8_t* nextAddress = static_cast<uint8_t*>(payload(block)) + wantedBytes;
        BlockHeader* next = reinterpret_cast<BlockHeader*>(nextAddress);
        next->magic = kBlockMagic;
        next->size = originalSize - wantedBytes - blockHeaderBytes();
        next->free = true;
        next->segment = block->segment;
        next->prev = block;
        next->next = block->next;
        if (block->next) block->next->prev = next;
        block->next = next;
        block->size = wantedBytes;

        mFreeBytes -= blockHeaderBytes();
    }

    void shrinkBlock(BlockHeader* block, size_t wantedBytes) {
        if (!block || block->free || block->size <= wantedBytes) return;
        const size_t oldSize = block->size;
        if (oldSize < wantedBytes + blockHeaderBytes() + PROTOGC_HEAP_MIN_SPLIT_BYTES) return;

        const size_t freeSize = oldSize - wantedBytes - blockHeaderBytes();
        uint8_t* nextAddress = static_cast<uint8_t*>(payload(block)) + wantedBytes;
        BlockHeader* next = reinterpret_cast<BlockHeader*>(nextAddress);
        next->magic = kBlockMagic;
        next->size = freeSize;
        next->free = true;
        next->segment = block->segment;
        next->prev = block;
        next->next = block->next;
        if (block->next) block->next->prev = next;
        block->next = next;
        block->size = wantedBytes;

        mUsedBytes -= oldSize - wantedBytes;
        mFreeBytes += freeSize;
        coalesce(next);
    }

    void unlinkNextBlock(BlockHeader* block) {
        BlockHeader* next = block->next;
        if (!block || !next) return;
        block->next = next->next;
        if (next->next) next->next->prev = block;
    }

    void coalesce(BlockHeader* block) {
        if (!block || !block->free) return;

        if (block->next && block->next->free) {
            BlockHeader* next = block->next;
            block->size += blockHeaderBytes() + next->size;
            unlinkNextBlock(block);
            mFreeBytes += blockHeaderBytes();
        }

        if (block->prev && block->prev->free) {
            BlockHeader* prev = block->prev;
            prev->size += blockHeaderBytes() + block->size;
            unlinkNextBlock(prev);
            mFreeBytes += blockHeaderBytes();
        }
    }

    bool segmentIsCompletelyFree(const SegmentHeader* segment) const {
        return segment && segment->magic == kSegmentMagic && segment->first &&
               segment->first->free && segment->first->prev == nullptr &&
               segment->first->next == nullptr;
    }

    size_t freeBytes() const {
        return mFreeBytes;
    }

    size_t largestFreeBlock() const {
        size_t largest = 0;
        SegmentHeader* segment = mHead;
        while (segment) {
            BlockHeader* block = segment->first;
            while (block) {
                if (block->free && block->size > largest) largest = block->size;
                block = block->next;
            }
            segment = segment->next;
        }
        return largest;
    }
};

} // namespace protogc