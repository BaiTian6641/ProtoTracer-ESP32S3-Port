# GPU Memory Management API — Refined Design

> **ProtoGL v0.7 — Enhanced Memory Management**
>
> This document refines and extends the GPU memory management system from v0.5
> (basic alloc/free/tier-placement) into a comprehensive Vulkan-inspired memory
> management API with resource binding, memory pools, defragmentation, and
> diagnostic introspection.

---

## §1 Motivation

The v0.5 memory management API provides basic building blocks:
- `MemAlloc` / `MemFree` (allocate/free in a specific tier)
- `MemWrite` / `MemReadRequest` (direct byte-level access)
- `SetResourceTier` (tier placement hints)
- `MemCopy` (GPU-internal tier-to-tier copy)

While functional, this design has gaps compared to production graphics APIs:

| Gap | Impact | v0.7 Solution |
|-----|--------|---------------|
| No memory pools | Fragmentation over time | Pool allocator per resource class |
| No defragmentation | Long-running sessions degrade | Online compaction with handle indirection |
| No memory budgeting | OOM crashes unpredictable | Per-tier budget with eviction policy |
| No resource binding | Manual address management | Bind handles to resource slots |
| Limited diagnostics | Hard to debug memory issues | Detailed per-allocation tracking |
| No streaming | Large resources can't stream | Streaming upload with staging buffers |
| No memory mapping | Host must use read/write commands | Map GPU memory into command stream |
| Unidirectional data flow | Host can write, no fast read-back | Bidirectional Octal SPI + shared memory window + mailbox |

### Design Goals

1. **Vulkan-Inspired** — Memory types, memory pools, bind/unbind, mapping
2. **Microcontroller-Friendly** — No heap allocation on GPU; all tracking is fixed-size
3. **Transparent Tiering** — Automatic promotion/demotion remains the default
4. **Backward Compatible** — v0.5 commands still work unchanged
5. **Observable** — Rich diagnostic queries for host-side memory visualization

---

## §2 Memory Architecture (Refined)

### §2.1 Physical Memory Model

```
┌─────────────────────────────────────────────────────────────────┐
│                        GPU Physical Memory                       │
│                                                                   │
│  Tier 0: SRAM (520 KB)                    [1-cycle access]       │
│  ┌─────────────────────────────────────────────────────────┐     │
│  │ System Reserved (60 KB)                                  │     │
│  │   Framebuffers, Z-buffer, stack, static data            │     │
│  │ ┌──────────────────────────────────────────────────────┐│     │
│  │ │ Dynamic SRAM Pool (~350 KB)                          ││     │
│  │ │   Scene state pools, mesh/material/texture data,     ││     │
│  │ │   shader programs, layer buffers                     ││     │
│  │ └──────────────────────────────────────────────────────┘│     │
│  │ Cache Arena (~64 KB)                                     │     │
│  │   Hot data promoted from lower tiers                    │     │
│  └─────────────────────────────────────────────────────────┘     │
│                                                                   │
│  Tier 1: QSPI-A External VRAM (up to 2 chips, PSRAM / MRAM)     │
│  ┌─────────────────────────────────────────────────────────┐     │
│  │ Warm data: large meshes, textures, LUTs                  │     │
│  │ [DMA access via PIO2 SM0+SM1: <1 µs for 256-byte block]   │     │
│  └─────────────────────────────────────────────────────────┘     │
│                                                                   │
│  Tier 2: QSPI-B External VRAM (up to 2 chips, PSRAM / MRAM)     │
│  ┌─────────────────────────────────────────────────────────┐     │
│  │ Cold data: font glyphs, color LUTs, large textures       │     │
│  │ [DMA access via PIO2 SM2+SM3: <1 µs for 256-byte block]   │     │
│  └─────────────────────────────────────────────────────────┘     │
└─────────────────────────────────────────────────────────────────┘
```

### §2.2 Memory Types (Vulkan-Style)

```cpp
/// Memory type bits — combine to describe memory requirements
enum PglMemoryPropertyFlags : uint16_t {
    PGL_MEM_DEVICE_LOCAL     = 0x0001,  // GPU-local (fastest tier for resource class)
    PGL_MEM_HOST_VISIBLE     = 0x0002,  // Readable by host via bidirectional Octal SPI
    PGL_MEM_HOST_COHERENT    = 0x0004,  // No explicit flush needed (MRAM/SRAM)
    PGL_MEM_LAZILY_ALLOCATED = 0x0008,  // May not allocate until first access (future)
    PGL_MEM_NON_VOLATILE     = 0x0010,  // Survives power cycle (MRAM only)
    PGL_MEM_RANDOM_ACCESS    = 0x0020,  // No row-buffer penalty (SRAM, MRAM)
    PGL_MEM_DMA_CAPABLE      = 0x0040,  // Can be DMA source/destination
    PGL_MEM_CACHEABLE        = 0x0080,  // Can be cached in SRAM arena
    PGL_MEM_HOST_WRITABLE    = 0x0100,  // Host can write directly (shared memory window)
    PGL_MEM_BIDIRECTIONAL    = 0x0200,  // Both host and GPU have direct access
};

/// Memory type descriptor (reported by GPU capability query)
struct PglMemoryType {
    uint16_t    propertyFlags;   // PglMemoryPropertyFlags bitmask
    uint8_t     tier;            // PglMemTier backing store
    uint32_t    totalBytes;      // Total capacity
    uint32_t    freeBytes;       // Currently available
    uint16_t    maxAllocSize;    // Largest possible single allocation
    uint8_t     alignment;       // Required alignment (log2)
};

/// GPU reports available memory types (up to 8)
static constexpr uint8_t PGL_MAX_MEMORY_TYPES = 8;
```

---

## §3 Memory Pools

### §3.1 Pool Concept

Memory pools pre-allocate a region in a tier and sub-allocate from it with minimal fragmentation. This is similar to Vulkan's `VkDeviceMemory` → `vkBindBufferMemory` pattern.

```cpp
/// Pool creation parameters
struct PglMemPoolConfig {
    uint8_t  tier;               // PglMemTier
    uint32_t size;               // Pool size in bytes
    uint16_t blockSize;          // Minimum allocation unit (power of 2, 16–4096)
    uint16_t maxAllocations;     // Max simultaneous allocations within this pool
    uint8_t  resourceClass;      // PglMemResourceClass (for tier placement hints)
    uint8_t  flags;              // bit0: allowDefrag, bit1: pinned (no tier migration)
};

/// Pool handle
using PglMemPool = uint8_t;
static constexpr PglMemPool PGL_INVALID_MEM_POOL = 0xFF;
static constexpr uint8_t PGL_MAX_MEM_POOLS = 16;
```

### §3.2 New SPI Commands for Pools

