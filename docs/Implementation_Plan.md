# Transition Plan & Roadmap: Dual-MCU Refactor

> **⚠ This document is the original Phase 1 transition plan, updated for Phase 2.**
> For the current day-by-day milestone schedule (M0–M13), risk register, and
> audit trail, see **[Project_Schedule.md](Project_Schedule.md)**.
>
> The phases below have been fully incorporated into the milestone schedule:
>
> | Original Phase | Milestone(s) | Status |
> |---|---|---|
> | Phase 1: Planning and Research | M0 (Toolchain & Skeleton) | ✅ Complete |
> | Phase 2: ESP32-S3 Firmware Changes | M0, M2 (Host Integration) | ✅ Complete |
> | Phase 3: RP2350 Base Station Code | M1 (RP2350 Core Bring-up) | ✅ Complete |
> | Phase 4: PIO HUB75 Driver | M1 (HUB75 PIO + BCM) | ✅ Complete |
> | Phase 5: Debug & Profile | M2 (Hardware Validation) | ✅ Complete |
> | Phase 6: Scene Math & Rasterizer | M3–M6 | ✅ Complete |
> | Phase 7: Animation & Memory | M7–M8 | ✅ Complete |
> | Phase 8: Shaders & Diagnostics | M9–M10a | ✅ Complete |
> | **Phase 9: Display + Pools** | **M11** | 🔧 Planned |
> | **Phase 10: DVI + 2D + Defrag** | **M12** | 🔧 Planned |
> | **Phase 11: Multi-Display + Compositing + Streaming** | **M13** | 🔧 Planned |
>
> Phase 1 milestones M0–M10a are code complete. Phase 2 milestones M11–M13
> extend the system with display abstraction, 2D graphics, and refined memory management.

---

## Phase 1: Planning and Research
1. Identify memory requirements for RP2350. *Verdict: 520KB SRAM is plenty for Double Framebuffer + Z-Buffer + Local Scene Nodes.*
2. Shift strictly to **Geometry Sync (Command Buffer API)**. Pushing framebuffers is wasteful when we can treat the RP2350 as a discrete GPU to bypass ESP32 PSRAM latency.

## Phase 2: Firmware Scaffolding (ESP32-S3 Changes)
1. **Remove Local Panel Drivers:** Gradually deprecate `SmartMatrixHUB75.h`, I2S drivers, and `SmartMatrix` dependencies from the ESP32-S3 target.
2. **Implement Vulkan-like Controller:** Create a new class `GPUDriverController.h` that acts as the Command Buffer encoder. Instead of rasterizing on the ESP, it packs `Transform`, `Material`, and `Camera` data into byte arrays and sends them via Octal SPI DMA.
3. **Setup I2C Master:** Introduce a configuration daemon that acts on Boot and state changes to dispatch `Wire` (I2C) commands addressing `0x3C` (The RP2350).

## Phase 3: Create The RP2350 Base Station Code
1. **New Repository:** Create a new branch/repo explicitly for the RP2350 firmware using Pico-SDK (as Arduino Core for RP2350 might be limiting for pure PIO implementations).
2. **I2C Slave Node:** Initialize hardware I2C in slave mode to parse the register map.
3. **Octal SPI Receiver:** Configure a DMA + PIO machine specifically tailored to absorb 8-bit parallel data clocked from the ESP32-S3 continuously into a local array.

