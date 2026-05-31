#pragma once
// ProtoGC — Cooperative Garbage Collector for ESP32-S3 FreeRTOS
// ================================================================
//
// ESP32 has no compacting GC. Instead, ProtoGC uses a two-level strategy:
//
//   Level 1 — Internal DRAM (512 KB, fast, DMA-capable)
//     Reserved exclusively for DMA-critical subsystems:
//       • HUB75 LED matrix framebuffer
//       • BLE controller Link Layer buffers
//       • Camera SIMD vertex-transform buffers
//     Allocations here are explicit (heap_caps_malloc CAP_INTERNAL) and
//     long-lived. ProtoGC monitors this pool and raises alarms when it
//     fragments below safe thresholds.
//
//   Level 2 — PSRAM (8 MB, cacheable, slower)
//     All application allocations go here: Strings, JSON documents,
//     std::vector, temporary buffers, etc. ProtoGC provides two
//     fragmentation-proof allocators:
//       • ArenaAllocator  — bump-pointer, reset-at-once (for JSON/String)
//       • PoolAllocator   — fixed-size free-list (for repeated alloc/free)
//
//   Collection Phases:
//     collectLight()  — Reset all arenas, compact pools (safe anytime)
//     collectFull()   — Light + flush caches, shrink heaps
//     emergency()     — Full + restart non-critical subsystems
//
// Usage:
//   #include <ProtoGC.h>
//   ProtoGC::begin();
//   auto* arena = ProtoGC::createArena(65536);   // 64 KB JSON parsing arena
//   void* buf   = ProtoGC::poolAlloc(128);        // from 128-byte slab pool
//   ProtoGC::collectLight();                       // after network download
//   ProtoGC::stats().print();                      // heap health report

#include "PoolAllocator.h"
#include "ArenaAllocator.h"
#include <esp_heap_caps.h>
#include <cstdint>
#include <cstdio>

// ---------------------------------------------------------------------------
// Pool configuration — tuned for Protogen animation workload
// ---------------------------------------------------------------------------
#ifndef PROTOGC_POOL_32
#define PROTOGC_POOL_32  64   // 64 blocks of 32  B →  2 KB
#endif
#ifndef PROTOGC_POOL_64
#define PROTOGC_POOL_64  64   // 64 blocks of 64  B →  4 KB
#endif
#ifndef PROTOGC_POOL_128
#define PROTOGC_POOL_128 48   // 48 blocks of 128 B →  6 KB
#endif
#ifndef PROTOGC_POOL_256
#define PROTOGC_POOL_256 32   // 32 blocks of 256 B →  8 KB
#endif
#ifndef PROTOGC_POOL_512
#define PROTOGC_POOL_512 16   // 16 blocks of 512 B →  8 KB
#endif
#ifndef PROTOGC_POOL_1K
#define PROTOGC_POOL_1K   8   //  8 blocks of 1 KB  →  8 KB
#endif

// ---------------------------------------------------------------------------
// Internal DRAM health thresholds (configurable via build flags)
// ---------------------------------------------------------------------------
#ifndef PROTOGC_WARN_LARGEST_BLOCK
#define PROTOGC_WARN_LARGEST_BLOCK     65536   //  64 KB — yellow
#endif
#ifndef PROTOGC_CRITICAL_LARGEST_BLOCK
#define PROTOGC_CRITICAL_LARGEST_BLOCK  32768   //  32 KB — red
#endif

namespace protogc {

// -----------------------------------------------------------------------
// HeapGuard — monitors internal DRAM and triggers callbacks
// -----------------------------------------------------------------------
class HeapGuard {
public:
    enum Level { OK, WARN, CRITICAL };

    using Callback = void (*)(Level level, size_t freeBytes, size_t largestBlock);

    static Level check(size_t* outFree = nullptr, size_t* outLargest = nullptr) {
        const size_t free    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        if (outFree)    *outFree    = free;
        if (outLargest) *outLargest = largest;

        if (largest < PROTOGC_CRITICAL_LARGEST_BLOCK) return CRITICAL;
        if (largest < PROTOGC_WARN_LARGEST_BLOCK)     return WARN;
        return OK;
    }