| Opcode | Name | Description |
|--------|------|-------------|
| `0x38` | `CMD_MEM_CREATE_POOL` | Create a memory pool in a tier |
| `0x39` | `CMD_MEM_DESTROY_POOL` | Destroy pool and free all allocations |
| `0x3A` | `CMD_MEM_POOL_ALLOC` | Allocate from a pool |
| `0x3B` | `CMD_MEM_POOL_FREE` | Free an allocation within a pool |
| `0x3C` | `CMD_MEM_DEFRAG` | Request defragmentation of a pool |
| `0x3D` | `CMD_MEM_BIND_RESOURCE` | Bind a resource handle to a pool allocation |
| `0x3E` | `CMD_MEM_QUERY_STATS` | Request detailed memory statistics |

### §3.3 Wire-Format Structures

```cpp
#pragma pack(push, 1)

// CMD_MEM_CREATE_POOL (0x38)
struct PglCmdMemCreatePool {
    uint8_t  poolId;           // 0–15
    uint8_t  tier;             // PglMemTier
    uint32_t size;             // Pool size in bytes
    uint16_t blockSize;        // Minimum alloc unit
    uint16_t maxAllocations;
    uint8_t  resourceClass;    // PglMemResourceClass
    uint8_t  flags;
};

// CMD_MEM_DESTROY_POOL (0x39)
struct PglCmdMemDestroyPool {
    uint8_t  poolId;
};

// CMD_MEM_POOL_ALLOC (0x3A)
struct PglCmdMemPoolAlloc {
    uint8_t  poolId;
    uint32_t size;             // Bytes requested
    uint16_t tag;              // Caller-defined identification tag
    uint8_t  alignment;        // Required alignment (log2, 0 = pool default)
};

// CMD_MEM_POOL_FREE (0x3B)
struct PglCmdMemPoolFree {
    uint8_t      poolId;
    PglMemHandle handle;       // Handle returned by pool alloc
};

// CMD_MEM_DEFRAG (0x3C)
struct PglCmdMemDefrag {
    uint8_t  poolId;           // 0xFF = all pools
    uint8_t  urgency;          // 0 = lazy (1 block per frame), 1 = moderate, 2 = full compact
};

// CMD_MEM_BIND_RESOURCE (0x3D)
struct PglCmdMemBindResource {
    uint8_t      resourceClass;  // PglMemResourceClass
    uint16_t     resourceId;     // Mesh/Material/Texture handle
    PglMemHandle memHandle;      // Pool allocation handle
    uint32_t     offset;         // Byte offset within the allocation
};

// CMD_MEM_QUERY_STATS (0x3E)
struct PglCmdMemQueryStats {
    uint8_t queryType;         // 0 = global, 1 = per-pool, 2 = per-tier, 3 = allocation list
    uint8_t poolId;            // When queryType=1
    uint8_t tier;              // When queryType=2
};

#pragma pack(pop)
```

---

## §4 I2C Diagnostic Extensions

### §4.1 New I2C Registers

| Register | Address | R/W | Size | Description |
|----------|---------|-----|------|-------------|
| `PGL_REG_MEM_POOL_INFO` | 0x18 | R | 16 | Pool stats (write pool index first) |
| `PGL_REG_MEM_HEAP_INFO` | 0x19 | R | 20 | Global heap statistics |
| `PGL_REG_MEM_ALLOC_LIST` | 0x1A | R | 32 | Per-allocation detail (paginated) |
| `PGL_REG_MEM_DEFRAG_STATUS` | 0x1B | R | 8 | Defragmentation progress |

### §4.2 Response Structures

```cpp
#pragma pack(push, 1)

/// Per-pool statistics (PGL_REG_MEM_POOL_INFO)
struct PglMemPoolInfoResponse {
    uint8_t  poolId;
    uint8_t  tier;
    uint32_t totalSize;
    uint32_t usedBytes;
    uint16_t allocationCount;
    uint16_t maxAllocations;
    uint8_t  fragmentationPercent;  // 0–100
    uint8_t  flags;                 // bit0: defragInProgress
};
static_assert(sizeof(PglMemPoolInfoResponse) == 16, "PglMemPoolInfoResponse must be 16 bytes");

/// Global heap statistics (PGL_REG_MEM_HEAP_INFO)
struct PglMemHeapInfoResponse {
    // Tier 0 (SRAM)
    uint32_t sramTotal;
    uint32_t sramUsed;
    uint16_t sramAllocCount;
    // Tier 1 (QSPI-A)
    uint32_t tier1Total;
    uint32_t tier1Used;
    uint16_t tier1AllocCount;
    // Metadata
    uint8_t  poolCount;          // Active memory pools
    uint8_t  defragPending;      // Number of pools needing defrag
};
static_assert(sizeof(PglMemHeapInfoResponse) == 20, "PglMemHeapInfoResponse must be 20 bytes");

/// Per-allocation detail entry (PGL_REG_MEM_ALLOC_LIST, paginated)
struct PglMemAllocEntry {
    PglMemHandle handle;
    uint8_t      poolId;
    uint8_t      tier;
    uint32_t     address;
    uint32_t     size;
    uint16_t     tag;
    uint8_t      resourceClass;
    uint16_t     resourceId;
    uint8_t      accessScore;     // 0–255 (tiering manager hotness score)
    uint32_t     lastAccessFrame; // Frame number of last access
    uint8_t      flags;           // bit0: pinned, bit1: dirty, bit2: cached
    // Padding to 20 bytes
    uint8_t      reserved[2];
};

/// Defragmentation status (PGL_REG_MEM_DEFRAG_STATUS)
struct PglMemDefragStatusResponse {
    uint8_t  poolId;
    uint8_t  status;             // 0=idle, 1=in-progress, 2=completed, 3=failed
    uint16_t blocksMoved;
    uint16_t blocksRemaining;
    uint16_t bytesRecovered;
};
static_assert(sizeof(PglMemDefragStatusResponse) == 8, "PglMemDefragStatusResponse must be 8 bytes");

#pragma pack(pop)
```

---

## §5 GPU-Side Memory Pool Implementation

### §5.1 Pool Allocator Design

```cpp
/// GPU-side pool allocator (fixed overhead, no heap allocation)
class MemPool {
public:
    bool Initialize(uint8_t poolId, PglMemTier tier, uint32_t baseAddr,
                    uint32_t size, uint16_t blockSize, uint16_t maxAllocs);
    void Shutdown();

    /// Allocate from pool. Returns handle or PGL_INVALID_MEM_HANDLE.
    PglMemHandle Alloc(uint32_t size, uint8_t alignment, uint16_t tag);

    /// Free an allocation within this pool.
    bool Free(PglMemHandle handle);

    /// Defragment: move one block per call (incremental).
    /// Returns true if more work remains.
    bool DefragStep();

    /// Full compaction (blocks until complete).
    void DefragFull();

    /// Query stats.
    PglMemPoolInfoResponse GetStats() const;

    /// Get the GPU-side address for a handle.
    uint32_t GetAddress(PglMemHandle handle) const;

private:
    // Allocation tracking (fixed-size array)
    struct AllocRecord {
        PglMemHandle handle;
        uint32_t     offset;      // Offset within pool
        uint32_t     size;
        uint16_t     tag;
        bool         active;
    };

    uint8_t      poolId_;
    PglMemTier   tier_;
    uint32_t     baseAddr_;
    uint32_t     totalSize_;
    uint16_t     blockSize_;

    AllocRecord  records_[256];   // Max allocations per pool
    uint16_t     recordCount_;
    uint16_t     maxRecords_;
    uint32_t     usedBytes_;

    // Free-list header for first-fit allocation
    uint32_t     freeListHead_;
};
```

