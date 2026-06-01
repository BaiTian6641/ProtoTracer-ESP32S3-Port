#pragma once
// ProtoGC - cooperative memory reclamation for ESP32-S3 FreeRTOS.
//
// ESP32-S3 cannot implement malloc_trim() for arbitrary live C++ objects:
// pointers cannot be moved, and the ESP-IDF heap already coalesces adjacent
// free blocks internally. ProtoGC therefore makes reclamation explicit:
// ProtoGC-owned PSRAM and internal-SRAM segments can be coalesced and trimmed,
// scoped arenas can be reset, deferred frees can be drained from the owner task,
// and subsystems can register purge callbacks for optional caches.

#include "PoolAllocator.h"
#include "ArenaAllocator.h"
#include "HeapAllocator.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

#ifndef PROTOGC_POOL_32
#define PROTOGC_POOL_32 64
#endif
#ifndef PROTOGC_POOL_64
#define PROTOGC_POOL_64 64
#endif
#ifndef PROTOGC_POOL_128
#define PROTOGC_POOL_128 48
#endif
#ifndef PROTOGC_POOL_256
#define PROTOGC_POOL_256 32
#endif
#ifndef PROTOGC_POOL_512
#define PROTOGC_POOL_512 16
#endif
#ifndef PROTOGC_POOL_1K
#define PROTOGC_POOL_1K 8
#endif

#ifndef PROTOGC_WARN_LARGEST_BLOCK
#define PROTOGC_WARN_LARGEST_BLOCK 32768
#endif
#ifndef PROTOGC_CRITICAL_LARGEST_BLOCK
#define PROTOGC_CRITICAL_LARGEST_BLOCK 8192
#endif
#ifndef PROTOGC_MAX_ARENAS
#define PROTOGC_MAX_ARENAS 8
#endif
#ifndef PROTOGC_MAX_PURGE_CALLBACKS
#define PROTOGC_MAX_PURGE_CALLBACKS 8
#endif
#ifndef PROTOGC_DEFERRED_FREE_SLOTS
#define PROTOGC_DEFERRED_FREE_SLOTS 32
#endif
#ifndef PROTOGC_POLL_DRAIN_LIMIT
#define PROTOGC_POLL_DRAIN_LIMIT 4
#endif
#ifndef PROTOGC_SCRATCH_ARENA_BYTES
#define PROTOGC_SCRATCH_ARENA_BYTES 65536
#endif
#ifndef PROTOGC_PRINT_COLLECTIONS
#define PROTOGC_PRINT_COLLECTIONS 1
#endif
#ifndef PROTOGC_OVERRIDE_NEW
#define PROTOGC_OVERRIDE_NEW 1
#endif

namespace protogc {

enum CollectPhase : uint8_t {
    CollectLightPhase = 1,
    CollectFullPhase = 2,
    CollectEmergencyPhase = 4
};

enum class ArenaPolicy : uint8_t {
    Manual = 0,
    ResetOnLight = 1,
    ResetOnFull = 2
};

enum class DeferredFreeKind : uint8_t {
    HeapCaps = 0,
    Pool = 1
};

struct ArenaRecord {
    ArenaAllocator* arena = nullptr;
    ArenaPolicy policy = ArenaPolicy::Manual;
    const char* name = nullptr;
    bool ownsObject = false;
};

struct PurgeRecord {
    const char* name = nullptr;
    void (*callback)(uint8_t phaseMask, void* context) = nullptr;
    void* context = nullptr;
    uint8_t phases = 0;
};

struct DeferredFreeRecord {
    void* ptr = nullptr;
    DeferredFreeKind kind = DeferredFreeKind::HeapCaps;
};

class HeapGuard {
public:
    enum Level { OK, WARN, CRITICAL };
    using Callback = void (*)(Level level, size_t freeBytes, size_t largestBlock);

    static Level check(size_t* outFree = nullptr, size_t* outLargest = nullptr) {
        const size_t freeBytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        if (outFree) *outFree = freeBytes;
        if (outLargest) *outLargest = largest;

        if (largest < PROTOGC_CRITICAL_LARGEST_BLOCK) return CRITICAL;
        if (largest < PROTOGC_WARN_LARGEST_BLOCK) return WARN;
        return OK;
    }

    static void onWarning(Callback cb) { sWarnCb = cb; }
    static void onCritical(Callback cb) { sCritCb = cb; }

    static void poll() {
        size_t freeBytes = 0;
        size_t largest = 0;
        const Level level = check(&freeBytes, &largest);
        if (level == CRITICAL && sCritCb) {
            sCritCb(level, freeBytes, largest);
        } else if (level == WARN && sWarnCb) {
            sWarnCb(level, freeBytes, largest);
        }
    }

private:
    static Callback sWarnCb;
    static Callback sCritCb;
};

struct HeapStats {
    size_t internalFree = 0;
    size_t internalLargestBlock = 0;
    size_t psramFree = 0;
    size_t psramLargestBlock = 0;
    size_t arenaCapacity = 0;
    size_t arenaUsed = 0;
    size_t arenaPeak = 0;
    size_t poolReservedBytes = 0;
    size_t poolUsedBytes = 0;
    size_t poolUsedBlocks = 0;
    size_t fallbackBytes = 0;
    size_t fallbackPeakBytes = 0;
    size_t fallbackBlocks = 0;
    size_t managedSegmentBytes = 0;
    size_t managedUsedBytes = 0;
    size_t managedFreeBytes = 0;
    size_t managedLargestFreeBlock = 0;
    size_t managedSegments = 0;
    size_t psramManagedSegmentBytes = 0;
    size_t psramManagedUsedBytes = 0;
    size_t psramManagedFreeBytes = 0;
    size_t psramManagedLargestFreeBlock = 0;
    size_t psramManagedSegments = 0;
    size_t internalManagedSegmentBytes = 0;
    size_t internalManagedUsedBytes = 0;
    size_t internalManagedFreeBytes = 0;
    size_t internalManagedLargestFreeBlock = 0;
    size_t internalManagedSegments = 0;
    size_t trimReleasedBytes = 0;
    size_t psramTrimReleasedBytes = 0;
    size_t internalTrimReleasedBytes = 0;
    size_t deferredFreeCount = 0;
    size_t lastReclaimedBytes = 0;
    uint32_t lightCollections = 0;
    uint32_t fullCollections = 0;
    uint32_t emergencyCollections = 0;
    bool mallocLockedToPsram = false;