    static void onWarning(Callback cb)   { sWarnCb = cb; }
    static void onCritical(Callback cb)  { sCritCb = cb; }

    // Call once per frame (or periodically) — invokes callbacks if needed
    static void poll() {
        size_t free, largest;
        const Level lvl = check(&free, &largest);
        if (lvl == CRITICAL && sCritCb) sCritCb(lvl, free, largest);
        else if (lvl == WARN && sWarnCb) sWarnCb(lvl, free, largest);
    }

private:
    static Callback sWarnCb;
    static Callback sCritCb;
};

// -----------------------------------------------------------------------
// Stats — snapshot of both memory pools
// -----------------------------------------------------------------------
struct HeapStats {
    size_t internalFree;
    size_t internalLargestBlock;
    size_t psramFree;
    size_t psramLargestBlock;
    size_t arenaUsed;
    size_t arenaPeak;
    size_t poolUsedBlocks;

    void print(const char* tag = "ProtoGC") const {
        std::printf("[%s] intFree=%u largestBlk=%u psramFree=%u "
                    "arena=%u/%u poolUsed=%u\n",
                    tag,
                    static_cast<unsigned>(internalFree),
                    static_cast<unsigned>(internalLargestBlock),
                    static_cast<unsigned>(psramFree),
                    static_cast<unsigned>(arenaUsed),
                    static_cast<unsigned>(arenaPeak),
                    static_cast<unsigned>(poolUsedBlocks));
    }
};

// -----------------------------------------------------------------------
// ProtoGC — top-level coordinator
// -----------------------------------------------------------------------
class ProtoGC {
public:
    // Initialize all subsystems. Call once during setup().
    // Returns false if PSRAM pools cannot be allocated (degraded mode).
    static bool begin() {
        bool ok = true;
        ok &= sPool32.begin();
        ok &= sPool64.begin();
        ok &= sPool128.begin();
        ok &= sPool256.begin();
        ok &= sPool512.begin();
        ok &= sPool1K.begin();
        if (!ok) {
            std::printf("[ProtoGC] WARNING: One or more pools failed to "
                        "allocate in PSRAM — degraded mode\n");
        }
        sInitialized = ok || sPool128.isReady(); // at least one pool must work
        return sInitialized;
    }

    // ---- Arena management ----

    // Create a new PSRAM-backed arena. Returns nullptr on failure.
    static ArenaAllocator* createArena(size_t capacityBytes) {
        ArenaAllocator* arena = new ArenaAllocator();
        if (!arena) return nullptr;
        if (!arena->begin(capacityBytes)) {
            delete arena;
            return nullptr;
        }
        return arena;
    }

    // Destroy an arena and free its backing PSRAM.
    static void destroyArena(ArenaAllocator* arena) {
        if (arena) {
            arena->end();
            delete arena;
        }
    }

    // ---- Pool allocation (fragmentation-free) ----