### §5.2 Defragmentation Strategy

Defragmentation uses handle indirection — resources reference memory through handles, not raw addresses. When blocks are moved during defrag, only the handle table is updated.

```
Before Defrag:          After Defrag:
┌────┬──┬────┬──┬────┐  ┌────┬────┬────┬──────────┐
│ A  │  │ B  │  │ C  │  │ A  │ B  │ C  │  FREE    │
│128 │64│256 │96│128 │  │128 │256 │128 │  352     │
└────┴──┴────┴──┴────┘  └────┴────┴────┴──────────┘
  Used: 512/672            Used: 512/672
  Max free block: 96       Max free block: 352
  Fragmentation: 24%       Fragmentation: 0%
```

Incremental defrag moves one block per frame to avoid stalls. The GPU firmware calls `DefragStep()` during idle time between frames.

---

## §6 Streaming Upload

For large resources that don't fit in a single command buffer frame:

### §6.1 Streaming Protocol

```cpp
// New opcodes for streaming
static constexpr uint8_t PGL_CMD_MEM_STREAM_BEGIN  = 0x3F;  // Begin streaming upload
static constexpr uint8_t PGL_CMD_MEM_STREAM_CHUNK  = 0x40;  // Upload chunk
static constexpr uint8_t PGL_CMD_MEM_STREAM_END    = 0x41;  // Finalize stream

#pragma pack(push, 1)

// CMD_MEM_STREAM_BEGIN (0x3F)
struct PglCmdMemStreamBegin {
    PglMemHandle handle;       // Target allocation handle
    uint32_t     totalSize;    // Total bytes to stream
    uint16_t     chunkSize;    // Max bytes per chunk (recommended: 4096)
};

// CMD_MEM_STREAM_CHUNK (0x40) — header; followed by chunk data
struct PglCmdMemStreamChunkHeader {
    PglMemHandle handle;
    uint16_t     chunkIndex;   // 0-based chunk number
    uint16_t     chunkSize;    // Bytes in this chunk
    // Followed by chunkSize bytes of data
};

// CMD_MEM_STREAM_END (0x41)
struct PglCmdMemStreamEnd {
    PglMemHandle handle;
    uint32_t     crc32;        // CRC-32 of complete data (optional, 0 = skip)
};

#pragma pack(pop)
```

### §6.2 Host-Side Streaming API

```cpp
class PglEncoder {
    // ... existing methods ...

    /// Begin streaming a large resource to GPU memory across multiple frames.
    void MemStreamBegin(PglMemHandle handle, uint32_t totalSize, uint16_t chunkSize = 4096);

    /// Upload one chunk of a streaming resource.
    void MemStreamChunk(PglMemHandle handle, uint16_t chunkIndex,
                        const void* data, uint16_t chunkSize);

    /// Finalize streaming upload with optional CRC verification.
    void MemStreamEnd(PglMemHandle handle, uint32_t crc32 = 0);
};
```

### §6.3 Streaming Use Cases

Streaming is essential for resources that exceed the per-frame command buffer capacity
(default 32 KB). Common streaming candidates:

| Resource | Typical Size | Frames to Upload (4 KB chunks) |
|---|---|---|
| Large texture (128×128 RGB565) | 32 KB | 8 frames |
| Image sequence (32 frames × 64×64 RGB565) | 256 KB | 64 frames |
| Image sequence (16 frames × 128×64 RGB888) | 384 KB | 96 frames |
| Custom font atlas (256×128 GRAY8) | 32 KB | 8 frames |

**Example: Streaming a 32-frame image sequence to external VRAM**

```cpp
// Step 1: Create the image sequence header (allocates GPU-side handle)
enc->CreateImageSequenceHeader(seqId, 64, 64, 32, PGL_TEX_RGB565,
                               PGL_LOOP_LOOP, 12.0f);
// GPU automatically places the atlas in external VRAM (Tier 1/2)

// Step 2: Begin streaming the pixel data
uint32_t totalSize = 64 * 64 * 2 * 32;  // 256 KB
enc->MemStreamBegin(seqId | PGL_HANDLE_IMAGE_SEQ, totalSize, 4096);

// Step 3: Upload chunks across subsequent frames (4 KB per chunk)
// Typically 4–8 chunks per frame to stay within command buffer budget
for (uint16_t i = 0; i < totalSize / 4096; i++) {
    enc->MemStreamChunk(seqId | PGL_HANDLE_IMAGE_SEQ, i,
                        atlasPixels + i * 4096, 4096);
}

// Step 4: Finalize with CRC check
enc->MemStreamEnd(seqId | PGL_HANDLE_IMAGE_SEQ, computedCrc32);

// The image sequence is now usable by both 3D materials and 2D sprites
```

---

## §7 Resource Binding Model

### §7.1 Concept

Instead of having resource creation commands (CreateMesh, CreateTexture) implicitly allocate GPU memory, v0.7 separates memory allocation from resource creation:

```
Vulkan-Style Resource Lifecycle:
1. Create resource handle  →  PglCmdCreateMesh (handle allocated, no data)
2. Allocate GPU memory     →  PglCmdMemPoolAlloc (returns memHandle)
3. Bind memory to resource →  PglCmdMemBindResource (links mesh → memory)
4. Upload data             →  PglCmdMemWrite (fill the bound memory)
5. Use resource            →  PglCmdDrawObject (reference by handle)
6. Unbind & free           →  PglCmdMemPoolFree + PglCmdDestroyMesh
```

**Backward Compatibility:** The existing `CreateMesh`/`CreateMaterial`/`CreateTexture` commands continue to work as before — they internally use the default SRAM pool with automatic allocation. The new bind model is opt-in for advanced use cases.

### §7.2 Benefits

- **Predictable memory usage** — explicit allocation before binding
- **Sub-allocation** — multiple small resources share one large allocation
- **Defrag-safe** — resources bound via handles, not addresses
- **Cross-tier** — bind a mesh to QSPI-A VRAM, texture to QSPI-B VRAM
- **Debugging** — clear ownership chain from resource → memory → tier

### §7.3 Tier Placement Guidance by Resource Category

When using automatic allocation (the default `CreateTexture`/`CreateMaterial` path),
the GPU tiering manager applies the following placement rules. When using explicit
`MemPoolAlloc` + `MemBindResource`, the host must respect these guidelines:

