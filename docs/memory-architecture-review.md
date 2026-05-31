# ProtoTracer ESP32-S3 Memory Architecture Review

> **Date**: 2026-05-31 | **Target**: ESP32-S3 (N8R8/N8R16) | **FreeRTOS + Arduino Framework**

## 1. Memory Map Summary

| Region | Size (N8R8) | Used For |
|---|---|---|
| Internal DRAM (SRAM0+SRAM1) | ~512 KB | Stack, `.data`, `.bss`, heap (malloc/new) |
| Internal IRAM (SRAM0) | ~384 KB shared | `.text` hot code + data |
| PSRAM (Octal SPI) | 8 MB / 16 MB | Explicit `heap_caps_malloc(SPIRAM)` allocations |
| Flash | 16 MB | `.rodata`, code, LittleFS |

### Key Build Flags (from `platformio.ini`)
```ini
-D BOARD_HAS_PSRAM
-D CONFIG_SPIRAM_USE=SPIRAM_USE_MALLOC
-D CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
-D CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY
-D CONFIG_SPIRAM_FETCH_INSTRUCTIONS
-D CONFIG_SPIRAM_RODATA
-D CONFIG_SPIRAM_SPEED_120M
```

## 2. CRITICAL: `heap_caps_malloc_extmem_enable(0)` — The Double-Edged Sword

```cpp
// main.cpp line 251
heap_caps_malloc_extmem_enable(0);  // DISABLES malloc→PSRAM fallback
```

**What this does**: All `new`, `malloc`, `String`, `std::vector`, `std::string` allocations go to **internal DRAM only**. PSRAM is only used when explicitly requested via `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`.

**Why it was done**: To keep HUB75 DMA buffers and performance-critical rendering data in fast internal SRAM.

**The Problem**: This means EVERY incidental allocation — STL containers, `String` temporaries, JSON parsing objects, BLE buffers — also stays in internal DRAM, competing with the DMA and rendering data for the same precious 512 KB.

### ⚠️ This is likely the ROOT CAUSE of the freezing issue:
- Internal DRAM is being silently exhausted by accumulation of small allocations
- When internal DRAM is full, `new` returns `nullptr` → unguarded dereference → crash/freeze
- The `FreeMem()` function only measures stack-heap gap in internal DRAM, giving a misleading "OK" reading while PSRAM sits mostly unused

## 3. Internal DRAM Budget Analysis (512 KB total)

| Consumer | Estimate | Notes |
|---|---|---|
| HUB75 DMA buffers (MatrixPanel_I2S_DMA) | ~40-64 KB | 2×64×32×RGB16 or similar double-buffer |
| PixelGroup::pixelColors (2048×3B) | ~6 KB | `MALLOC_CAP_INTERNAL` preferred |
| PixelGroup::pixelBuffer (2048×3B) | ~6 KB | `MALLOC_CAP_INTERNAL` preferred |
| PixelGroup::up/down/left/right (4×2048×4B) | ~32 KB | **Uses `new` → internal only!** |
| PixelGroup::rectCoords (2048×8B) | ~16 KB | `new[]` → internal only |
| Camera::tmpX/tmpY/rotX/rotY (4×2048×4B) | ~32 KB | `MALLOC_CAP_INTERNAL` preferred |
| Camera::cachedRays (2048×8B) | ~16 KB | `new[]` → internal only |
| Scene::objectsShadow (12×8B) | ~96 B | `MALLOC_CAP_INTERNAL` preferred (minor) |
| QuadTree per-frame (Node×N + Triangle2D×M) | **10-40 KB/frame** | Allocated & freed EVERY frame |
| WiFi stack buffers | ~30-50 KB | LWIP + WiFi driver |
| BLE stack buffers | ~20-30 KB | NimBLE/BLEDevice |
| AsyncWebServer + AsyncTCP | ~20-40 KB | Event-driven, socket buffers |
| ArduinoJson (DynamicJsonDocument) | Varies | Startup: may use PSRAM; runtime: internal |
| STL containers (std::vector, std::string) | Varies | **internal only** due to extmem_enable(0) |
| FreeRTOS task stacks | ~8-16 KB | Arduino loop task + BLE task + WiFi task |
| Stack | ~8-16 KB | Main loop stack |