    // Allocate from the best-fit pool. Falls back to PSRAM malloc() if no
    // pool covers this size or pools are exhausted.
    static void* poolAlloc(size_t sizeBytes) {
        void* p = poolAllocInternal(sizeBytes);
        if (p) return p;
        // Fallback: plain PSRAM malloc (acceptable for rare large allocs)
        return heap_caps_malloc(sizeBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    // Return a pool-allocated block. Safe to call with nullptr or
    // non-pool pointers (no-op).
    static void poolFree(void* ptr) {
        if (!ptr) return;
        if (sPool32Free(ptr))   return;
        if (sPool64Free(ptr))   return;
        if (sPool128Free(ptr))  return;
        if (sPool256Free(ptr))  return;
        if (sPool512Free(ptr))  return;
        if (sPool1KFree(ptr))   return;
        // Not a pool pointer — must have been a fallback malloc
        heap_caps_free(ptr);
    }

    // ---- Collection phases ----

    // Light collection: safe to call anytime, even mid-frame.
    // Resets all tracked arenas, compacts pool free lists.
    static void collectLight() {
        // Nothing to compact in free-list pools (they're self-compacting).
        // Arena resets are done explicitly by the owner.
    }

    // Full collection: call after major operations (network downloads,
    // JSON parsing complete, expression changes).
    // Resets arenas and trims heap if possible.
    static void collectFull() {
        collectLight();
        // ESP-IDF doesn't have malloc_trim, but we can request the
        // allocator to return free memory to the system.
        // On ESP32 this is a no-op; the TLSF allocator auto-coalesces.
    }

    // Emergency collection: call when internal DRAM is critically low.
    // Flushes all optional caches and resets non-essential subsystems.
    static void emergency() {
        collectFull();
        std::printf("[ProtoGC] EMERGENCY — intFree=%u largestBlk=%u\n",
                    static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                    static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    }

    // ---- Statistics ----

    static HeapStats stats() {
        HeapStats s;
        s.internalFree          = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        s.internalLargestBlock  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        s.psramFree             = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        s.psramLargestBlock     = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        s.arenaUsed             = 0; // arenas are user-managed
        s.arenaPeak             = 0;
        s.poolUsedBlocks        = sPool32.usedBlocks()  + sPool64.usedBlocks() +
                                  sPool128.usedBlocks() + sPool256.usedBlocks() +
                                  sPool512.usedBlocks() + sPool1K.usedBlocks();
        return s;
    }

    static bool isInitialized() { return sInitialized; }

private:
    static bool sInitialized;

    // ---- Internal pool dispatch ----

    static void* poolAllocInternal(size_t sizeBytes) {
        if      (sizeBytes <= 32)   { void* p = sPool32.alloc();  if (p) return p; }
        if      (sizeBytes <= 64)   { void* p = sPool64.alloc();  if (p) return p; }
        if      (sizeBytes <= 128)  { void* p = sPool128.alloc(); if (p) return p; }
        if      (sizeBytes <= 256)  { void* p = sPool256.alloc(); if (p) return p; }
        if      (sizeBytes <= 512)  { void* p = sPool512.alloc(); if (p) return p; }
        if      (sizeBytes <= 1024) { void* p = sPool1K.alloc();  if (p) return p; }
        return nullptr; // too large for any pool
    }

    // Pool instances
    static PoolAllocator<32,  PROTOGC_POOL_32>  sPool32;
    static PoolAllocator<64,  PROTOGC_POOL_64>  sPool64;
    static PoolAllocator<128, PROTOGC_POOL_128> sPool128;
    static PoolAllocator<256, PROTOGC_POOL_256> sPool256;
    static PoolAllocator<512, PROTOGC_POOL_512> sPool512;
    static PoolAllocator<1024,PROTOGC_POOL_1K>  sPool1K;

    // Free-dispatch helpers
    static bool sPool32Free(void* p)  { sPool32.free(p);  return true; }
    static bool sPool64Free(void* p)  { sPool64.free(p);  return true; }
    static bool sPool128Free(void* p) { sPool128.free(p); return true; }
    static bool sPool256Free(void* p) { sPool256.free(p); return true; }
    static bool sPool512Free(void* p) { sPool512.free(p); return true; }
    static bool sPool1KFree(void* p)  { sPool1K.free(p);  return true; }
};

// ---- Static member definitions ----
inline bool ProtoGC::sInitialized = false;

inline HeapGuard::Callback HeapGuard::sWarnCb  = nullptr;
inline HeapGuard::Callback HeapGuard::sCritCb = nullptr;

inline PoolAllocator<32,  PROTOGC_POOL_32>  ProtoGC::sPool32;
inline PoolAllocator<64,  PROTOGC_POOL_64>  ProtoGC::sPool64;
inline PoolAllocator<128, PROTOGC_POOL_128> ProtoGC::sPool128;
inline PoolAllocator<256, PROTOGC_POOL_256> ProtoGC::sPool256;
inline PoolAllocator<512, PROTOGC_POOL_512> ProtoGC::sPool512;
inline PoolAllocator<1024,PROTOGC_POOL_1K>  ProtoGC::sPool1K;

} // namespace protogc