| Resource | Tier | Rationale |
|---|---|---|
| **Color-based material params** (SimpleMaterial, NormalMaterial, DepthMaterial, GradientMaterial, LightMaterial, SimplexNoise, RainbowNoise, CombineMaterial, MaterialMask, **MaterialAnimator**) | **SRAM (Tier 0) — always** | 3–50 bytes each. Read at per-pixel frequency during rasterization; must be single-cycle. `MaterialAnimator` interpolates between two color materials every frame — still only 9 bytes. |
| **ImageMaterial / ImageSequenceMaterial params** | **SRAM (Tier 0)** | Only the 12–13 byte param block. Backing pixel data lives in the texture/sequence resource. |
| **Small textures** (≤ ~4 KB) | **SRAM (Tier 0)** | Icons, small sprites, simple patterns — fast texel lookup. |
| **Large textures** (> 4 KB) | **External VRAM (Tier 1/2)** | Placed by weight score. Hot texels promoted to SRAM cache arena via DMA. |
| **Image sequence atlases** | **External VRAM (Tier 1/2) — never SRAM** | KB–MB sized. Only the active frame region is DMA-fetched to SRAM cache arena per rasterization pass. |
| **Font atlases** | **External VRAM (prefer MRAM channel)** | Read-only after upload. MRAM's uniform latency handles glyph lookups efficiently. Small fonts (< 2 KB) may stay SRAM. |
| **Shader program bytecode** | **External VRAM (prefer MRAM channel)** | Read-only instruction stream, benefits from MRAM's uniform access latency. |
| **Vertex / index data** | **SRAM → QSPI-A** | Sequential access pattern, DMA-prefetchable. |

> **Example:** A scene uses a `SimpleMaterial` (3 bytes, SRAM), a `MaterialAnimator` blending
> between two `GradientMaterial`s (9 + 24 + 24 bytes, all SRAM), and an `ImageSequenceMaterial`
> referencing a 32-frame walk cycle at 64×64 RGB565 (256 KB atlas in QSPI-A VRAM, 13-byte
> param block in SRAM, ~8 KB active-frame cache in SRAM arena). Total SRAM for materials:
> ~73 bytes. Total external VRAM: ~256 KB.

---

## §8 Memory Budget & Eviction

### §8.1 Budget System

```cpp
/// Per-tier memory budget (set by host)
struct PglMemBudget {
    uint8_t  tier;
    uint32_t softLimit;    // Warning threshold (trigger promotion/demotion)
    uint32_t hardLimit;    // Absolute maximum (reject new allocations)
    uint8_t  evictionPolicy; // 0=LRU, 1=LFU, 2=priority-based
};
```

### §8.2 Eviction Policy

When a tier exceeds its soft limit, the GPU's tiering manager automatically:

1. **Identify cold resources** — Lowest access score in the over-budget tier
2. **Demote to lower tier** — Copy data to Tier 1 or Tier 2 via DMA
3. **Free SRAM** — Update handle table to point to new tier
4. **Promote hot data** — If lower tier has frequently accessed data, promote to SRAM

The eviction runs incrementally (one resource per frame) to avoid frame stalls.

### §8.3 Resource Persistence & Flash Writeback

Large resources — textures, image sequence atlases, font atlases — are always placed
in external VRAM (Tier 1 or Tier 2) by default. When no external memory is detected
the GPU falls back to SRAM, but the *design target* is external memory first. The GPU
must support a **persistence policy** so that these expensive-to-upload resources survive
power cycles when appropriate.

#### §8.3.1 Persistence Decision Tree

The persistence behaviour depends on two factors: (1) the detected external memory
type, and (2) whether the host explicitly requests persistence for a given resource.

```
                     ┌─────────────────────────┐
                     │  Resource uploaded to    │
                     │  External VRAM (T1/T2)   │
                     └───────────┬─────────────┘
                                 │
                    ┌────────────▼────────────┐
                    │ Detect VRAM chip type    │
                    │ (PglQspiChipType from    │
                    │  boot probe)             │
                    └────────────┬────────────┘
                                 │
             ┌───────────────────┴───────────────────┐
             │                                       │
  ┌──────────▼──────────┐              ┌─────────────▼─────────────┐
  │ MRAM (non-volatile) │              │ PSRAM (volatile)          │
  │ e.g., MR10Q010      │              │ e.g., APS6408L            │
  └──────────┬──────────┘              └─────────────┬─────────────┘
             │                                       │
  Data already persistent.              ┌────────────▼────────────┐
  No action needed.                     │ Host requests persist?  │
  Resource table records                │ (CMD_PERSIST_RESOURCE   │
  tier+address — survives               │  with PERSIST flag)     │
  power cycle as-is.                    └────────────┬────────────┘
             │                                       │
             │                          ┌────────────┴────────────┐
  ┌──────────▼──────────┐   ┌──────────▼──────┐     ┌────────────▼──────┐
  │ Done. Info points    │   │ Yes: Writeback  │     │ No: Volatile OK.  │
  │ to MRAM location.   │   │ to GPU flash.   │     │ Resource lost on  │
  │ No flash write.     │   │                 │     │ power cycle.      │
  └─────────────────────┘   └────────┬────────┘     └───────────────────┘
                                     │
                            ┌────────▼────────┐
                            │GPU DMA-copies    │
                            │resource from     │
                            │PSRAM → on-board  │
                            │flash (async,     │
                            │background,       │
                            │after frame end). │
                            │Updates resource  │
                            │table: flashAddr, │
                            │persistent=true.  │
                            └────────┬────────┘
                                     │
                            ┌────────▼────────┐
                            │On next boot,     │
                            │GPU checks flash  │
                            │manifest. If found│
                            │→ DMA from flash  │
                            │to PSRAM, rebuild │
                            │resource table.   │
                            │Host skips upload.│
                            └─────────────────┘
```

#### §8.3.2 Flash Persistence Architecture

The GPU reserves a **flash manifest region** at a fixed offset in its on-board QSPI
flash (typically the last 64 KB sector). The manifest tracks which resources have been
persisted:

```cpp
/// Flash manifest header (stored at GPU_FLASH_MANIFEST_ADDR)
static constexpr uint32_t PGL_FLASH_MANIFEST_MAGIC = 0x50474C46; // "PGLF"
static constexpr uint32_t PGL_FLASH_MANIFEST_ADDR  = 0x10380000; // Last 512 KB of 4 MB flash
static constexpr uint16_t PGL_FLASH_MAX_PERSISTED  = 64;         // Max persisted resources

struct PglFlashManifestHeader {
    uint32_t magic;           // PGL_FLASH_MANIFEST_MAGIC
    uint16_t version;         // Manifest format version
    uint16_t entryCount;      // Number of persisted resources
    uint32_t totalBytes;      // Total flash bytes used by persisted data
    uint32_t crc32;           // CRC-32 over all entries + data
};

struct PglFlashManifestEntry {
    uint8_t  resourceClass;   // PglMemResourceClass
    uint16_t resourceId;      // Resource handle
    uint32_t flashOffset;     // Offset from PGL_FLASH_MANIFEST_ADDR + header
    uint32_t dataSize;        // Size of persisted data in bytes
    uint8_t  sourceTier;      // Tier the data was originally in
    uint8_t  flags;           // bit0: valid, bit1: dirty (needs re-upload)
    uint32_t uploadCrc32;     // CRC-32 of original upload data
    uint8_t  reserved[2];     // Padding to 16 bytes
};
```