    void print(const char* tag = "ProtoGC") const {
        std::printf("[%s] intFree=%u intLargest=%u psramFree=%u psramLargest=%u "
                    "arena=%u/%u peak=%u pool=%u/%u psramManaged=%u/%u largest=%u seg=%u "
                    "intManaged=%u/%u largest=%u seg=%u fallback=%u peak=%u blocks=%u "
                    "deferred=%u reclaimed=%u trim=%u/%u "
                    "gc=%u/%u/%u lock=%u\n",
                    tag,
                    static_cast<unsigned>(internalFree),
                    static_cast<unsigned>(internalLargestBlock),
                    static_cast<unsigned>(psramFree),
                    static_cast<unsigned>(psramLargestBlock),
                    static_cast<unsigned>(arenaUsed),
                    static_cast<unsigned>(arenaCapacity),
                    static_cast<unsigned>(arenaPeak),
                    static_cast<unsigned>(poolUsedBytes),
                    static_cast<unsigned>(poolReservedBytes),
                    static_cast<unsigned>(psramManagedUsedBytes),
                    static_cast<unsigned>(psramManagedSegmentBytes),
                    static_cast<unsigned>(psramManagedLargestFreeBlock),
                    static_cast<unsigned>(psramManagedSegments),
                    static_cast<unsigned>(internalManagedUsedBytes),
                    static_cast<unsigned>(internalManagedSegmentBytes),
                    static_cast<unsigned>(internalManagedLargestFreeBlock),
                    static_cast<unsigned>(internalManagedSegments),
                    static_cast<unsigned>(fallbackBytes),
                    static_cast<unsigned>(fallbackPeakBytes),
                    static_cast<unsigned>(fallbackBlocks),
                    static_cast<unsigned>(deferredFreeCount),
                    static_cast<unsigned>(lastReclaimedBytes),
                    static_cast<unsigned>(psramTrimReleasedBytes),
                    static_cast<unsigned>(internalTrimReleasedBytes),
                    static_cast<unsigned>(lightCollections),
                    static_cast<unsigned>(fullCollections),
                    static_cast<unsigned>(emergencyCollections),
                    mallocLockedToPsram ? 1U : 0U);
    }
};

class ProtoGC {
public:
    using PurgeCallback = void (*)(uint8_t phaseMask, void* context);

    static bool begin(size_t scratchArenaBytes = PROTOGC_SCRATCH_ARENA_BYTES) {
        if (sBeginCalled) return sInitialized;
        sBeginCalled = true;
        sPsramHeap.begin(PROTOGC_PSRAM_HEAP_SEGMENT_BYTES,
                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                 HeapAllocator::defaultPsramForbiddenCaps());
        sInternalHeap.begin(PROTOGC_INTERNAL_HEAP_SEGMENT_BYTES,
                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
                    HeapAllocator::defaultInternalForbiddenCaps());

        bool poolsOk = true;
        poolsOk &= sPool32.begin();
        poolsOk &= sPool64.begin();
        poolsOk &= sPool128.begin();
        poolsOk &= sPool256.begin();
        poolsOk &= sPool512.begin();
        poolsOk &= sPool1K.begin();

        if (scratchArenaBytes > 0 && sScratchArena.begin(scratchArenaBytes)) {
            registerArena(&sScratchArena, ArenaPolicy::ResetOnLight, "scratch", false);
        }

        sInitialized = poolsOk || sPool128.isReady() || sScratchArena.isReady();
        if (!poolsOk) {
            std::printf("[ProtoGC] warning: one or more PSRAM pools failed; degraded mode\n");
        }
#if PROTOGC_OVERRIDE_NEW
        // Default policy: route global operator new through ProtoGC so PSRAM is
        // preferred for every C++ allocation, with internal SRAM as fallback.
        // Direct heap_caps_malloc DMA/EXEC paths are unaffected (those bypass new).
        sNewDeleteTakeoverEnabled = sInitialized;
#endif
        return sInitialized;
    }

    static void lockMallocToPsram(size_t mallocExternalThreshold = 0) {
        heap_caps_malloc_extmem_enable(mallocExternalThreshold);
        portENTER_CRITICAL(&sMux);
        sMallocLockedToPsram = true;
        sMallocExternalThreshold = mallocExternalThreshold;
        portEXIT_CRITICAL(&sMux);
    }

    static bool isMallocLockedToPsram() { return sMallocLockedToPsram; }

    static bool isInIsrContext() {
#if defined(ESP32)
        return xPortInIsrContext();
#else
        return false;
#endif
    }