## Phase 4: RP2350 PIO HUB75 Driver
1. Adapt [Pico-PIO-HUB75](https://github.com/Wren6991/Pico-PIO-Hub75) or a similar library to push the ingested framebuffer payload to the HUB75 Matrix out hardware pins.
2. The PIO block makes this operation entirely background-driven.
3. Hook up the PIO buffers to the incoming SPI buffers over a simple Double Buffer.

## Phase 5: Debug & Profile
1. **Visual Testing:** Look for frame tearing. Implement a simple hardware-level handshaking pin (e.g. `VSYNC` or `RDY`) so the ESP32-S3 doesn't overwrite an actively scanning frame on the RP2350.
2. **Framerate checking:** Read the `0x0A Status Request` via I2C to see if RP2350 is dropping incoming frames.
3. Tune Octal SPI MHz parameters to safely remain under signal-bounce limits.

---

## Phase 9: Display Abstraction & Memory Pools (M11)

> Design docs: [Display_Frontend_Design.md](Display_Frontend_Design.md),
> [Memory_Management_API.md](Memory_Management_API.md)

### 9.1 Display Driver Interface

1. **Extract `DisplayDriver` base class** from existing `hub75_driver.cpp`:
   - Abstract methods: `Init()`, `WritePixel()`, `SwapBuffers()`, `SetBrightness()`, `GetTimingInfo()`
   - Common framebuffer format: RGB565 input, driver-specific conversion in `SwapBuffers()`
   - `DisplayCaps` struct: resolution, color depth, PIO/DMA usage, feature flags

2. **Refactor `hub75_driver` to implement `DisplayDriver`:**
   - Move all PIO0 / DMA configuration into `Hub75Driver::Init()`
   - Move BCM bitplane conversion into `Hub75Driver::SwapBuffers()`
   - Preserve existing PIO program and timing

3. **Create `SpiLcdDriver` as second driver:**
   - Hardware SPI + DMA for common SPI displays (ST7789, ILI9341, SSD1351)
   - RGB565 pass-through (no format conversion needed)
   - Implements partial-update region (`CMD_DISPLAY_SET_REGION`) for efficient writes
   - Good first test of the display abstraction — SPI LCD is much simpler than HUB75

4. **Implement `DisplayManager` singleton:**
   - Runtime driver selection via I2C `DISPLAY_MODE` register (0x15)
   - PIO allocation validation (prevent conflicting drivers)
   - Single-display routing for M11 (multi-display in M13)

### 9.2 Memory Pools

1. **Implement `MemPool` allocator** within `mem_tier.cpp`:
   - Fixed-size block pool backed by a contiguous allocation from the tier's free-list
   - Singly-linked free list for O(1) alloc/free
   - Pool metadata: block size, count, free count, first-free pointer

2. **Add pool commands to command parser:**
   - `CMD_MEM_POOL_CREATE` (0x38), `CMD_MEM_POOL_ALLOC` (0x39),
     `CMD_MEM_POOL_FREE` (0x3A), `CMD_MEM_POOL_DESTROY` (0x3B)

3. **Add I2C diagnostic register** `MEM_POOL_STATUS` (0x18)

4. **Host-side `PglEncoder` methods:**
   - `MemPoolCreate()`, `MemPoolAlloc()`, `MemPoolFree()`, `MemPoolDestroy()`

### 9.3 Exit Criteria

- HUB75 output unchanged (regression test)
- SPI LCD driver renders test pattern at ≥30 FPS
- Memory pool alloc/free cycle 1000× with zero fragmentation
- `CMD_DISPLAY_CONFIGURE` and `CMD_MEM_POOL_CREATE` round-trip functional

---

## Phase 10: DVI-D + 2D Primitives + Defragmentation (M12)

> Design docs: [Display_Frontend_Design.md](Display_Frontend_Design.md),
> [2D_Graphics_And_Compositing.md](2D_Graphics_And_Compositing.md),
> [Memory_Management_API.md](Memory_Management_API.md)

### 10.1 DVI-D Display Driver

1. **Implement `DviDriver`** using PIO TMDS encoding:
   - 3 PIO state machines for R/G/B TMDS channels
   - 3 DMA channels for continuous scanline output
   - RGB565 → RGB888 → TMDS 10b/8b conversion in `SwapBuffers()`
   - PIO program based on Pico DVI / PicoDVI signal generation

2. **Create `QspiLcdDriver`** for QSPI displays:
   - PIO-driven 4-bit data bus
   - Shares PIO0 allocation slot with HUB75/DVI (mutually exclusive)

### 10.2 2D Drawing Primitives

1. **Create `Rasterizer2D` namespace** in `render/rasterizer_2d.cpp`:
   - `DrawRect()`: Filled/outlined rectangles
   - `DrawLine()`: Bresenham line rasterization
   - `DrawCircle()`: Midpoint circle algorithm
   - `DrawRoundedRect()`: Rectangle + quarter-circle corners
   - `DrawTriangle2D()`: Barycentric fill for 2D triangles
   - `DrawArc()`: Incremental arc rasterization
   - All functions write directly to a layer's framebuffer (not the 3D framebuffer)

2. **Implement single-layer 2D command parsing:**
   - Add opcodes 0xA0–0xA5, 0xA9, 0xAA–0xAC to command parser
   - Layer 0 reserved for 3D scene; Layer 1 available for 2D
   - Simple over-compositing after 3D rasterization

### 10.3 Defragmentation

1. **Implement `Defragment()` in `mem_tier.cpp`:**
   - Walk the allocation table, find gaps, move allocations down to compact
   - Incremental mode: limit bytes moved per frame via `maxMoveKB`
   - Update all resource pointers after relocation (indirection table)
   - Urgent mode: block frame pipeline, complete in one pass

2. **Add `CMD_MEM_DEFRAG` (0x3C)** to command parser
3. **Add `MEM_DEFRAG_STATUS` (0x19)** I2C register

### 10.4 Resource Persistence & Flash Writeback

1. **Implement flash manifest region** at end of GPU on-board flash:
   - `PglFlashManifestHeader` + `PglFlashManifestEntry` array (max 64 entries)
   - Manifest read/write with CRC-32 integrity check
   - Reserve last 512 KB of 4 MB flash for persisted resource data

2. **Implement background writeback queue:**
   - Queue up to 4 pending persist requests
   - Incremental write: 4 KB SRAM staging buffer → flash page-program, one chunk per frame
   - DMA copy from PSRAM → staging buffer after `EndFrame`
   - Update manifest entry on completion, notify host via mailbox slot 9 + IRQ

3. **Add persistence commands to command parser:**
   - `CMD_PERSIST_RESOURCE` (0x46): Queue async writeback (PSRAM) or acknowledge (MRAM)
   - `CMD_RESTORE_RESOURCE` (0x47): Load from flash manifest to VRAM
   - `CMD_QUERY_PERSISTENCE` (0x48): Query per-resource or manifest status
   - `MEM_PERSIST_STATUS` (0x1C) I2C register

4. **Implement boot-time auto-restore:**
   - GPU checks flash manifest at boot (after VRAM detection)
   - If valid entries found and VRAM is volatile PSRAM: DMA-load from flash → PSRAM
   - Rebuild resource table entries for restored resources
   - Report `persistedResourceCount` in capability response

5. **MRAM persistence path:**
   - If detected VRAM is MRAM: store manifest at MRAM offset 0x0000
   - `CMD_PERSIST_RESOURCE` returns `ALREADY_PERSISTENT` immediately
   - Boot: rebuild resource table from MRAM manifest (no DMA copy needed)
   - Set `PGL_CAP_NVRAM_VRAM` capability flag

6. **Host-side encoder methods:**
   - `PersistResource()`, `RestoreResource()`, `QueryPersistence()`, `ErasePersisted()`

### 10.5 Direct Framebuffer Write

1. **Implement `CMD_WRITE_FRAMEBUFFER` (0x45)** in command parser:
   - Parse x, y, w, h, layerId, pixel data from SPI command stream
   - `memcpy` RGB565 pixel data into back buffer (layerId=0xFF) or layer buffer (0–7)
   - Clip to framebuffer bounds (no error on out-of-bounds)
   - Multiple writes per frame allowed

2. **Host-side encoder method:**
   - `WriteFramebuffer(x, y, w, h, data, layerId)` in `PglEncoder`
   - Full-frame convenience: `WriteFullFrame(const void* pixels)` sends 128×64 RGB565

3. **Integration:**
   - Direct writes execute during command parsing phase (before rasterizer runs)
   - If both `DrawObject` and `WriteFramebuffer` are in the same frame, GPU renders
     3D first, then the direct-write regions overwrite the corresponding pixels
   - When targeting a compositing layer, the compositor blends normally at `EndFrame`

### 10.6 Exit Criteria

- DVI-D output at 640×480 @ 60 Hz on PicoVision or custom board
- 2D rectangles, lines, circles render correctly on Layer 1
- Layer compositing: 3D scene + 2D overlay renders correctly
- Defrag reduces fragmentation by ≥50% on a synthetic workload
- No 3D performance regression (rasterizer unaffected by 2D additions)
- Flash manifest CRC integrity verified across power cycles
- 256 KB resource persists to flash in ~64 frames and restores on boot
- MRAM path: `PGL_CAP_NVRAM_VRAM` set, `CMD_PERSIST_RESOURCE` returns `ALREADY_PERSISTENT`
- `CMD_WRITE_FRAMEBUFFER` full-frame write displays correctly without draw calls
- Hybrid mode: GPU 3D on Layer 0 + host-written UI on Layer 1 composites correctly

---

## Phase 11: Multi-Display + Full Compositing + Streaming (M13)

> Design docs: All three new design docs

### 11.1 Multi-Display Support

1. **Extend `DisplayManager` for simultaneous drivers:**
   - Up to 4 active displays (within PIO/DMA resource limits)
   - Per-display framebuffer routing: each display can mirror or show independent content
   - `CMD_DISPLAY_SYNC` (0x92) for synchronized frame swap across displays
   - `MULTI_DISPLAY_ROUTE` I2C register (0x17) for viewport configuration

2. **Practical combinations:**
   - HUB75 (main) + SPI LCD (status display)
   - DVI-D (main) + SPI LCD (debug output)
   - 2× SPI LCD (split-face protogen)

### 11.2 Full Multi-Layer Compositing

1. **Extend to 8 layers** with full GPU-side compositor:
   - Per-layer: opacity (0–255), blend mode, viewport offset, clip rectangle
   - Z-order sorting for compositing order
   - Sprite engine with batched rendering (`CMD_DRAW_SPRITE_BATCH`)
   - Text rendering with font atlas textures (`CMD_DRAW_TEXT`)
   - Billboard sprites projected through 3D camera (`CMD_BILLBOARD_SPRITE`)

2. **Memory management for layers:**
   - Each 128×64 RGB565 layer = 16 KB
   - SRAM budget limits: 4 layers in SRAM, additional layers in external QSPI VRAM
   - Automatic tier placement for layer buffers

### 11.3 Streaming Upload

1. **Implement streaming state machine** in command parser:
   - Track up to 4 concurrent streams
   - `CMD_STREAM_BEGIN` allocates staging buffer in target tier
   - `CMD_STREAM_DATA` writes chunks (out-of-order allowed via offset field)
   - `CMD_STREAM_COMMIT` binds fully-uploaded data to resource handle

2. **Resource binding model:**
   - `CMD_MEM_BIND_RESOURCE` (0x40) / `CMD_MEM_UNBIND_RESOURCE` (0x41)
   - Separate allocation lifetime from resource lifetime
   - Enables texture atlas reuse, sprite sheet swapping without reallocation

3. **I2C diagnostic registers:**
   - `MEM_STREAM_STATUS` (0x1A): per-stream progress
   - `MEM_BINDING_TABLE` (0x1B): active bindings (paginated)

### 11.4 Exit Criteria

- 2 simultaneous displays render independently at ≥30 FPS each
- 4-layer compositing (3D + 3 UI layers) at ≥45 FPS on 128×64
- Streaming upload of 64 KB texture completes across 4 frames with correct result
- Resource bind/unbind cycle preserves memory allocation across resource destroy/recreate
- Full integration test: 3D scene + 2D HUD + sprite overlay + I2C OLED status display

---

## Rollout Summary

| Phase | Milestone | Key Deliverables | Dependencies |
|-------|-----------|-----------------|--------------|
| 9 | M11 | DisplayDriver ABC, Hub75Driver refactor, SpiLcdDriver, DisplayManager, MemPool allocator | M0–M10a complete |
| 10 | M12 | DviDriver, QspiLcdDriver, Rasterizer2D (6 primitives), single-layer compositing, MemDefrag, **flash persistence/writeback**, **direct framebuffer write** | M11 |
| 11 | M13 | Multi-display routing, 8-layer compositor, sprite batcher, text renderer, streaming upload, resource binding | M12 |

**Estimated timeline:** 9–12 weeks (3–4 weeks per phase). See [Project_Schedule.md](Project_Schedule.md) for
the detailed week-by-week schedule.

---

## Related Documents

- [Architecture_Split.md](Architecture_Split.md) — High-level ESP32↔GPU split
- [GPU_API_Design.md](GPU_API_Design.md) — Detailed GPU architecture and API design
- [ProtoGL_API_Spec.md](ProtoGL_API_Spec.md) — Full wire-format specification
- [Display_Frontend_Design.md](Display_Frontend_Design.md) — Unified display driver interface
- [2D_Graphics_And_Compositing.md](2D_Graphics_And_Compositing.md) — Multi-layer 2D graphics
- [Memory_Management_API.md](Memory_Management_API.md) — Refined memory management
- [Project_Schedule.md](Project_Schedule.md) — Week-by-week schedule with exit criteria
- [Shader_System_Design.md](Shader_System_Design.md) — PGLSL compiler and shader VM