#### §8.3.3 Writeback Mechanism

When the host sends `CMD_PERSIST_RESOURCE` and the external VRAM is volatile (PSRAM):

1. **Queue the writeback** — GPU adds the resource to a background writeback queue
   (max 4 pending). The writeback does **not** stall the current frame.
2. **DMA copy to SRAM staging** — At the end of the current frame (after `EndFrame`),
   the GPU DMA-copies the resource from external VRAM into a 4 KB staging buffer in SRAM.
3. **Flash erase + program** — GPU erases the target flash sector (if needed) and
   programs the data in 256-byte pages. The RP2350's QSPI flash supports page-program
   while XIP is active on a different bank. Write throughput: ~200 KB/s.
4. **Update manifest** — After the full resource is written, the GPU updates the
   flash manifest entry: set `valid=1`, record `flashOffset`, `dataSize`, `uploadCrc32`.
5. **Notify host** — GPU sets `PGL_MBOX_GPU_PERSIST_STATUS` mailbox slot (slot 9)
   with the completed resource handle, and pulses IRQ.

For large resources (> 4 KB), the writeback is **incremental**: 4 KB per frame, spread
across multiple frames. A 256 KB image sequence atlas takes ~64 frames (~1 second at
60 FPS) to fully persist.

#### §8.3.4 Boot Restore from Flash

When the GPU boots and detects volatile external VRAM (PSRAM):

1. **Check manifest** — Read `PglFlashManifestHeader` from flash. Verify magic + CRC.
2. **Restore resources** — For each valid entry, DMA-copy from flash to the appropriate
   external VRAM tier at the original address. Rebuild the resource table entry.
3. **Report to host** — The capability response includes a `persistedResourceCount`
   field. The host can query `MEM_PERSIST_STATUS` (I2C 0x1C) for the full list.
4. **Host skips redundant uploads** — If the host detects that a resource is already
   persisted (via manifest query), it can skip the `CreateTexture` / `CreateImageSequence`
   upload entirely, saving significant SPI bandwidth on startup.

#### §8.3.5 MRAM Persistence (Zero-Cost)

When external VRAM is MRAM (non-volatile, unlimited write endurance):

- Resources stored in MRAM are **inherently persistent**. No flash writeback needed.
- The GPU's resource table records the MRAM tier + address. On reboot, the resource
  table is rebuilt from the MRAM-resident manifest header (stored at MRAM offset 0x0000).
- The host queries capability flags: if `PGL_CAP_NVRAM_VRAM` is set, persistence is
  automatic for all Tier 1/2 resources. No `CMD_PERSIST_RESOURCE` needed.
- MRAM supports unlimited write cycles, so no wear-leveling or erase-before-write
  overhead. Write-through from SRAM cache is safe at any frequency.

#### §8.3.6 Persistence-Related Configuration

| Config | Default | Description |
|---|---|---|
| `FLASH_PERSIST_ENABLED` | `true` | Enable flash persistence subsystem |
| `FLASH_PERSIST_MAX_ENTRIES` | 64 | Maximum number of persisted resource entries |
| `FLASH_PERSIST_REGION_SIZE` | 524288 | Flash bytes reserved for persistence (512 KB) |
| `FLASH_PERSIST_STAGING_SIZE` | 4096 | SRAM staging buffer for flash writes (4 KB) |
| `FLASH_PERSIST_MAX_PER_FRAME` | 4096 | Maximum bytes written to flash per frame |
| `MRAM_AUTO_PERSIST` | `true` | If MRAM detected, all external resources auto-persist |

### §8.4 Direct Framebuffer Write

The standard ProtoGL pipeline has the GPU rasterize the scene and produce the output
framebuffer internally. However, many use cases benefit from the host writing **raw
pixels directly** into the GPU's output framebuffer:

- **Pre-rendered content** — Host has its own rasterizer / image decoder and wants to
  bypass the GPU scene pipeline entirely (Option B from Architecture_Split.md)
- **Camera passthrough** — Host captures frames from an external camera and displays
  them directly
- **Hybrid rendering** — GPU renders 3D scene on Layer 0; host injects pre-rendered
  UI overlays by writing directly into Layer 1's buffer
- **Debug visualization** — Host draws debug markers / bounding boxes directly

#### §8.4.1 Architecture

The host uses `CMD_WRITE_FRAMEBUFFER` to push raw pixel data (RGB565) directly into
a target framebuffer:

```
    Host (ESP32-S3)                       GPU (RP2350)
    ┌─────────────────┐                  ┌───────────────────────────────┐
    │ PglEncoder::     │   Octal SPI     │ Command Parser                │
    │  WriteFramebuffer│──────────────→  │   case CMD_WRITE_FRAMEBUFFER: │
    │  (x, y, w, h,   │   ~80 MB/s TX   │   ├─ Validate bounds          │
    │   pixelData)     │                 │   ├─ memcpy → back buffer     │
    └─────────────────┘                  │   └─ (or target layer buffer) │
                                          │                               │
                                          │  On EndFrame:                 │
                                          │  ├─ Normal pipeline runs      │
                                          │  │  (if any draw calls)       │
                                          │  ├─ Compositor blends layers  │
                                          │  └─ SwapBuffers → display     │
                                          └───────────────────────────────┘
```

- **Target:** The command writes into the **back buffer** (the buffer not currently
  being displayed). After `CMD_END_FRAME`, the GPU swaps buffers as usual.
- **Layer support:** If compositing layers are active, the host can target a specific
  layer buffer instead of the 3D framebuffer. This allows mixed GPU-rendered and
  host-rendered content.
- **Format:** RGB565 only (native framebuffer format). The host is responsible for any
  format conversion before sending.
- **No pipeline interaction:** Direct framebuffer writes are raw `memcpy` into the
  target buffer. They bypass the rasterizer, QuadTree, Z-buffer, and shaders. If the
  host writes the entire frame, it can skip all `DrawObject` calls and still get
  output via `EndFrame`.

#### §8.4.2 Wire Format

See `CMD_WRITE_FRAMEBUFFER` (0x45) in [ProtoGL_API_Spec.md](ProtoGL_API_Spec.md) §4.10.

#### §8.4.3 Performance