    static void enableNewDeleteTakeover(bool enabled = true) {
        portENTER_CRITICAL(&sMux);
        sNewDeleteTakeoverEnabled = enabled;
        portEXIT_CRITICAL(&sMux);
    }

    static bool isNewDeleteTakeoverEnabled() { return sNewDeleteTakeoverEnabled; }

    static ArenaAllocator* scratchArena(size_t minCapacityBytes = 0) {
        if (minCapacityBytes > 0 && !sScratchArena.ensureCapacity(minCapacityBytes)) {
            return nullptr;
        }
        return sScratchArena.isReady() ? &sScratchArena : nullptr;
    }

    static ArenaAllocator* createArena(size_t capacityBytes,
                                       ArenaPolicy policy = ArenaPolicy::Manual,
                                       const char* name = nullptr) {
        ArenaAllocator* arena = new ArenaAllocator();
        if (!arena) return nullptr;
        if (!arena->begin(capacityBytes)) {
            delete arena;
            return nullptr;
        }

        if (!registerArena(arena, policy, name, true)) {
            sArenaRegistryOverflow = true;
        }
        return arena;
    }

    static void destroyArena(ArenaAllocator* arena) {
        if (!arena) return;

        bool ownsObject = false;
        const bool tracked = unregisterArenaInternal(arena, &ownsObject);
        arena->end();

        if ((tracked && ownsObject) || (!tracked && arena != &sScratchArena)) {
            delete arena;
        }
    }

    static bool registerArena(ArenaAllocator* arena,
                              ArenaPolicy policy = ArenaPolicy::Manual,
                              const char* name = nullptr,
                              bool ownsObject = false) {
        if (!arena) return false;

        portENTER_CRITICAL(&sMux);
        for (size_t i = 0; i < PROTOGC_MAX_ARENAS; ++i) {
            if (sArenaRecords[i].arena == arena) {
                sArenaRecords[i].policy = policy;
                sArenaRecords[i].name = name;
                sArenaRecords[i].ownsObject = ownsObject;
                portEXIT_CRITICAL(&sMux);
                return true;
            }
        }
        for (size_t i = 0; i < PROTOGC_MAX_ARENAS; ++i) {
            if (sArenaRecords[i].arena == nullptr) {
                sArenaRecords[i].arena = arena;
                sArenaRecords[i].policy = policy;
                sArenaRecords[i].name = name;
                sArenaRecords[i].ownsObject = ownsObject;
                portEXIT_CRITICAL(&sMux);
                return true;
            }
        }
        portEXIT_CRITICAL(&sMux);
        return false;
    }

    static bool unregisterArena(ArenaAllocator* arena) {
        return unregisterArenaInternal(arena, nullptr);
    }

    static void* heapAlloc(size_t sizeBytes,
                           uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) {
        if (isInIsrContext()) return nullptr;
        if (sizeBytes == 0) sizeBytes = 1;

        portENTER_CRITICAL(&sMux);
        HeapAllocator* managedHeap = managedHeapForCapsLocked(caps);
        void* managed = managedHeap ? managedHeap->allocate(sizeBytes, caps) : nullptr;
        portEXIT_CRITICAL(&sMux);
        if (managed) return managed;

        AllocationHeader* header = static_cast<AllocationHeader*>(
            heap_caps_malloc(sizeof(AllocationHeader) + sizeBytes, caps));
        if (!header) return nullptr;

        header->magic = kAllocationMagic;
        header->size = sizeBytes;
        header->caps = caps;
        header->prev = nullptr;

        portENTER_CRITICAL(&sMux);
        header->next = sFallbackHead;
        if (sFallbackHead) sFallbackHead->prev = header;
        sFallbackHead = header;
        sFallbackBlocks++;
        sFallbackBytes += sizeBytes;
        if (sFallbackBytes > sFallbackPeakBytes) sFallbackPeakBytes = sFallbackBytes;
        portEXIT_CRITICAL(&sMux);

        return static_cast<void*>(header + 1);
    }

    static bool heapFree(void* ptr) {
        if (isInIsrContext()) return deferHeapFree(ptr);
        return heapFreeBytes(ptr) != 0;
    }

    static void* heapCalloc(size_t count,
                            size_t sizeBytes,
                            uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) {
        if (count != 0 && sizeBytes > (static_cast<size_t>(-1) / count)) return nullptr;
        const size_t total = count * sizeBytes;
        void* ptr = heapAlloc(total, caps);
        if (ptr) std::memset(ptr, 0, total);
        return ptr;
    }

    static void* heapRealloc(void* ptr,
                             size_t newSize,
                             uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) {
        if (isInIsrContext()) return nullptr;
        if (!ptr) return heapAlloc(newSize, caps);
        if (newSize == 0) {
            heapFree(ptr);
            return nullptr;
        }

        portENTER_CRITICAL(&sMux);
        HeapAllocator* managedHeap = owningManagedHeapLocked(ptr);
        void* managed = managedHeap ? managedHeap->reallocate(ptr, newSize, managedHeap->requiredCaps()) : nullptr;
        portEXIT_CRITICAL(&sMux);
        if (managed) return managed;

        if (managedHeap) return nullptr;

        size_t oldSize = 0;
        uint32_t oldCaps = caps;
        bool fallbackOwned = false;
        portENTER_CRITICAL(&sMux);
        AllocationHeader* header = findFallbackHeaderLocked(ptr);
        if (header) {
            oldSize = header->size;
            oldCaps = header->caps;
            fallbackOwned = true;
        }
        portEXIT_CRITICAL(&sMux);
        if (!fallbackOwned) return nullptr;

        void* replacement = heapAlloc(newSize, oldCaps);
        if (!replacement) return nullptr;
        std::memcpy(replacement, ptr, oldSize < newSize ? oldSize : newSize);
        heapFree(ptr);
        return replacement;
    }