**Total estimate**: 280-380 KB just for identified allocations, approaching the 512 KB limit.
With fragmentation overhead (~20-30%), the effective ceiling is lower.

## 4. Memory Fragmentation Hotspots

### 4.1 QuadTree — Per-Frame Allocation/Deallocation Storm ⚠️⚠️⚠️
`Camera::Rasterize()` in `Camera.h` creates a **brand new QuadTree every frame**:
```cpp
QuadTree tree(transformedBounds);  // stack object, but...
// Inside QuadTree::Insert():
entities = (Triangle2D*)realloc(entities, newCapacity * sizeof(Triangle2D));
// Inside Node::Subdivide():
childNodes = new Node[4];           // heap allocation
entities = new Triangle2D*[newCount]; // heap allocation
// Inside Node::Expand():
entities = new Triangle2D*[newCount]; // heap allocation
delete[] tmp;                         // heap deallocation
```

This causes **dozens of heap alloc/free cycles per frame**. On ESP32-S3 with its simple heap implementation (TLSF or similar), this rapidly fragments internal DRAM. After minutes/hours of operation, allocation failures become likely.

### 4.2 STL Container Growth
`std::vector<String>`, `std::string`, and Arduino `String` objects used in the animation and BLE code all allocate from internal DRAM. Each `push_back`, `reserve`, or concatenation can trigger reallocation.

### 4.3 JSON Parsing During Runtime
`DynamicJsonDocument` (used in BLE handler and animation config loading) internally uses `malloc`/`realloc`. Even though some JSON docs use PSRAM allocators, the BLE JSON path and the expression loading path use default allocators.

## 5. Misleading FreeMem() Implementation

```cpp
float FreeMem() {
    uint32_t stackT = (uint32_t)&stackT;
    void *heapPos = malloc(1);
    uint32_t heapT = (uint32_t)heapPos;
    free(heapPos);
    return (stackT - heapT) / 1000000.0f;
}
```

This measures the gap between stack and the **last malloc'd address** in internal DRAM. It does NOT:
- Account for PSRAM usage
- Detect fragmentation (many small free blocks scattered throughout)
- Report total free internal DRAM

On a fragmented heap, `malloc(1)` may succeed (returning a block from the top) while a `malloc(32KB)` would fail — but `FreeMem()` would still report "OK".

**Recommendation**: Replace with `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` and `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)`.

## 6. Critical Questions for the Team

1. **Q: What is the exact failure mode?** Does the firmware watchdog-reset, hard-fault, or just freeze with the panel displaying the last frame? A crash dump from `esp32_exception_decoder` would be invaluable.

2. **Q: How long does it run before freezing?** Minutes? Hours? This would help distinguish between slow fragmentation vs. a fixed memory exhaustion.

3. **Q: What size is `universal_face.json`?** This determines the JSON doc capacity needed. Larger files (>200KB) need PSRAM allocators to avoid internal DRAM exhaustion during loading.

4. **Q: Are there any serial logs before the freeze?** If `PRINTINFO` is enabled, what does `FreeMem()` report just before freeze?

5. **Q: Is the ESP32-S3 the N8R8 (8MB PSRAM) or N8R16 (16MB PSRAM) variant?** The board JSON name says N8R16 but platformio.ini uses N8R8.

6. **Q: Does the freeze correlate with BLE activity?** (e.g., when a phone connects/disconnects, or during manifest transfer)

7. **Q: Has `CONFIG_SPIRAM_USE=SPIRAM_USE_MALLOC` been verified to work correctly at runtime?** Sometimes the psram `malloc` integration fails silently on certain ESP32-S3 silicon revisions.

## 7. Immediate Diagnostic Steps

1. **Add memory telemetry** to the main loop:
```cpp
Serial.printf("Free internal: %u, largest block: %u, free PSRAM: %u\n",
    heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
    heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
    heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
```

2. **Check for null pointer returns** after every `new`/`malloc` — add assertions or logging.

3. **Enable the ESP-IDF heap corruption detector**:
```ini
build_flags = -D CONFIG_HEAP_POISONING_COMPREHENSIVE
```

4. **Monitor fragmentation**: Track `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)` over time. If it drops below ~40KB, the next QuadTree allocation will fail.