| Scenario | Data Size | SPI Time | Notes |
|---|---|---|---|
| Full frame (128×64) | 16,384 B | ~0.2 ms @ 80 MHz | Faster than GPU rasterization for simple content |
| Half frame (128×32) | 8,192 B | ~0.1 ms | Partial update for scrolling text |
| Small region (32×16) | 1,024 B | ~13 µs | Icon/badge overlay |
| Per-frame overhead | 3+8 bytes | ~0.1 µs | Command header + region descriptor |

The direct framebuffer write is the **fastest possible** path to put pixels on screen —
no GPU-side computation, just a memcpy from SPI buffer to framebuffer.

---

## §9 Host-Side PglEncoder Extensions

```cpp
class PglEncoder {
    // ... existing methods ...

    // ─── Memory Pool Management ──────────────────────────────────────

    /// Create a memory pool in a specific tier.
    void MemCreatePool(uint8_t poolId, PglMemTier tier, uint32_t size,
                       uint16_t blockSize, uint16_t maxAllocations,
                       PglMemResourceClass resourceClass, uint8_t flags = 0);

    /// Destroy a memory pool and free all its allocations.
    void MemDestroyPool(uint8_t poolId);

    /// Allocate from a pool. Result via PGL_REG_MEM_ALLOC_RESULT.
    void MemPoolAlloc(uint8_t poolId, uint32_t size, uint16_t tag = 0,
                      uint8_t alignment = 0);

    /// Free an allocation within a pool.
    void MemPoolFree(uint8_t poolId, PglMemHandle handle);

    /// Request defragmentation of a pool.
    void MemDefrag(uint8_t poolId = 0xFF, uint8_t urgency = 0);

    /// Bind a resource to a pool allocation.
    void MemBindResource(PglMemResourceClass resourceClass, uint16_t resourceId,
                         PglMemHandle memHandle, uint32_t offset = 0);

    /// Request detailed memory statistics (result via I2C).
    void MemQueryStats(uint8_t queryType, uint8_t poolId = 0, uint8_t tier = 0);

    // ─── Streaming Upload ────────────────────────────────────────────

    /// Begin streaming a large resource to GPU memory.
    void MemStreamBegin(PglMemHandle handle, uint32_t totalSize,
                        uint16_t chunkSize = 4096);

    /// Upload one chunk of a streaming transfer.
    void MemStreamChunk(PglMemHandle handle, uint16_t chunkIndex,
                        const void* data, uint16_t chunkSize);

    /// Finalize streaming upload.
    void MemStreamEnd(PglMemHandle handle, uint32_t crc32 = 0);

    // ─── Resource Persistence ────────────────────────────────────────

    /// Request the GPU to persist a resource to flash (if VRAM is volatile PSRAM).
    /// On MRAM, this is a no-op (data is already persistent).
    /// flags: bit0=urgent (block until complete), bit1=delete-on-fail
    void PersistResource(PglMemResourceClass resourceClass, uint16_t resourceId,
                         uint8_t flags = 0);

    /// Restore a previously persisted resource from flash to VRAM.
    /// GPU checks the flash manifest and DMA-loads the data.
    void RestoreResource(PglMemResourceClass resourceClass, uint16_t resourceId);

    /// Query persistence status for a resource.
    /// Result available via SPI read (SPI_READ_PERSIST_STATUS) or I2C 0x1C.
    void QueryPersistence(PglMemResourceClass resourceClass, uint16_t resourceId);

    /// Erase a persisted resource from flash (free flash space).
    void ErasePersisted(PglMemResourceClass resourceClass, uint16_t resourceId);

    // ─── Direct Framebuffer Write ────────────────────────────────────

    /// Write raw RGB565 pixels directly into the GPU back buffer (or a layer buffer).
    /// x, y: top-left corner of the target region.
    /// w, h: dimensions of the pixel rectangle.
    /// layerId: 0xFF = main 3D framebuffer (back buffer); 0–7 = layer buffer.
    /// data: RGB565 pixel data, row-major, w×h×2 bytes.
    void WriteFramebuffer(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                          const void* data, uint8_t layerId = 0xFF);
};
```

---

## §9.5 Bidirectional Memory Access — Shared Memory Window

### §9.5.1 Problem Statement

The v0.5 data flow is asymmetric:

| Direction | Channel | Throughput | Latency |
|-----------|---------|------------|---------|
| Host → GPU | Octal SPI (CMD stream) | ~40 MB/s | ~1 µs per command |
| GPU → Host | I2C register read | ~50 KB/s | ~200 µs per read |

The host can blast data to the GPU at high speed, but **reading back** from the
GPU (scene state, diagnostic counters, computed values, framebuffer peek) is
limited to the slow I2C channel. For interactive protogen firmware this is
acceptable (most reads are small status words), but richer host-side features —
scene introspection, host-side collision queries, GPU-computed analytics,
shader debug readback — need a faster reverse channel.

### §9.5.2 Design Goals

1. **Direct Access for Both Sides** — Host and GPU can read/write a shared region
   without protocol overhead
2. **Low-Latency Small Reads** — Mailbox registers for quick status/result exchange
3. **Bulk Reverse Channel** — GPU can stage data that the host reads at Octal SPI speed
4. **Cache Coherent** — No stale data; explicit acquire/release semantics
5. **No Extra Hardware** — Works with existing bidirectional Octal SPI + GPIO pins

### §9.5.3 Architecture

```
┌──────────────── ESP32-S3 (Host) ──────────────────────┐
│                                                        │
│  PglDevice / PglEncoder                               │
│    ├── Octal SPI Master (Bidirectional) ──────┐       │
│    │     LCD_CAM (TX) / SPI2 (RX half-duplex) │       │
│    ├── DIR pin (output, GPIO 10) ────────┐    │       │
│    └── IRQ pin (input, GPIO 13) ←────────┤    │       │
│                                          │    │       │
└──────────────────────────────────────────┼────┼───────┘
                                           │    │
                                  DIR/IRQ  │  Octal SPI
                                  pins     │  D0-D7 + CLK
                                           │  (bidir)
                                           │    │
┌──────────────────────────────────────────┼────┼───────┐
│                RP2350 (GPU)              │    │       │
│                                          │    │       │
│  PIO1 SM0 (RX) / SM1 (TX) ──────────────┼────┘       │
│  GPIO 13 (IRQ output) ──────────────────►┘            │
│                                                        │
│  ┌───────────────────────────────▼───────▼──────────┐ │
│  │          Shared Memory Window (SMW)              │ │
│  │          4 KB in SRAM @ fixed address            │ │
│  │                                                  │ │
│  │  ┌──────────┐ ┌──────────┐ ┌───────────────┐     │ │
│  │  │ Mailbox  │ │ Mailbox  │ │  Bulk Data    │     │ │
│  │  │ Region   │ │ Region   │ │  Staging      │     │ │
│  │  │ (Host→GPU│ │ (GPU→Host│ │  Buffer       │     │ │
│  │  │  64 B)   │ │  64 B)   │ │  (3840 B)     │     │ │
│  │  └──────────┘ └──────────┘ └───────────────┘     │ │
│  └──────────────────────────────────────────────────┘ │
│                                                        │
│  GPU firmware reads/writes SMW at SRAM speed           │
│  Host writes via SPI TX, reads via SPI RX (half-duplex)│
│  GPIO IRQ signals "new data ready" to host             │
└────────────────────────────────────────────────────────┘
```