    static void* malloc(size_t sizeBytes) { return heapAlloc(sizeBytes); }
    static void* calloc(size_t count, size_t sizeBytes) { return heapCalloc(count, sizeBytes); }
    static void* realloc(void* ptr, size_t newSize) { return heapRealloc(ptr, newSize); }
    static void free(void* ptr) { heapFree(ptr); }

    static void* psramAlloc(size_t sizeBytes) {
        return heapAlloc(sizeBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    static void* psramCalloc(size_t count, size_t sizeBytes) {
        return heapCalloc(count, sizeBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    static void* psramRealloc(void* ptr, size_t newSize) {
        return heapRealloc(ptr, newSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    static void* internalAlloc(size_t sizeBytes) {
        return heapAlloc(sizeBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    static void* internalCalloc(size_t count, size_t sizeBytes) {
        return heapCalloc(count, sizeBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    static void* internalRealloc(void* ptr, size_t newSize) {
        return heapRealloc(ptr, newSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    static void* poolAlloc(size_t sizeBytes) {
        if (isInIsrContext()) return nullptr;
        void* ptr = nullptr;
        portENTER_CRITICAL(&sMux);
        ptr = poolAllocInternalLocked(sizeBytes);
        portEXIT_CRITICAL(&sMux);
        if (ptr) return ptr;
        return heapAlloc(sizeBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    static void poolFree(void* ptr) {
        if (!ptr) return;
        if (isInIsrContext()) {
            deferPoolFree(ptr);
            return;
        }

        bool freedByPool = false;
        portENTER_CRITICAL(&sMux);
        freedByPool = poolFreeInternalLocked(ptr);
        portEXIT_CRITICAL(&sMux);

        if (freedByPool) return;
        heapFree(ptr);
    }

    static bool poolOwns(void* ptr) {
        bool owned = false;
        portENTER_CRITICAL(&sMux);
        owned = sPool32.owns(ptr) || sPool64.owns(ptr) || sPool128.owns(ptr) ||
                sPool256.owns(ptr) || sPool512.owns(ptr) || sPool1K.owns(ptr);
        portEXIT_CRITICAL(&sMux);
        return owned;
    }

    static bool deferHeapFree(void* ptr) {
        return deferFree(ptr, DeferredFreeKind::HeapCaps);
    }

    static bool deferPoolFree(void* ptr) {
        return deferFree(ptr, DeferredFreeKind::Pool);
    }

    static size_t drainDeferredFrees(size_t maxItems = 0) {
        size_t reclaimed = 0;
        size_t processed = 0;
        DeferredFreeRecord item;

        while ((maxItems == 0 || processed < maxItems) && popDeferredFree(&item)) {
            if (item.kind == DeferredFreeKind::Pool) {
                poolFree(item.ptr);
            } else {
                reclaimed += heapFreeBytes(item.ptr);
            }
            ++processed;
        }
        return reclaimed;
    }

    static bool registerPurgeCallback(const char* name,
                                      PurgeCallback callback,
                                      void* context = nullptr,
                                      uint8_t phases = CollectFullPhase | CollectEmergencyPhase) {
        if (!callback || phases == 0) return false;

        portENTER_CRITICAL(&sMux);
        for (size_t i = 0; i < PROTOGC_MAX_PURGE_CALLBACKS; ++i) {
            if (sPurgeRecords[i].callback == callback && sPurgeRecords[i].context == context) {
                sPurgeRecords[i].name = name;
                sPurgeRecords[i].phases = phases;
                portEXIT_CRITICAL(&sMux);
                return true;
            }
        }
        for (size_t i = 0; i < PROTOGC_MAX_PURGE_CALLBACKS; ++i) {
            if (sPurgeRecords[i].callback == nullptr) {
                sPurgeRecords[i].name = name;
                sPurgeRecords[i].callback = callback;
                sPurgeRecords[i].context = context;
                sPurgeRecords[i].phases = phases;
                portEXIT_CRITICAL(&sMux);
                return true;
            }
        }
        sPurgeRegistryOverflow = true;
        portEXIT_CRITICAL(&sMux);
        return false;
    }

    static bool unregisterPurgeCallback(PurgeCallback callback, void* context = nullptr) {
        if (!callback) return false;

        portENTER_CRITICAL(&sMux);
        for (size_t i = 0; i < PROTOGC_MAX_PURGE_CALLBACKS; ++i) {
            if (sPurgeRecords[i].callback == callback && sPurgeRecords[i].context == context) {
                sPurgeRecords[i] = PurgeRecord();
                portEXIT_CRITICAL(&sMux);
                return true;
            }
        }
        portEXIT_CRITICAL(&sMux);
        return false;
    }

    static size_t mallocTrim(size_t padBytes = 0, bool trimEmptyPools = false) {
        if (isInIsrContext()) return 0;

        size_t reclaimed = drainDeferredFrees(0);
        portENTER_CRITICAL(&sMux);
        reclaimed += sPsramHeap.trim(padBytes);
        reclaimed += sInternalHeap.trim(padBytes);
        if (trimEmptyPools) {
            reclaimed += sPool32.trimIfEmpty();
            reclaimed += sPool64.trimIfEmpty();
            reclaimed += sPool128.trimIfEmpty();
            reclaimed += sPool256.trimIfEmpty();
            reclaimed += sPool512.trimIfEmpty();
            reclaimed += sPool1K.trimIfEmpty();
        }
        sLastReclaimedBytes = reclaimed;
        portEXIT_CRITICAL(&sMux);
        return reclaimed;
    }

    static void collectLight(const char* tag = nullptr) {
        const HeapStats before = stats();
        size_t reclaimed = drainDeferredFrees(0);
        reclaimed += resetTrackedArenas(CollectLightPhase);
        compactEmptyPools();
        runPurgeCallbacks(CollectLightPhase);
        reclaimed += drainDeferredFrees(0);

        portENTER_CRITICAL(&sMux);
        sLightCollections++;
        sLastReclaimedBytes = reclaimed;
        portEXIT_CRITICAL(&sMux);

        printCollection(tag, "light", before, stats(), reclaimed);
    }

    static void collectFull(const char* tag = nullptr) {
        const HeapStats before = stats();
        size_t reclaimed = drainDeferredFrees(0);
        reclaimed += resetTrackedArenas(CollectFullPhase);
        compactEmptyPools();
        runPurgeCallbacks(CollectFullPhase);
        reclaimed += drainDeferredFrees(0);
        reclaimed += mallocTrim(0, false);

        portENTER_CRITICAL(&sMux);
        sFullCollections++;
        sLastReclaimedBytes = reclaimed;
        portEXIT_CRITICAL(&sMux);

        printCollection(tag, "full", before, stats(), reclaimed);
    }

    static void emergency(const char* tag = nullptr) {
        const HeapStats before = stats();
        size_t reclaimed = drainDeferredFrees(0);
        reclaimed += resetTrackedArenas(static_cast<uint8_t>(CollectFullPhase | CollectEmergencyPhase));
        compactEmptyPools();
        runPurgeCallbacks(static_cast<uint8_t>(CollectFullPhase | CollectEmergencyPhase));
        reclaimed += drainDeferredFrees(0);
        reclaimed += mallocTrim(0, true);

        portENTER_CRITICAL(&sMux);
        sEmergencyCollections++;
        sLastReclaimedBytes = reclaimed;
        portEXIT_CRITICAL(&sMux);

        printCollection(tag, "emergency", before, stats(), reclaimed);
    }

    static void poll() {
        drainDeferredFrees(PROTOGC_POLL_DRAIN_LIMIT);
        HeapGuard::poll();
    }

    static HeapStats stats() {
        HeapStats result;
        result.internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        result.internalLargestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        result.psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        result.psramLargestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

        portENTER_CRITICAL(&sMux);
        result.arenaCapacity = arenaCapacityLocked();
        result.arenaUsed = arenaUsedLocked();
        result.arenaPeak = arenaPeakLocked();
        result.poolReservedBytes = poolReservedBytesLocked();
        result.poolUsedBytes = poolUsedBytesLocked();
        result.poolUsedBlocks = poolUsedBlocksLocked();
        const HeapAllocator::Stats psramStats = sPsramHeap.stats();
        const HeapAllocator::Stats internalStats = sInternalHeap.stats();
        result.psramManagedSegmentBytes = psramStats.segmentBytes;
        result.psramManagedUsedBytes = psramStats.usedBytes;
        result.psramManagedFreeBytes = psramStats.freeBytes;
        result.psramManagedLargestFreeBlock = psramStats.largestFreeBlock;
        result.psramManagedSegments = psramStats.segmentCount;
        result.internalManagedSegmentBytes = internalStats.segmentBytes;
        result.internalManagedUsedBytes = internalStats.usedBytes;
        result.internalManagedFreeBytes = internalStats.freeBytes;
        result.internalManagedLargestFreeBlock = internalStats.largestFreeBlock;
        result.internalManagedSegments = internalStats.segmentCount;
        result.psramTrimReleasedBytes = psramStats.trimReleasedBytes;
        result.internalTrimReleasedBytes = internalStats.trimReleasedBytes;
        result.managedSegmentBytes = psramStats.segmentBytes + internalStats.segmentBytes;
        result.managedUsedBytes = psramStats.usedBytes + internalStats.usedBytes;
        result.managedFreeBytes = psramStats.freeBytes + internalStats.freeBytes;
        result.managedLargestFreeBlock = psramStats.largestFreeBlock > internalStats.largestFreeBlock
                             ? psramStats.largestFreeBlock
                             : internalStats.largestFreeBlock;
        result.managedSegments = psramStats.segmentCount + internalStats.segmentCount;
        result.trimReleasedBytes = psramStats.trimReleasedBytes + internalStats.trimReleasedBytes;
        result.fallbackBytes = sFallbackBytes;
        result.fallbackPeakBytes = sFallbackPeakBytes;
        result.fallbackBlocks = sFallbackBlocks;
        result.deferredFreeCount = sDeferredCount;
        result.lastReclaimedBytes = sLastReclaimedBytes;
        result.lightCollections = sLightCollections;
        result.fullCollections = sFullCollections;
        result.emergencyCollections = sEmergencyCollections;
        result.mallocLockedToPsram = sMallocLockedToPsram;
        portEXIT_CRITICAL(&sMux);
        return result;
    }

    static bool isInitialized() { return sInitialized; }

private:
    struct AllocationHeader {
        uint32_t magic;
        uint32_t caps;
        size_t size;
        AllocationHeader* prev;
        AllocationHeader* next;
    };

    static constexpr uint32_t kAllocationMagic = 0x50474331UL;
    static constexpr uint32_t kAllocationFreed = 0x50474346UL;

    static bool sBeginCalled;
    static bool sInitialized;
    static bool sMallocLockedToPsram;
    static bool sNewDeleteTakeoverEnabled;
    static bool sArenaRegistryOverflow;
    static bool sPurgeRegistryOverflow;
    static size_t sMallocExternalThreshold;
    static size_t sFallbackBytes;
    static size_t sFallbackPeakBytes;
    static size_t sFallbackBlocks;
    static AllocationHeader* sFallbackHead;
    static size_t sLastReclaimedBytes;
    static uint32_t sLightCollections;
    static uint32_t sFullCollections;
    static uint32_t sEmergencyCollections;
    static portMUX_TYPE sMux;

    static ArenaRecord sArenaRecords[PROTOGC_MAX_ARENAS];
    static PurgeRecord sPurgeRecords[PROTOGC_MAX_PURGE_CALLBACKS];
    static DeferredFreeRecord sDeferredFrees[PROTOGC_DEFERRED_FREE_SLOTS];
    static size_t sDeferredHead;
    static size_t sDeferredTail;
    static size_t sDeferredCount;

    static ArenaAllocator sScratchArena;
    static HeapAllocator sPsramHeap;
    static HeapAllocator sInternalHeap;
    static PoolAllocator<32, PROTOGC_POOL_32> sPool32;
    static PoolAllocator<64, PROTOGC_POOL_64> sPool64;
    static PoolAllocator<128, PROTOGC_POOL_128> sPool128;
    static PoolAllocator<256, PROTOGC_POOL_256> sPool256;
    static PoolAllocator<512, PROTOGC_POOL_512> sPool512;
    static PoolAllocator<1024, PROTOGC_POOL_1K> sPool1K;

    static HeapAllocator* managedHeapForCapsLocked(uint32_t caps) {
        if ((caps & MALLOC_CAP_DMA) != 0) return nullptr;
#ifdef MALLOC_CAP_EXEC
        if ((caps & MALLOC_CAP_EXEC) != 0) return nullptr;
#endif
        const bool wantsPsram = (caps & MALLOC_CAP_SPIRAM) != 0;
        const bool wantsInternal = (caps & MALLOC_CAP_INTERNAL) != 0;
        if (wantsPsram == wantsInternal) return nullptr;
        return wantsPsram ? &sPsramHeap : &sInternalHeap;
    }

    static HeapAllocator* owningManagedHeapLocked(void* ptr) {
        if (!ptr) return nullptr;
        if (sPsramHeap.owns(ptr)) return &sPsramHeap;
        if (sInternalHeap.owns(ptr)) return &sInternalHeap;
        return nullptr;
    }

    static bool unregisterArenaInternal(ArenaAllocator* arena, bool* outOwnsObject) {
        if (outOwnsObject) *outOwnsObject = false;
        if (!arena) return false;

        portENTER_CRITICAL(&sMux);
        for (size_t i = 0; i < PROTOGC_MAX_ARENAS; ++i) {
            if (sArenaRecords[i].arena == arena) {
                if (outOwnsObject) *outOwnsObject = sArenaRecords[i].ownsObject;
                sArenaRecords[i] = ArenaRecord();
                portEXIT_CRITICAL(&sMux);
                return true;
            }
        }
        portEXIT_CRITICAL(&sMux);
        return false;
    }

    static void* poolAllocInternalLocked(size_t sizeBytes) {
        if (sizeBytes <= 32) { void* p = sPool32.alloc(); if (p) return p; }
        if (sizeBytes <= 64) { void* p = sPool64.alloc(); if (p) return p; }
        if (sizeBytes <= 128) { void* p = sPool128.alloc(); if (p) return p; }
        if (sizeBytes <= 256) { void* p = sPool256.alloc(); if (p) return p; }
        if (sizeBytes <= 512) { void* p = sPool512.alloc(); if (p) return p; }
        if (sizeBytes <= 1024) { void* p = sPool1K.alloc(); if (p) return p; }
        return nullptr;
    }

    static bool poolFreeInternalLocked(void* ptr) {
        if (sPool32.free(ptr)) return true;
        if (sPool64.free(ptr)) return true;
        if (sPool128.free(ptr)) return true;
        if (sPool256.free(ptr)) return true;
        if (sPool512.free(ptr)) return true;
        if (sPool1K.free(ptr)) return true;
        return false;
    }

    static void compactEmptyPools() {
        portENTER_CRITICAL(&sMux);
        sPool32.resetIfEmpty();
        sPool64.resetIfEmpty();
        sPool128.resetIfEmpty();
        sPool256.resetIfEmpty();
        sPool512.resetIfEmpty();
        sPool1K.resetIfEmpty();
        portEXIT_CRITICAL(&sMux);
    }

    static AllocationHeader* findFallbackHeaderLocked(void* ptr) {
        AllocationHeader* header = sFallbackHead;
        while (header && static_cast<void*>(header + 1) != ptr) {
            header = header->next;
        }
        return (header && header->magic == kAllocationMagic) ? header : nullptr;
    }

    static size_t heapFreeBytes(void* ptr) {
        if (!ptr) return 0;

        portENTER_CRITICAL(&sMux);
        HeapAllocator* managedHeap = owningManagedHeapLocked(ptr);
        const size_t managedSize = managedHeap ? managedHeap->usableSize(ptr) : 0;
        if (managedHeap && managedHeap->deallocate(ptr)) {
            portEXIT_CRITICAL(&sMux);
            return managedSize;
        }
        portEXIT_CRITICAL(&sMux);

        portENTER_CRITICAL(&sMux);
        AllocationHeader* header = findFallbackHeaderLocked(ptr);
        if (!header) {
            portEXIT_CRITICAL(&sMux);
            return 0;
        }

        const size_t size = header->size;
        if (header->prev) header->prev->next = header->next;
        else sFallbackHead = header->next;
        if (header->next) header->next->prev = header->prev;
        if (sFallbackBytes >= size) sFallbackBytes -= size;
        else sFallbackBytes = 0;
        if (sFallbackBlocks > 0) --sFallbackBlocks;
        portEXIT_CRITICAL(&sMux);

        header->magic = kAllocationFreed;
        header->prev = nullptr;
        header->next = nullptr;

        heap_caps_free(header);
        return size;
    }

    static bool deferFree(void* ptr, DeferredFreeKind kind) {
        if (!ptr) return true;

        const bool fromIsr = isInIsrContext();
        if (fromIsr) portENTER_CRITICAL_ISR(&sMux);
        else portENTER_CRITICAL(&sMux);
        if (sDeferredCount >= PROTOGC_DEFERRED_FREE_SLOTS) {
            if (fromIsr) portEXIT_CRITICAL_ISR(&sMux);
            else portEXIT_CRITICAL(&sMux);
            return false;
        }
        sDeferredFrees[sDeferredHead].ptr = ptr;
        sDeferredFrees[sDeferredHead].kind = kind;
        sDeferredHead = (sDeferredHead + 1) % PROTOGC_DEFERRED_FREE_SLOTS;
        ++sDeferredCount;
        if (fromIsr) portEXIT_CRITICAL_ISR(&sMux);
        else portEXIT_CRITICAL(&sMux);
        return true;
    }

    static bool popDeferredFree(DeferredFreeRecord* out) {
        if (!out) return false;

        portENTER_CRITICAL(&sMux);
        if (sDeferredCount == 0) {
            portEXIT_CRITICAL(&sMux);
            return false;
        }
        *out = sDeferredFrees[sDeferredTail];
        sDeferredFrees[sDeferredTail] = DeferredFreeRecord();
        sDeferredTail = (sDeferredTail + 1) % PROTOGC_DEFERRED_FREE_SLOTS;
        --sDeferredCount;
        portEXIT_CRITICAL(&sMux);
        return true;
    }

    static size_t resetTrackedArenas(uint8_t phaseMask) {
        ArenaAllocator* arenas[PROTOGC_MAX_ARENAS];
        size_t count = 0;

        portENTER_CRITICAL(&sMux);
        for (size_t i = 0; i < PROTOGC_MAX_ARENAS; ++i) {
            ArenaAllocator* arena = sArenaRecords[i].arena;
            if (!arena) continue;

            const ArenaPolicy policy = sArenaRecords[i].policy;
            const bool resetOnLight = policy == ArenaPolicy::ResetOnLight && (phaseMask & CollectLightPhase);
            const bool resetOnFull = (policy == ArenaPolicy::ResetOnLight || policy == ArenaPolicy::ResetOnFull) &&
                                     (phaseMask & (CollectFullPhase | CollectEmergencyPhase));
            if ((resetOnLight || resetOnFull) && count < PROTOGC_MAX_ARENAS) {
                arenas[count++] = arena;
            }
        }
        portEXIT_CRITICAL(&sMux);

        size_t reclaimed = 0;
        for (size_t i = 0; i < count; ++i) {
            reclaimed += arenas[i]->reset();
        }
        return reclaimed;
    }

    static void runPurgeCallbacks(uint8_t phaseMask) {
        PurgeRecord callbacks[PROTOGC_MAX_PURGE_CALLBACKS];
        size_t count = 0;

        portENTER_CRITICAL(&sMux);
        for (size_t i = 0; i < PROTOGC_MAX_PURGE_CALLBACKS; ++i) {
            if (sPurgeRecords[i].callback && (sPurgeRecords[i].phases & phaseMask)) {
                callbacks[count++] = sPurgeRecords[i];
            }
        }
        portEXIT_CRITICAL(&sMux);

        for (size_t i = 0; i < count; ++i) {
            callbacks[i].callback(phaseMask, callbacks[i].context);
        }
    }

    static size_t arenaCapacityLocked() {
        size_t total = 0;
        for (size_t i = 0; i < PROTOGC_MAX_ARENAS; ++i) {
            if (sArenaRecords[i].arena) total += sArenaRecords[i].arena->capacity();
        }
        return total;
    }

    static size_t arenaUsedLocked() {
        size_t total = 0;
        for (size_t i = 0; i < PROTOGC_MAX_ARENAS; ++i) {
            if (sArenaRecords[i].arena) total += sArenaRecords[i].arena->used();
        }
        return total;
    }

    static size_t arenaPeakLocked() {
        size_t total = 0;
        for (size_t i = 0; i < PROTOGC_MAX_ARENAS; ++i) {
            if (sArenaRecords[i].arena) total += sArenaRecords[i].arena->peakUsed();
        }
        return total;
    }

    static size_t poolReservedBytesLocked() {
        return sPool32.reservedBytes() + sPool64.reservedBytes() + sPool128.reservedBytes() +
               sPool256.reservedBytes() + sPool512.reservedBytes() + sPool1K.reservedBytes();
    }

    static size_t poolUsedBytesLocked() {
        return sPool32.usedBytes() + sPool64.usedBytes() + sPool128.usedBytes() +
               sPool256.usedBytes() + sPool512.usedBytes() + sPool1K.usedBytes();
    }

    static size_t poolUsedBlocksLocked() {
        return sPool32.usedBlocks() + sPool64.usedBlocks() + sPool128.usedBlocks() +
               sPool256.usedBlocks() + sPool512.usedBlocks() + sPool1K.usedBlocks();
    }

    static void printCollection(const char* tag,
                                const char* phaseName,
                                const HeapStats& before,
                                const HeapStats& after,
                                size_t reclaimed) {
#if PROTOGC_PRINT_COLLECTIONS
        if (!tag) return;
        std::printf("[ProtoGC:%s] %s reclaimed=%u intLargest %u->%u psramFree %u->%u deferred=%u\n",
                    tag,
                    phaseName,
                    static_cast<unsigned>(reclaimed),
                    static_cast<unsigned>(before.internalLargestBlock),
                    static_cast<unsigned>(after.internalLargestBlock),
                    static_cast<unsigned>(before.psramFree),
                    static_cast<unsigned>(after.psramFree),
                    static_cast<unsigned>(after.deferredFreeCount));
#else
        (void)tag;
        (void)phaseName;
        (void)before;
        (void)after;
        (void)reclaimed;
#endif
    }
};

class ScopedArena {
public:
    explicit ScopedArena(size_t capacityBytes, const char* name = "scoped")
        : mArena(ProtoGC::createArena(capacityBytes, ArenaPolicy::Manual, name)) {}

    ~ScopedArena() { ProtoGC::destroyArena(mArena); }

    ScopedArena(const ScopedArena&) = delete;
    ScopedArena& operator=(const ScopedArena&) = delete;

    ArenaAllocator* get() const { return mArena; }
    bool ok() const { return mArena != nullptr && mArena->isReady(); }

    void* alloc(size_t sizeBytes, size_t alignment = 4) {
        return mArena ? mArena->alloc(sizeBytes, alignment) : nullptr;
    }

    size_t reset() { return mArena ? mArena->reset() : 0; }

private:
    ArenaAllocator* mArena;
};

struct ProtoJsonPsramAllocator {
    void* allocate(size_t size) {
        return ProtoGC::heapAlloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    void* reallocate(void* ptr, size_t newSize) {
        return ProtoGC::heapRealloc(ptr, newSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    void deallocate(void* ptr) {
        ProtoGC::heapFree(ptr);
    }
};

struct ProtoJsonInternalAllocator {
    void* allocate(size_t size) {
        return ProtoGC::heapAlloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    void* reallocate(void* ptr, size_t newSize) {
        return ProtoGC::heapRealloc(ptr, newSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    void deallocate(void* ptr) {
        ProtoGC::heapFree(ptr);
    }
};

inline HeapGuard::Callback HeapGuard::sWarnCb = nullptr;
inline HeapGuard::Callback HeapGuard::sCritCb = nullptr;

inline bool ProtoGC::sBeginCalled = false;
inline bool ProtoGC::sInitialized = false;
inline bool ProtoGC::sMallocLockedToPsram = false;
inline bool ProtoGC::sNewDeleteTakeoverEnabled = false;
inline bool ProtoGC::sArenaRegistryOverflow = false;
inline bool ProtoGC::sPurgeRegistryOverflow = false;
inline size_t ProtoGC::sMallocExternalThreshold = 64;
inline size_t ProtoGC::sFallbackBytes = 0;
inline size_t ProtoGC::sFallbackPeakBytes = 0;
inline size_t ProtoGC::sFallbackBlocks = 0;
inline ProtoGC::AllocationHeader* ProtoGC::sFallbackHead = nullptr;
inline size_t ProtoGC::sLastReclaimedBytes = 0;
inline uint32_t ProtoGC::sLightCollections = 0;
inline uint32_t ProtoGC::sFullCollections = 0;
inline uint32_t ProtoGC::sEmergencyCollections = 0;
inline portMUX_TYPE ProtoGC::sMux = portMUX_INITIALIZER_UNLOCKED;

inline ArenaRecord ProtoGC::sArenaRecords[PROTOGC_MAX_ARENAS] = {};
inline PurgeRecord ProtoGC::sPurgeRecords[PROTOGC_MAX_PURGE_CALLBACKS] = {};
inline DeferredFreeRecord ProtoGC::sDeferredFrees[PROTOGC_DEFERRED_FREE_SLOTS] = {};
inline size_t ProtoGC::sDeferredHead = 0;
inline size_t ProtoGC::sDeferredTail = 0;
inline size_t ProtoGC::sDeferredCount = 0;

inline ArenaAllocator ProtoGC::sScratchArena;
inline HeapAllocator ProtoGC::sPsramHeap;
inline HeapAllocator ProtoGC::sInternalHeap;
inline PoolAllocator<32, PROTOGC_POOL_32> ProtoGC::sPool32;
inline PoolAllocator<64, PROTOGC_POOL_64> ProtoGC::sPool64;
inline PoolAllocator<128, PROTOGC_POOL_128> ProtoGC::sPool128;
inline PoolAllocator<256, PROTOGC_POOL_256> ProtoGC::sPool256;
inline PoolAllocator<512, PROTOGC_POOL_512> ProtoGC::sPool512;
inline PoolAllocator<1024, PROTOGC_POOL_1K> ProtoGC::sPool1K;

} // namespace protogc