### §9.5.4 Shared Memory Window Layout

```cpp
/// Fixed address in GPU SRAM for the Shared Memory Window
static constexpr uint32_t PGL_SMW_BASE_ADDR = 0x20070000;  // Near top of SRAM
static constexpr uint32_t PGL_SMW_SIZE      = 4096;         // 4 KB total

/// SMW sub-regions
static constexpr uint32_t PGL_SMW_HOST_TO_GPU_MAILBOX  = PGL_SMW_BASE_ADDR;         // 64 bytes
static constexpr uint32_t PGL_SMW_GPU_TO_HOST_MAILBOX  = PGL_SMW_BASE_ADDR + 64;    // 64 bytes
static constexpr uint32_t PGL_SMW_BULK_STAGING         = PGL_SMW_BASE_ADDR + 128;   // 3840 bytes
static constexpr uint32_t PGL_SMW_SEQUENCE_COUNTER     = PGL_SMW_BASE_ADDR + 3968;  // 4 bytes
static constexpr uint32_t PGL_SMW_FLAGS                = PGL_SMW_BASE_ADDR + 3972;  // 4 bytes

/// Mailbox register layout (64 bytes each direction)
/// Each mailbox slot is 4 bytes (uint32_t). 16 slots per direction.
struct PglMailbox {
    uint32_t slots[16];  // 16 × 32-bit general-purpose mailbox registers
};

/// Well-known mailbox slot assignments (GPU→Host)
enum PglGpuMailboxSlot : uint8_t {
    PGL_MBOX_GPU_STATUS       = 0,   // GPU state flags (idle, busy, error)
    PGL_MBOX_GPU_FPS          = 1,   // Current GPU-side FPS × 100
    PGL_MBOX_GPU_FRAME_NUM    = 2,   // Last completed frame number
    PGL_MBOX_GPU_FREE_SRAM    = 3,   // Free SRAM bytes
    PGL_MBOX_GPU_CORE0_LOAD   = 4,   // Core 0 utilization (0–1000 → 0.0%–100.0%)
    PGL_MBOX_GPU_CORE1_LOAD   = 5,   // Core 1 utilization
    PGL_MBOX_GPU_TEMP         = 6,   // Junction temperature × 10 (°C)
    PGL_MBOX_GPU_ERROR_CODE   = 7,   // Last error code
    PGL_MBOX_GPU_USER0        = 8,   // Application-defined
    // Slots 9–15: reserved / user-defined
};

/// Well-known mailbox slot assignments (Host→GPU)
enum PglHostMailboxSlot : uint8_t {
    PGL_MBOX_HOST_CMD         = 0,   // Quick command word (no SPI overhead)
    PGL_MBOX_HOST_PARAM0      = 1,   // Command parameter 0
    PGL_MBOX_HOST_PARAM1      = 2,   // Command parameter 1
    PGL_MBOX_HOST_BRIGHTNESS  = 3,   // Global brightness (quick update)
    PGL_MBOX_HOST_USER0       = 4,   // Application-defined
    // Slots 5–15: reserved / user-defined
};
```

### §9.5.5 Access Protocol

#### Host Reading from GPU (Fast Path)

**Instead of** I2C register read (~200 µs per word), the host uses a bidirectional Octal
SPI read transaction:

```
1. Host sends SPI_READ_SMW(PGL_SMW_GPU_TO_HOST_MAILBOX, 64)  → ~2 µs
   (TX phase: 4-byte command, turnaround, RX phase: 64 bytes @ 80 MHz)
2. Receive 64 bytes of mailbox data at Octal SPI speed
3. Parse mailbox slots locally
```

This is **100× faster** than I2C for reading GPU status.

#### Host Writing to GPU (Mailbox Path)

```
1. Host sends SPI MemWrite(PGL_SMW_HOST_TO_GPU_MAILBOX, data, 64)  → ~2 µs
2. GPU polls mailbox at start of each frame (or IRQ)
3. GPU reads updated slots and acts on them
```

#### GPU → Host Notification (IRQ)

When the GPU writes new data to the GPU→Host mailbox or bulk staging area,
it asserts a GPIO line (active-low pulse, ~1 µs). The host's ISR reads the
sequence counter to determine what changed:

```cpp
/// GPU increments this counter each time it updates the SMW
volatile uint32_t smwSequenceCounter;  // At PGL_SMW_SEQUENCE_COUNTER

/// Flags indicating which SMW region has new data
enum PglSmwFlags : uint32_t {
    PGL_SMW_FLAG_MAILBOX_UPDATED  = 0x01,  // GPU→Host mailbox has new data
    PGL_SMW_FLAG_BULK_READY       = 0x02,  // Bulk staging buffer has data
    PGL_SMW_FLAG_ERROR            = 0x04,  // Error mailbox slot updated
};
```

#### Bulk Data Reverse Channel

For larger reads (framebuffer peek, shader debug dump, allocation list):

```
1. Host sends CMD_MEM_STAGE_READ(srcAddr, size) via SPI TX
2. GPU copies requested data into SMW bulk staging buffer (DMA)
3. GPU sets PGL_SMW_FLAG_BULK_READY and pulses IRQ
4. Host reads SMW bulk staging via SPI_READ_SMW  → up to 3840 bytes at ~40 MB/s
```

This effectively gives the host a "pull" channel at near-SPI speed for
arbitrary GPU memory reads, using the bidirectional Octal SPI bus.

### §9.5.6 New SPI Commands for Shared Memory

| Opcode | Name | Description |
|--------|------|-------------|
| `0x42` | `CMD_MEM_SMW_WRITE` | Write to Host→GPU mailbox (fast path) |
| `0x43` | `CMD_MEM_SMW_READ` | Read GPU→Host mailbox (fast path) |
| `0x44` | `CMD_MEM_STAGE_READ` | Request GPU to stage data into bulk buffer |

```cpp
#pragma pack(push, 1)

// CMD_MEM_SMW_WRITE (0x42)
struct PglCmdSmwWrite {
    uint8_t  slotIndex;     // Mailbox slot (0–15)
    uint32_t value;         // 32-bit value to write
};

// CMD_MEM_STAGE_READ (0x44)
struct PglCmdStageRead {
    uint32_t srcAddress;    // GPU memory address to read from
    uint16_t size;          // Bytes to stage (max 3840)
    uint8_t  flags;         // bit0: notify host via IRQ when ready
};

#pragma pack(pop)
```

### §9.5.7 Host-Side PglEncoder Extensions

```cpp
class PglEncoder {
    // ... existing methods ...

    // ─── Shared Memory Window ────────────────────────────────────────

    /// Write a value to a Host→GPU mailbox slot.
    void SmwWriteMailbox(uint8_t slot, uint32_t value);

    /// Read all GPU→Host mailbox slots (returns 16 × uint32_t).
    /// Uses bidirectional Octal SPI read — fast, no I2C.
    void SmwReadMailbox(uint32_t outSlots[16]);

    /// Request GPU to stage a memory region into the bulk read buffer.
    /// After completion (poll via SmwReadMailbox or wait for IRQ),
    /// call SmwReadBulk() to retrieve the data.
    void SmwStageRead(uint32_t gpuAddress, uint16_t size, bool irqNotify = true);

    /// Read the bulk staging buffer contents (up to 3840 bytes).
    void SmwReadBulk(void* dst, uint16_t size);

    /// Read the SMW sequence counter (for change detection).
    uint32_t SmwGetSequence();
};
```

### §9.5.8 Performance Comparison

| Operation | Old (I2C @ 400 kHz) | New (Bidirectional Octal SPI) | Speedup |
|-----------|---------------------|------------------------------|---------|
| Read 1 status word | ~200 µs | ~1.5 µs | **130×** |
| Read 16 mailbox slots | ~3.2 ms | ~2 µs | **1600×** |
| Read 1 KB diagnostic data | ~20 ms | ~15 µs | **1300×** |
| Read 3840-byte framebuffer stripe | ~77 ms | ~55 µs | **1400×** |

### §9.5.9 SRAM Cost

| Component | Size | Notes |
|-----------|------|-------|
| Shared Memory Window | 4 KB | Fixed reservation in SRAM |
| SMW management state | ~16 bytes | Sequence counter, flags |
| **Total** | **4.016 KB** | Deducted from Dynamic SRAM Pool |

### §9.5.10 Coherency & Safety

- **Sequence counter** prevents torn reads: host checks counter before and after
  reading a mailbox region. If counter changed during read → retry.
- **Host→GPU mailbox** is write-only from host perspective. GPU reads it at
  frame boundaries (no mid-frame race).
- **GPU→Host mailbox** is write-only from GPU perspective. GPU updates all slots
  atomically (single DMA copy), then increments sequence counter.
- **Bulk staging** uses double-flag protocol: GPU sets BULK_READY, host reads,
  host clears BULK_READY. GPU won't overwrite staging until flag is cleared.

---

## §10 Vulkan Parallels

| ProtoGL v0.7 Concept | Vulkan Equivalent |
|----------------------|-------------------|
| `PglMemTier` | `VkMemoryHeap` |
| `PglMemoryPropertyFlags` | `VkMemoryPropertyFlags` |
| `PglMemPool` | `VkDeviceMemory` (allocated block) |
| `MemPoolAlloc` | `vkAllocateMemory` (sub-allocation) |
| `MemBindResource` | `vkBindBufferMemory` / `vkBindImageMemory` |
| `MemReadRequest` | `vkMapMemory` (approximate — bidirectional SPI is async) |
| `SmwReadMailbox` | `vkMapMemory` (fast path via shared window) |
| `SmwStageRead` | `vkCmdCopyBuffer` to host-visible staging |
| `MemStreamChunk` | Staging buffer + `vkCmdCopyBuffer` |
| `MemDefrag` | None (Vulkan relies on VMA) |
| `MemQueryStats` | `vkGetDeviceMemoryCommitment` |
| `SetResourceTier` | VMA custom pools with allocation strategies |
| Shared Memory Window | `VkBuffer` with `HOST_VISIBLE` \| `HOST_COHERENT` |
| `PersistResource` | `VK_MEMORY_PROPERTY_HOST_CACHED_BIT` + explicit flush |
| `WriteFramebuffer` | `vkCmdBlitImage` to swapchain image (approximate) |

---

## §11 Implementation Plan

### Phase 1: Pool Allocator (M11, Week 28-29)
- Implement `MemPool` class on GPU side
- Wire CMD_MEM_CREATE_POOL / DESTROY_POOL / POOL_ALLOC / POOL_FREE
- I2C/SPI pool info register
- Backward-compatible with existing v0.5 alloc/free

### Phase 2: Resource Binding (M11, Week 30)
- CMD_MEM_BIND_RESOURCE parsing
- Handle indirection table in SceneState
- Test with mesh + texture binding

### Phase 3: Defragmentation (M12, Week 31-32)
- Incremental DefragStep() in idle loop
- Full compaction via CMD_MEM_DEFRAG
- Defrag status via SPI read / I2C register

### Phase 4: Streaming Upload (M12, Week 33)
- CMD_MEM_STREAM_BEGIN/CHUNK/END parsing
- Multi-frame streaming pipeline
- CRC-32 verification

### Phase 5: Budget & Eviction (M13, Week 34-35)
- Per-tier budget configuration
- LRU/LFU eviction policy
- Promotion/demotion with budget awareness

### Phase 6: Shared Memory Window (M13, Week 36)
- Reserve 4 KB SMW in SRAM
- Implement mailbox read/write on GPU side
- Bulk staging DMA copy + IRQ notification
- Host-side `SmwReadMailbox()` / `SmwStageRead()` / `SmwReadBulk()`
- Sequence counter coherency protocol

### Phase 7: Resource Persistence & Flash Writeback (M12, Week 32-33)
- Implement flash manifest region (last 512 KB of on-board flash)
- `PglFlashManifestHeader` + `PglFlashManifestEntry` read/write
- Background writeback queue (max 4 pending, 4 KB/frame incremental)
- `CMD_PERSIST_RESOURCE` / `CMD_RESTORE_RESOURCE` parser integration
- Boot-time manifest check: auto-restore persisted resources to VRAM
- MRAM auto-persist path: skip flash, record MRAM addresses in manifest
- `MEM_PERSIST_STATUS` I2C register (0x1C) + mailbox slot 9 notification
- Host-side `PersistResource()` / `RestoreResource()` / `QueryPersistence()`

### Phase 8: Direct Framebuffer Write (M12, Week 33)
- `CMD_WRITE_FRAMEBUFFER` (0x45) parser + memcpy to back buffer / layer buffer
- Bounds validation (clip to framebuffer dimensions)
- Host-side `WriteFramebuffer()` encoder method
- Integration test: host sends raw frame → GPU displays without draw calls
- Hybrid test: GPU renders 3D on Layer 0, host writes UI on Layer 1

---

## Related Documents

- [GPU_API_Design.md](GPU_API_Design.md) — §8 Tiered memory, §9 Memory access API
- [Display_Frontend_Design.md](Display_Frontend_Design.md) — Display driver interface
- [2D_Graphics_And_Compositing.md](2D_Graphics_And_Compositing.md) — 2D/multi-layer system
- [ProtoGL_API_Spec.md](ProtoGL_API_Spec.md) — Wire-format specification
