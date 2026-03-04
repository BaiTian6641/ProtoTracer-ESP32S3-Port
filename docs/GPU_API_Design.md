# GPU Architecture Design (Vulkan-like API)

## 1. The Core Motivation
Currently, on the ESP32-S3, rendering and scene processing heavily utilize external **PSRAM**. While PSRAM offers megabytes of capacity, it suffers from severe cache-miss penalties during non-sequential access patterns. 
The QuadTree traversal, Raycasting, and Z-buffer checks in `Camera::Rasterize` are highly random memory accesses. Combined with the interrupt overhead of driving the HUB75 matrix via I2S/LCD peripherals, this drags the ESP32-S3 down to ~20 FPS.

**The Solution:** Offload rendering to a dedicated GPU core.
A GPU with tightly-coupled internal SRAM (single-cycle access) eliminates the PSRAM bottleneck. By keeping the active Scene, Bounding Boxes, and Framebuffers entirely within fast on-chip SRAM, memory latency drops to near zero.

### 1.1 Phase 1 GPU: RP2350
Phase 1 targets the **RP2350** as the concrete GPU implementation:

| Property | Value |
|---|---|
| Cores | 2 × ARM Cortex-M33 @ 150 MHz (dynamically adjustable 150–300 MHz, see §10.4) |
| SRAM | 520 KB tightly-coupled (single-cycle access) |
| FPU | Single-precision hardware FPU per core |
| Display output | PIO-driven HUB75 (zero CPU overhead) |
| Data ingress | PIO-driven Octal SPI receiver (8-bit parallel) |
| Config bus | Hardware I2C slave at 0x3C |
| Flow control | GPIO RDY pin |

### 1.2 Architecture-Agnostic API
The ProtoGL wire protocol is **architecture-agnostic**. Phase 2 can retarget other GPU implementations without host-side changes:

| Architecture | Example | SRAM | FPU | Unaligned Access | Phase |
|---|---|---|---|---|---|
| ARM Cortex-M33 | RP2350 (ARM mode) | 520 KB | Yes (SP) | Yes (native) | **Phase 1** |
| RISC-V Hazard3 | RP2350 (RISC-V mode) | 520 KB | Yes (SP) | Optional | Phase 2 |
| Custom RISC-V | User SoC | Varies | Optional | Unlikely | Phase 2 |
| FPGA (soft-core) | Lattice/Xilinx | Block RAM | Optional | N/A | Phase 2 |
| ARM Cortex-M7 | STM32H7, i.MX RT | 1 MB+ | Yes (DP) | Yes | Phase 2 |

The GPU reports its architecture and capabilities to the host via the I2C capability query
(register 0x09, `PglCapabilityResponse`).

## 2. Memory Economics (Phase 1: RP2350, 520 KB SRAM)
To render at 60+ FPS on a typical 128x64 display, a GPU needs sufficient on-chip SRAM:
- **Framebuffer (RGB565, Double Buffered):** 128 * 64 * 2 bytes * 2 buffers = ~32 KB
- **Z-Buffer (Float):** 128 * 64 * 4 bytes = ~32 KB
- **QuadTree/Node Cache:** ~100 KB
- **Geometry/Material Vertices (Command Buffer):** ~100 KB
- **Total requirement:** ~264 KB (On RP2350, leaves ~50% of the 520 KB SRAM free for Pico-SDK, PIO state, and stack. Phase 2 GPUs with different SRAM sizes can reduce `PGL_MAX_*` limits accordingly).

## 3. The Command Buffer API ("ProtoGL")
Instead of pushing raw pixels over Octal SPI, the host MCU acts as the "CPU/Game Engine", and the GPU core acts as the "GPU". They communicate via a serialized **Command Buffer** over a high-speed data bus. The protocol is architecture-agnostic — the GPU can be ARM, RISC-V, FPGA, or any other implementation.

### Render Pass Workflow:
1. **BeginFrame:** Host signals a new frame starts.
2. **PushMaterials:** Host sends updated color/material/image data.
3. **PushGeometry:** Host sends local vertex buffers (`Mesh` data), if changed.
4. **PushTransforms (DrawCalls):** Host sends a stream of transforms mapping to geometry IDs. *"Draw Mesh 1 at Transform X"*
5. **SetCamera:** Host sends the View/Projection quaternion vectors.
6. **EndFrame / Present:** Host triggers the GPU to traverse the QuadTree, Rasterize into its local framebuffers, and swap the display output buffer.

### Payload Structure (Example)
```c
struct CommandHeader {
    uint8_t opcode; // CMD_DRAW_INDEXED, CMD_UPDATE_MATERIAL, CMD_SET_CAMERA
    uint16_t length;
};

// CMD_DRAW_INDEXED
struct DrawCmd {
    uint16_t mesh_id;
    uint16_t material_id;
    Vector3D position;
    Quaternion rotation;
    Vector3D scale;
};
```

## 4. Workload Separation

### ESP32-S3 ("The CPU" / Host)
- Wi-Fi, OTA, WebSockets.
- File System operations (mklittlefs, JSON parsing).
- Animation state machines, calculating the `Transform` of objects.
- **Vulkan-like Driver:** Serializing the state of the scene into a ProtoGL command buffer and transmitting it asynchronously over the data bus. 
- *ESP32 Memory constraint resolved:* PSRAM is now perfectly fine here because Animation tracks and JSON are read sequentially.

### GPU Core ("The GPU" — Phase 1: RP2350)
The GPU core executes the rasterization pipeline. Phase 1 uses the RP2350 dual-core Cortex-M33 at 150 MHz with 520 KB SRAM. Phase 2 can retarget any core with sufficient SRAM and an FPU.

To extract maximum performance, the RP2350's dual cores use a symmetric screen-space split via the Multicore FIFO IPC:

**Strategy 1: Asymmetric Pipeline (Producer/Consumer)**
* **Core 0 (Frontend Engine):** Handles Octal SPI DMA ring buffers, parses the incoming ProtoGL Command Buffer, updates local Matrix structs, and generates the active `QuadTree` for the frame.
* **Core 1 (Backend Rasterizer):** Once Core 0 builds the QuadTree, Core 1 traverses the tree, loops over all screen pixels, calculates ray-intersections, and paints final RGB pixels into the Framebuffer.
* **Pros:** Clean separation of concerns.

**Strategy 2: Symmetric Screen-Space Rasterization (Chosen for Phase 1)**
Since Raycasting/Rasterization on a pixel-by-pixel basis is an *embarrassingly parallel* problem, we can load-balance the RP2350's two cores spatially:
* **Core 0 (Master):** Parses Command Buffer and builds the `QuadTree`.
* *Synchronization:* Core 0 sends a `multicore_fifo_push_blocking(START_RENDER)` to Core 1.
* **Core 0 (Raster-Top):** Rasterizes the top half of the screen (Y = 0 to 31).
* **Core 1 (Raster-Bottom):** Rasterizes the bottom half of the screen (Y = 32 to 63).
* *Synchronization:* Core 1 pushes `RENDER_DONE` on completion. Core 0 waits, then swaps the double-buffered framebuffer pointer for PIO HUB75 DMA.
* **Pros:** Double the compute power applied directly to the heaviest bottleneck (`Camera::Rasterize`).

### PIO / HSTX / Display Output (Phase 1: RP2350 PIO)
On the RP2350, hardware PIO blocks automatically consume the finalized framebuffer and drive the HUB75 pins with zero CPU overhead. The RP2350 has 3 PIO blocks × 4 state machines = 12 total; HUB75 uses 1 SM + 2 DMA channels, Octal SPI uses 1 SM + 1 DMA channel, leaving ample headroom.

Phase 2 GPU implementations may use:
- **SPI LCD** for small displays
- **Custom I/O logic** on FPGA
- **DMA-to-GPIO** on RISC-V with dedicated peripherals

The key principle: display scan-out should be hardware-driven to keep 100% of CPU time available for rasterization.

## 5. Expected Performance Gains
1. **Memory Latency Abolished:** QuadTree and Raster math move from PSRAM (host) to fast on-chip SRAM (GPU). Estimated **40-60% rendering speedup**.
2. **Interrupt Unblocking:** ESP32-S3 spends up to 20% of its CPU time servicing HUB75 I2S/LCD interrupts. GPU handles display output at the silicon level.
3. **Parallelism:** The host can calculate Frame N+1's physics & animations while the GPU is actively rendering Frame N.

**Phase 1 performance target (RP2350 GPU):** From ~20 FPS to **60+ FPS** at peak visual load.

## 6. Post-Processing Pipeline (General Shader System)

ProtoTracer includes a rich set of screen-space post-processing effects (in `src/Screenspace/`).
ProtoGL generalises these into a **shader class system**: instead of hardcoded effect types,
the API exposes three general *shader classes* — CONVOLUTION, DISPLACEMENT, COLOR_ADJUST —
each parameterised to express many effects (including all original ProtoTracer effects)
without future API changes.

### 6.1 Shader Class Architecture

Each shader slot stores:
- `shaderClass` — which general class to execute
- `intensity` (0.0–1.0) — global mix factor
- `params[20]` — class-specific parameter struct

The three classes cover all screen-space operations:

| Class | Enum | Purpose | Subsumes ProtoTracer Effects |
|---|---|---|---|
| CONVOLUTION | `0x01` | Configurable blur/smooth kernel | HorizontalBlur, VerticalBlur, RadialBlur, AntiAliasing, GaussianBlur |
| DISPLACEMENT | `0x02` | Coordinate-space warp / chromatic split | PhaseOffsetX, PhaseOffsetY, PhaseOffsetR |
| COLOR_ADJUST | `0x03` | Per-pixel colour transform | EdgeFeather, + new: Brightness, Contrast, Gamma, Threshold, Invert, EdgeDetect |

### 6.2 Shader Class Parameters

#### CONVOLUTION (`PglShaderParamsConvolution`, 16 bytes)

| Field | Type | Description |
|---|---|---|
| `kernelShape` | `uint8_t` | `PglKernelShape`: BOX (0), GAUSSIAN (1), TRIANGLE (2) |
| `radius` | `uint8_t` | Kernel half-width in pixels (1–32) |
| `separable` | `uint8_t` | 0 = 1D directional, 1 = 2D separable (4-neighbour) |
| `angle` | `float` | Direction angle in degrees (0° = horizontal, 90° = vertical) |
| `anglePeriod` | `float` | If > 0, angle auto-rotates with this period (seconds) |
| `sigma` | `float` | Gaussian σ, or smoothing weight for separable mode |

**How it maps to ProtoTracer effects:**
- `angle=0, separable=0` → Horizontal Blur
- `angle=90, separable=0` → Vertical Blur
- `anglePeriod>0, separable=0` → Radial Blur (auto-rotating direction)
- `separable=1, radius=1` → Anti-Aliasing (4-neighbour)
- `angle=45` → Diagonal Blur (new — not in ProtoTracer)
- `kernelShape=GAUSSIAN` → Gaussian Blur (new)

#### DISPLACEMENT (`PglShaderParamsDisplacement`, 20 bytes)

| Field | Type | Description |
|---|---|---|
| `axis` | `uint8_t` | `PglDisplacementAxis`: X (0), Y (1), RADIAL (2) |
| `perChannel` | `uint8_t` | 0 = uniform, 1 = chromatic R/G/B split (120° apart) |
| `amplitude` | `uint8_t` | Max displacement in pixels (1–32) |
| `waveform` | `uint8_t` | `PglWaveform`: SAWTOOTH (0), SINE (1), TRIANGLE (2), SQUARE (3) |
| `period` | `float` | Primary oscillator period (seconds), 0 = static |
| `frequency` | `float` | Spatial frequency multiplier (default 1.0) |
| `phase1Period` | `float` | Secondary oscillator (radial mode) |
| `phase2Period` | `float` | Tertiary oscillator (radial mode) |

**How it maps to ProtoTracer effects:**
- `axis=X, perChannel=1, waveform=SINE` → PhaseOffsetX
- `axis=Y, perChannel=1, waveform=SINE` → PhaseOffsetY
- `axis=RADIAL, perChannel=1` → PhaseOffsetR
- `perChannel=0` → Uniform displacement (new — wave distortion)
- `waveform=TRIANGLE/SQUARE` → New distortion types

#### COLOR_ADJUST (`PglShaderParamsColorAdjust`, 12 bytes)

| Field | Type | Description |
|---|---|---|
| `operation` | `uint8_t` | `PglColorAdjustOp` (see below) |
| `strength` | `float` | Primary control (meaning varies per operation) |
| `param2` | `float` | Secondary control (gamma exponent, etc.) |

Operations:

| # | Operation | `strength` meaning | `param2` meaning |
|---|---|---|---|
| 0x00 | EDGE_FEATHER | Dim factor (0=transparent, 1=no dimming) | — |
| 0x01 | THRESHOLD | Luminance threshold (0.0–1.0) | — |
| 0x02 | GAMMA | — | Gamma exponent (default 2.2) |
| 0x03 | INVERT | — | — |
| 0x04 | BRIGHTNESS | Delta (-1.0 to +1.0) | — |
| 0x05 | CONTRAST | Scale (0=grey, 1=unchanged, 2=double) | — |
| 0x06 | EDGE_DETECT | Sobel magnitude scale | — |

### 6.3 GPU-Side Implementation Strategy

On the GPU, shaders are significantly simpler than on the host because the framebuffer
is a regular rectangular grid (not an arbitrary `IPixelGroup`):

- **Neighbor access:** `left = index - 1`, `right = index + 1`, `up = index - width`,
  `down = index + width`. Boundary checks are simple range comparisons.
- **Scratch buffer:** The Z-buffer (32 KB of float) is repurposed as a scratch buffer
  after rasterization completes. For 128×64 RGB565, only 16 KB is needed — well within
  the Z-buffer's 32 KB.
- **Time-based animation:** Animated shaders (convolution with `anglePeriod`, displacement)
  use a general `Oscillate()` function with configurable waveform (sawtooth, sine,
  triangle, square) driven by accumulated wall time from `CMD_BEGIN_FRAME::frameTimeUs`.
- **RGB565 precision:** Shaders operate in 5-6-5 colour space (same as framebuffer)
  to avoid RGB888↔RGB565 conversion overhead.
- **Kernel weights:** CONVOLUTION supports BOX (equal weight), GAUSSIAN (exp(-d²/2σ²)),
  and TRIANGLE (linearly decreasing) kernels — all computed inline without lookup tables.

### 6.4 Shader Pipeline Placement

```
 ┌─────────────────────────────────────────────────────────────┐
 │                  GPU Frame Pipeline                         │
 │                                                             │
 │  1. Parse commands (Core 0)                                 │
 │  2. Transform + Project + QuadTree (Core 0)                 │
 │  3. Rasterize top half (Core 0) + bottom half (Core 1)     │
 │  4. ──── Screen-Space Shaders (Core 0, single-threaded) ── │
 │        ├─ Repurpose Z-buffer as scratch                     │
 │        ├─ For each camera:                                  │
 │        │    For each shader slot (0..3):                    │
 │        │      ApplyShader(backBuffer, scratch, params)      │
 │        └─ (typically < 0.5 ms for 128×64)                   │
 │  5. Swap framebuffers                                       │
 │  6. HUB75 scans out front buffer (PIO, zero CPU)            │
 └─────────────────────────────────────────────────────────────┘
```

Shaders are applied on Core 0 only (single-threaded) because:
- Shaders read from the entire framebuffer (not just a Y band)
- The framebuffer is small (8K pixels) — single-core is sufficient
- Avoids complex synchronization between cores during post-processing

### 6.5 Shader Chaining

Up to `PGL_MAX_SHADERS_PER_CAMERA` (4) shaders can be active per camera simultaneously.
The host sets each shader slot via `CMD_SET_SHADER` with a `shaderSlot` index (0–3).
Shaders are applied in slot order: slot 0 runs first, then slot 1, etc.

Typical usage:
- Slot 0: CONVOLUTION (separable, radius=1, sigma=0.25) — always-on anti-aliasing
- Slot 1: CONVOLUTION (angle=0, radius=4) — horizontal blur for expression transitions
- Slot 2: COLOR_ADJUST (EDGE_FEATHER, strength=0.5) — soften edges

### 6.6 Performance Budget

| Shader | Est. Cost (128×64, CM33 @ 150 MHz) | Notes |
|---|---|---|
| CONVOLUTION, horizontal (radius 4) | ~0.1 ms | 1D kernel, fast path |
| CONVOLUTION, vertical (radius 4) | ~0.1 ms | Column-major, slightly slower |
| CONVOLUTION, separable (AA) | ~0.08 ms | Fixed 4-neighbour, no loop |
| CONVOLUTION, angled (radius 4) | ~0.2 ms | sqrtf() + sinf()/cosf() per pixel |
| DISPLACEMENT, axis X/Y | ~0.15 ms | Per-pixel sinf() call |
| DISPLACEMENT, radial | ~0.3 ms | Per-pixel dual sinf()+cosf() |
| COLOR_ADJUST, edge feather | ~0.05 ms | In-place, skips black pixels |
| COLOR_ADJUST, brightness/contrast | ~0.03 ms | Simple per-pixel multiply/add |
| COLOR_ADJUST, gamma | ~0.12 ms | Per-pixel powf() |
| COLOR_ADJUST, Sobel edge detect | ~0.1 ms | 3×3 neighbourhood, scratch |
| **Worst case (4 shaders)** | **< 1.0 ms** | Well within 16.6 ms budget |

## 7. RISC-V GPU Considerations (Phase 2)

When implementing ProtoGL on a RISC-V core in Phase 2 (RP2350 Hazard3 or custom):

1. **Unaligned access:** RISC-V base ISA does not guarantee unaligned loads/stores.
   Use `PglParser.h` (memcpy-based deserialization) unless the core has the `Zicclsm`
   extension. The wire format does NOT change — only the parsing method differs.

2. **FPU:** ProtoGL uses IEEE 754 single-precision floats extensively. A hardware FPU
   (RISC-V F extension) is strongly recommended. Software float emulation will work
   but may reduce performance to ~15 FPS.

3. **Multi-core:** For dual-core RISC-V (e.g., RP2350 Hazard3), use the same symmetric
   screen-space split strategy. Replace RP2350 Multicore FIFO with the available IPC
   mechanism (CLINT software interrupts, shared-memory flags, or hardware semaphores).

4. **Capability reporting:** The GPU firmware should respond to I2C register 0x09 with
   a `PglCapabilityResponse` struct reporting `PGL_ARCH_RISCV_HAZARD3` (0x02),
   `PGL_ARCH_RISCV_CUSTOM` (0x03), or `PGL_ARCH_RISCV_RV32IMF` (0x21) as appropriate.

## 8. Tiered Memory Architecture (M8)

### 8.1 Problem Statement

The Phase 1 SRAM budget (§2) reserves 328 KB of 520 KB for the active render pipeline.
This works well for typical protogen scenes (< 20 objects, < 500 triangles, < 10 textures).
However, complex scenes with many textures, large material banks, or detailed meshes can
exceed the SRAM capacity — particularly when texture data (64 KB cache) is the bottleneck.

**Goal:** Add external memory capacity for textures, materials, and cold meshes **without
degrading the rasterization fast path.** All rasterization-critical data (framebuffer,
Z-buffer, QuadTree) remains permanently in SRAM.

### 8.2 Three-Tier Model

| Tier | Hardware | Interface | Access Model | Bandwidth | Latency |
|---|---|---|---|---|---|
| **0 — SRAM** | RP2350 internal (520 KB) | Direct | Single-cycle load/store | >1 GB/s | 1 cycle (~6.7 ns @ 150 MHz) |
| **1 — PIO2 External** | OPI PSRAM (8 MB) *or* QSPI MRAM (128–256 KB) via PIO2 | PIO + DMA | Indirect: DMA into SRAM cache arena | ~150 MB/s (OPI) / ~52 MB/s (MRAM) | ~5 cycles + DMA setup |
| **2 — QSPI XIP** | Auto-detected on QMI CS1 (MRAM 128 KB or PSRAM 8 MB) | QMI hardware | Memory-mapped (XIP) | ~52 MB/s (MRAM) / ~66 MB/s (PSRAM@133) | ~2 cycles (HW cache hit), 100–200 ns (miss) |

#### Why Two External Tiers?

- **PIO2 External (Tier 1)** uses the last free PIO block with explicit DMA prefetch.
  The firmware **controls exactly when and what** gets loaded — ideal for pipeline-critical
  but infrequently-accessed data (large textures, cold meshes). Three operating modes
  (selected at compile time via `PIO2_MEM_MODE` in `gpu_config.h`):
  - **OPI PSRAM (APS6408L):** 8 MB, ~150 MB/s burst, 8-bit bus (GPIO 34–41).
    Highest capacity and bandwidth, but row-buffer miss penalty on random access.
  - **Dual QSPI MRAM (2× MR10Q010):** 256 KB total, ~52 MB/s per chip, 4-bit bus (GPIO 34–37).
    Two chips with separate CS (GPIO 11 + 38). Address bit 17 selects chip.
    No random-access penalty, non-volatile, unlimited write endurance.
  - **Single QSPI MRAM (1× MR10Q010):** 128 KB, ~52 MB/s, 4-bit bus.
    Same benefits as dual mode but with one chip.
  All modes require data staging in the 64 KB SRAM cache arena before the rasterizer can read it.

- **QSPI XIP (Tier 2)** provides transparent hardware caching via the QMI controller.
  Code reads from XIP pointers as if they were SRAM — the hardware fetches and caches
  automatically. At boot, the firmware **auto-detects** the installed chip on QMI CS1:
  - **MRAM (MR10Q010):** 128 KB, non-volatile, no random-access penalty, ~52 MB/s.
    Ideal for LUTs, font atlases, material params, small textures. Uniform random-access
    performance → tier weights are aggressive (LUTs/materials demote from SRAM readily).
  - **PSRAM (APS6408L):** 8 MB, volatile, row-buffer miss penalty, ~66 MB/s @ 133 MHz.
    Much higher capacity but random reads suffer. Tier weights are conservative — keep
    random-access data in SRAM, demote only sequential/burst-friendly resources to QSPI.
  The `MemTierManager::BaseWeight()` function uses dual weight tables selected at boot
  based on the detected chip's `hasRandomAccessPenalty` flag. See §8.3.

### 8.3 Placement Policy: Weight + Score

Each resource registered with the memory tier system has two metrics:

```
    ┌─────────────────────────────────────────────────┐
    │   priority = α × weight + β × score             │
    │                                                   │
    │   weight  (0–255): Pipeline impact. Higher =     │
    │           more critical to rendering quality.    │
    │           Set at creation time from ResClass.    │
    │                                                   │
    │   score   (0–255): Access frequency. Higher =    │
    │           more accesses per frame. Updated at    │
    │           runtime; decays each frame.             │
    │                                                   │
    │   Default: α = 3, β = 1  (weight-dominant)       │
    └─────────────────────────────────────────────────┘
```

#### Tier Assignment Rules

| Priority Range | Tier | Rationale |
|---|---|---|
| ≥ SRAM threshold | **Tier 0 (SRAM)** | Hot + critical: direct single-cycle access |
| ≥ PIO2 threshold | **Tier 1 (PIO2)** | Critical but infrequent: DMA prefetch |
| Below PIO2 threshold | **Tier 2 (QSPI)** | Frequent-read or cold: HW cache |

Special cases:
- **Pinned resources** (framebuffer, Z-buffer, QuadTree) are **always Tier 0** regardless
  of score. They have `weight = 255` and `pinned = true`.
- **Promotion:** If a Tier 1/2 resource is accessed heavily for `promotionHysteresis`
  consecutive frames, it is promoted to SRAM (DMA copy from OPI, or memcpy from QSPI XIP).
- **Demotion:** If a Tier 0 non-pinned resource has `framesSinceAccess ≥ demotionThreshold`,
  it is demoted to the appropriate lower tier (write-back if dirty, then free SRAM slot).

#### Per-Resource-Class Base Weights (Chip-Aware)

Base weights are selected at boot based on the detected QSPI CS1 chip. When MRAM is
detected (`hasRandomAccessPenalty = false`), random-access resources get lower weights
so they demote to QSPI more readily — MRAM's uniform latency makes this safe. When
PSRAM is detected (or no CS1 chip), conservative weights keep random-access data in SRAM.

| Resource Class | PSRAM Weight | MRAM Weight | Default Tier | Notes |
|---|---|---|---|---|
| FRAMEBUFFER | 255 (pinned) | 255 (pinned) | SRAM | Never leaves SRAM |
| Z_BUFFER | 255 (pinned) | 255 (pinned) | SRAM | Never leaves SRAM |
| QUADTREE | 255 (pinned) | 255 (pinned) | SRAM | Never leaves SRAM |
| VERTEX_DATA | 200 | 200 | SRAM → PIO2 | Sequential access — same either way |
| INDEX_DATA | 200 | 200 | SRAM → PIO2 | Sequential access — same either way |
| TEXTURE | 128 | **100** | SRAM → QSPI/PIO2 | ↓ MRAM handles texel sampling with uniform latency |
| MATERIAL_PARAM | 160 | **80** | SRAM → QSPI | ↓ Random param reads are MRAM's sweet spot |
| LAYOUT_COORDS | 100 | **50** | SRAM / QSPI | ↓ Per-pixel read pattern suits MRAM |
| UV_DATA | 140 | 140 | SRAM / PIO2 | Tightly coupled to vertex pipeline — unchanged |
| LOOKUP_TABLE | 60 | **30** | QSPI XIP | ↓ Perfect MRAM fit: single-cycle random reads |
| FONT_ATLAS | 40 | **20** | QSPI XIP | ↓ Persistent in MRAM, read-only |
| COLD_MESH | 20 | **10** | QSPI / PIO2 | ↓ MRAM persistence eliminates reload cost |

### 8.4 SRAM Cache Arena

When PIO2 external data is needed by the rasterizer, it cannot be accessed directly — the CPU
must read it through DMA into SRAM. The cache arena provides this staging area.

```
    SRAM Map (with cache arena):
    ┌─────────────────────────────────────────────┐
    │  Framebuffers ×2                   32 KB    │
    │  Z-buffer                          32 KB    │
    │  QuadTree nodes                   100 KB    │
    │  Mesh storage                      48 KB    │
    │  Material table                    16 KB    │
    │  SPI ring buffer                   32 KB    │
    │  Draw list                          4 KB    │
    │  Texture hot cache                 64 KB    │  ← reduced if arena grows
    │  ─── SRAM Cache Arena ───          64 KB    │  ← configurable
    │  Pico-SDK + stack                  60 KB    │
    │  Free headroom                     68 KB    │
    └─────────────────────────────────────────────┘
    Total: 520 KB
```

Cache properties:
- **Line size:** 4 KB (configurable via `MEM_TIER_CACHE_LINE_SIZE`)
- **Total lines:** 16 (64 KB / 4 KB)
- **Eviction policy:** LRU (least recently used frame counter)
- **Write-back:** Dirty lines are written back to PIO2 external memory before eviction
- **Locking:** Lines prefetched for the current draw list are locked until frame end

### 8.5 Prefetch Pipeline

The key to avoiding rasterization stalls is **predictive prefetching**. The draw list is
known after command parsing (before rasterization begins), so the tier manager scans it
and pre-loads any PIO2-resident data needed this frame:

```
    Frame N Timeline:
    ┌──────────┬──────────────────┬────────────────────────────────┐
    │ SPI RX   │ Parse commands   │ QuadTree rebuild (Core 0)     │
    │          │ Scan draw list → │ DMA prefetch PIO2 → SRAM cache│
    │          │                  │ ^^^^^^^^^^^^^^^^^              │
    │          │                  │ (zero-CPU DMA, overlapped)     │
    └──────────┴──────────────────┴────────────────────────────────┘
                                  ┌────────────────────────────────┐
                                  │ Rasterize (Core 0 + Core 1)   │
                                  │ All data in SRAM or cache ✓    │
                                  └────────────────────────────────┘
```

If a resource is too large to fit in the cache arena, it is split into 4 KB chunks and
prefetched incrementally as the rasterizer processes each screen-space tile.

### 8.6 Data Flow Summary

```
    Host (ESP32-S3)                      GPU (RP2350)
    ┌─────────────┐   Octal SPI   ┌──────────────────────────────────────┐
    │ PglEncoder  │──────────────→│ Command Parser                       │
    │ PglDevice   │               │    ↓                                  │
    │             │               │ Scene State (Tier 0 SRAM)            │
    │             │   I2C 0x3C    │    ↓                                  │
    │             │←─────────────→│ MemTierManager                       │
    └─────────────┘               │    ├── Tier 0: SRAM (direct)         │
                                  │    ├── Tier 1: PIO2 External ← DMA  │
                                  │    └── Tier 2: QSPI CS1 ← XIP       │
                                  │    ↓                                  │
                                  │ Rasterizer (dual-core)               │
                                  │    ↓                                  │
                                  │ Framebuffer → HUB75 (PIO0 DMA)      │
                                  └──────────────────────────────────────┘
```

### 8.7 Configuration (`gpu_config.h`)

All memory tier features are **disabled by default**. Enable via config flags:

| Config | Default | Description |
|---|---|---|
| `PIO2_MEM_MODE` | `NONE` | PIO2 mode: `OPI_PSRAM`, `DUAL_QSPI_MRAM`, `SINGLE_QSPI_MRAM`, or `NONE` |
| `QSPI_CS1_ENABLED` | `false` | Enable QMI CS1 auto-detection (probes for MRAM or PSRAM) |
| `MEM_TIER_SRAM_CACHE_BUDGET` | 65536 | SRAM cache arena size for PIO2 staging |
| `MEM_TIER_CACHE_LINE_SIZE` | 4096 | Cache line granularity |
| `MEM_TIER_ALPHA_WEIGHT` | 3 | Priority formula: weight coefficient |
| `MEM_TIER_BETA_SCORE` | 1 | Priority formula: score coefficient |
| `MEM_TIER_DEMOTION_THRESHOLD` | 30 | Frames before unused resource demotes |
| `MEM_TIER_PROMOTION_HYSTERESIS` | 50 | Min priority to trigger promotion |

When `PIO2_MEM_MODE` is `NONE` and `QSPI_CS1_ENABLED` is `false`, the tier manager
is compiled out (or operates as a no-op passthrough to SRAM), incurring zero overhead.

### 8.8 Hardware Requirements

| Feature | Required Package | Availability |
|---|---|---|
| Tier 2 (QSPI XIP) | Any RP2350 QFN-60/80 + MR10Q010 MRAM or APS6408L PSRAM on QMI CS1 | Custom ProtoGL GPU board (MRAM needs dual-supply 3.3V/1.8V) |
| Tier 1 — OPI PSRAM | RP2350 QFN-80 + APS6408L (GPIO 34–41) | Custom board (8 data GPIOs needed) |
| Tier 1 — Dual QSPI MRAM | Any RP2350 + 2× MR10Q010 (GPIO 34–37 + CS0/CS1) | Custom board (4 data + 2 CS GPIOs) |
| Tier 1 — Single QSPI MRAM | Any RP2350 + 1× MR10Q010 (GPIO 34–37 + CS0) | Custom board (4 data + 1 CS GPIO) |
| Both tiers | RP2350 QFN-80 + PIO2 memory + QMI CS1 memory | Custom ProtoGL GPU board |

The system gracefully degrades:
- **No external memory:** Full SRAM-only mode (Phase 1 default). Same as M1–M7.
- **PIO2 OPI PSRAM only:** 8 MB for large textures and cold meshes with DMA prefetch.
- **PIO2 Dual QSPI MRAM only:** 256 KB persistent storage with no random-access penalty. Non-volatile → survives reboot.
- **PIO2 Single QSPI MRAM only:** 128 KB persistent storage. Simplest wiring.
- **QSPI XIP MRAM only:** 128 KB read-through XIP for LUTs, fonts, material params. Aggressive tier weights.
- **QSPI XIP PSRAM only:** 8 MB XIP with conservative tier weights.
- **PIO2 + QSPI XIP:** Maximum flexibility. Tier manager distributes resources optimally across both external tiers.

---

## 9. GPU Memory Access API

### 9.1 Motivation

ProtoGL's resource commands (`CreateMesh`, `CreateTexture`, etc.) provide high-level
abstractions — the host sends structured data and the GPU manages storage internally.
However, several real-world scenarios require direct host access to GPU device memory:

| Scenario | Direction | Why High-Level Commands Aren't Enough |
|---|---|---|
| Bulk texture streaming | Host → GPU | Uploading textures page-by-page into PSRAM via DMA, larger than a single SPI frame |
| Diagnostic readback | GPU → Host | Reading framebuffer screenshots or inspecting rasterizer output for debugging |
| Custom data upload | Host → GPU | Uploading lookup tables, fonts, or application-specific data to PSRAM |
| Memory profiling | GPU → Host | Querying per-tier usage, fragmentation, cache hit rates at runtime |
| Resource tier control | Host → GPU | Pinning a hot texture in SRAM or forcing a cold mesh to QSPI |
| GPU-side computation | GPU ↔ GPU | Copying data between tiers without involving the host |

The GPU Memory Access API extends ProtoGL with **7 new SPI commands (0x30–0x3F)** and
**4 new I2C registers (0x0C–0x0F)** that expose all three memory tiers to the host.

### 9.2 Architecture Overview

```
┌─────────────────────┐          ┌──────────────────────────────────┐
│     ESP32-S3 Host   │          │        RP2350 GPU                │
│                     │          │                                  │
│  PglEncoder::       │ OctalSPI │  Command Parser                  │
│   MemWrite(T1,addr) ├─────────►│  case PGL_CMD_MEM_WRITE:         │
│   MemAlloc(T0,4K)   │  64-80M  │    → OpiPsramDriver::Write()     │
│   MemReadRequest()   │  Host→GPU│    → QspiPsramDriver::Write()    │
│   FramebufferCapture │          │    → memcpy() (SRAM)             │
│                     │          │                                  │
│  PglDevice::        │   I2C    │  I2C Slave Handler               │
│   ReadMemTierInfo() ◄─────────►│  REG 0x0C → PglMemTierInfoResp  │
│   ReadMemData()     │ 100-400k │  REG 0x0E → staging buffer[32B] │
│   ReadAllocResult() │  Bidir   │  REG 0x0F → PglMemAllocResult   │
└─────────────────────┘          └──────────────────────────────────┘
```

**Key constraint:** SPI is unidirectional (host → GPU). All data flowing *back* to the
host must use the I2C bus, which is limited to 100–400 kHz (~12–50 KB/s). This makes
readback suitable for **debug/profiling/screenshots** but not real-time streaming.

### 9.3 SPI Command Reference

All memory commands use the 0x30–0x3F opcode range and follow the standard 3-byte
`PglCommandHeader` (opcode + payloadLength) wire format.

#### 9.3.1 CMD_MEM_WRITE (0x30)

Write raw bytes to a specific GPU memory tier and address.

| Field | Type | Offset | Description |
|---|---|---|---|
| tier | uint8_t | 0 | `PglMemTier` (0=SRAM, 1=OPI, 2=QSPI) |
| address | uint32_t | 1 | Byte offset within the tier's address space |
| size | uint32_t | 5 | Number of data bytes that follow |
| data[] | uint8_t[] | 9 | Raw bytes (variable length) |

**Total wire size:** 3 (cmd hdr) + 9 (write hdr) + size (data)

**Usage:** Bulk-upload textures to PIO2 external memory, write lookup tables to QSPI, fill custom
data regions in SRAM. For large transfers, the host can split across multiple frames.

#### 9.3.2 CMD_MEM_READ_REQUEST (0x31)

Request the GPU to stage a block of memory for I2C readback.

| Field | Type | Offset | Description |
|---|---|---|---|
| tier | uint8_t | 0 | Source `PglMemTier` |
| address | uint32_t | 1 | Byte offset within tier |
| size | uint16_t | 5 | Bytes to stage (max 4096) |

After processing, the GPU copies the requested range into its internal staging buffer.
The host then reads the data via `PGL_REG_MEM_READ_DATA` (I2C register 0x0E) in 32-byte
chunks. At 400 kHz I2C, reading 4096 bytes takes approximately 80 ms.

#### 9.3.3 CMD_MEM_SET_RESOURCE_TIER (0x32)

Tier placement hint for an existing resource.

| Field | Type | Offset | Description |
|---|---|---|---|
| resourceClass | uint8_t | 0 | `PglMemResourceClass` (mesh, material, texture, layout, generic) |
| resourceId | uint16_t | 1 | Handle of the resource |
| preferredTier | uint8_t | 3 | Desired `PglMemTier` (or 0xFF=AUTO) |
| flags | uint8_t | 4 | bit0: pinned (never auto-migrate) |

The tier manager records the preference and may initiate an asynchronous migration
if the resource currently resides in a different tier. Pinned resources are exempt
from automatic demotion/promotion based on access patterns.

#### 9.3.4 CMD_MEM_ALLOC (0x33)

Allocate a region in a specific GPU memory tier.

| Field | Type | Offset | Description |
|---|---|---|---|
| tier | uint8_t | 0 | Target `PglMemTier` (AUTO not allowed) |
| size | uint32_t | 1 | Bytes to allocate |
| tag | uint16_t | 5 | User-defined tag for debug/tracking |

The allocation result is available via `PGL_REG_MEM_ALLOC_RESULT` (I2C register 0x0F).
Returns a `PglMemHandle` that can be used with `CMD_MEM_WRITE`, `CMD_MEM_FREE`, and
`CMD_MEM_COPY`.

#### 9.3.5 CMD_MEM_FREE (0x34)

Free a previously allocated GPU memory region.

| Field | Type | Offset | Description |
|---|---|---|---|
| handle | uint16_t | 0 | Handle from a prior `CMD_MEM_ALLOC` |

#### 9.3.6 CMD_FRAMEBUFFER_CAPTURE (0x35)

Snapshot the framebuffer for I2C readback.

| Field | Type | Offset | Description |
|---|---|---|---|
| bufferSelect | uint8_t | 0 | 0=front (displayed), 1=back (in-progress) |
| format | uint8_t | 1 | 0=RGB565 (native), 1=RGB888 (expanded) |

After capture, the staging buffer contains the entire framebuffer. A 128×64 RGB565
framebuffer (16,384 bytes) requires 512 I2C reads of 32 bytes each ≈ 0.33 s at 400 kHz.

#### 9.3.7 CMD_MEM_COPY (0x36)

GPU-internal copy between memory regions or tiers.

| Field | Type | Offset | Description |
|---|---|---|---|
| srcTier | uint8_t | 0 | Source `PglMemTier` |
| srcAddress | uint32_t | 1 | Source byte offset |
| dstTier | uint8_t | 5 | Destination `PglMemTier` |
| dstAddress | uint32_t | 6 | Destination byte offset |
| size | uint32_t | 10 | Bytes to copy |

Executes entirely on-GPU. Same-tier SRAM copies use `memcpy`; cross-tier copies
use DMA where available. Useful for promoting hot data from QSPI→SRAM or backing up
SRAM resources to PSRAM.

### 9.4 I2C Register Reference

| Register | Address | Direction | Size | Description |
|---|---|---|---|---|
| `MEM_TIER_INFO` | 0x0C | Read | 20 B | Per-tier capacity, usage, cache stats |
| `MEM_READ_ADDR` | 0x0D | Write | 7 B | Set tier + address for next readback |
| `MEM_READ_DATA` | 0x0E | Read | 32 B | Read 32 bytes from staging buffer (auto-increments) |
| `MEM_ALLOC_RESULT` | 0x0F | Read | 7 B | Handle + address + status from last alloc |

#### MEM_TIER_INFO (0x0C) — `PglMemTierInfoResponse`

```
Offset  Size  Field               Description
0       2     sramTotalKB         Total SRAM for GPU use
2       2     sramFreeKB          Free SRAM
4       2     opiTotalKB          PIO2 external memory total (0 if absent)
6       2     opiFreeKB           PIO2 external memory free
8       1     opiEnabled          1 if PIO2 driver active
9       2     qspiTotalKB         QSPI MRAM total (0 if absent)
11      2     qspiFreeKB          QSPI MRAM free
13      1     qspiEnabled         1 if QSPI driver active
14      2     cachedEntries       SRAM cache arena entries
16      2     totalManagedAllocs  Total tracked allocations
18      1     cacheHitRate        Rolling average 0-100%
19      1     reserved            Padding
```

This allows the host to monitor GPU memory pressure at runtime and make informed
decisions about resource loading, quality scaling, or tier placement hints.

#### MEM_READ_DATA (0x0E) — Chunked Readback

Each I2C read returns exactly 32 bytes from the staging buffer and auto-increments
the read cursor. The host calculates the number of reads needed:

```
reads = ceil(stagedLength / 32)
```

If the cursor exceeds the staged length, the GPU returns zeros (padding).

#### MEM_ALLOC_RESULT (0x0F) — `PglMemAllocResult`

```
Offset  Size  Field     Description
0       2     handle    PglMemHandle (0xFFFF on failure)
2       4     address   Tier-relative byte address
6       1     status    0x00=OK, 0x01=OOM, 0x02=InvalidTier, 0x03=Disabled, 0x04=HandleExhausted
```

### 9.5 Host-Side Usage Examples

#### Upload Texture to PIO2 External Memory

```cpp
// 1. Allocate space in PIO2 external memory (enum covers OPI PSRAM / QSPI MRAM)
encoder.MemAlloc(PGL_TIER_OPI_PSRAM, textureBytes, 0x0001);
// ... transfer frame, then read I2C for the result handle ...

// 2. Write texture data in chunks across multiple frames
for (uint32_t off = 0; off < textureBytes; off += CHUNK_SIZE) {
    uint32_t len = min(CHUNK_SIZE, textureBytes - off);
    encoder.MemWrite(PGL_TIER_OPI_PSRAM, allocAddr + off,
                     textureData + off, len);
}

// 3. Pin the texture in PIO2 so tier manager doesn't demote it
encoder.SetResourceTier(PGL_RES_CLASS_TEXTURE, texId,
                        PGL_TIER_OPI_PSRAM, /*pinned=*/true);
```

#### Capture Framebuffer Screenshot

```cpp
// 1. Request capture
encoder.FramebufferCapture(0, PGL_TEX_RGB565);  // front buffer, RGB565

// 2. After frame is processed, read back via I2C
PglMemTierInfoResponse info;
device.ReadRegister(PGL_REG_MEM_TIER_INFO, &info, sizeof(info));

// 3. Read framebuffer data in 32-byte chunks
uint8_t screenshot[16384];
for (int i = 0; i < 512; i++) {
    device.ReadRegister(PGL_REG_MEM_READ_DATA, &screenshot[i*32], 32);
}
```

#### Query Memory Pressure

```cpp
PglMemTierInfoResponse info;
device.ReadRegister(PGL_REG_MEM_TIER_INFO, &info, sizeof(info));

if (info.sramFreeKB < 32) {
    // Low SRAM — reduce mesh complexity or move textures to PSRAM
    encoder.SetResourceTier(PGL_RES_CLASS_TEXTURE, coldTexId,
                            PGL_TIER_OPI_PSRAM);
}
printf("Cache hit rate: %u%%\n", info.cacheHitRate);
```

### 9.6 Design Rationale

| Decision | Rationale |
|---|---|
| Separate opcode range (0x30–0x3F) | Clean separation from resource commands; future-proof for more memory ops |
| I2C for readback (not reverse SPI) | Octal SPI is unidirectional by hardware. I2C already wired. Phase 2 may add a reverse SPI channel. |
| 32-byte I2C chunks | I2C transaction overhead dominates below ~32 bytes; larger chunks hit I2C buffer limits |
| 4096-byte staging buffer | Balances SRAM cost (~4 KB) vs readback utility. Covers most diagnostic needs. |
| PglMemHandle (uint16_t) | 65,534 possible handles; matches resource handle types for consistency |
| `PGL_TIER_AUTO` hint | Lets host defer to tier manager intelligence while keeping explicit control available |
| Framebuffer capture as separate opcode | Distinct from generic MemRead because it requires buffer selection and format conversion |

### 9.7 Vulkan Parallels

For developers familiar with Vulkan, the ProtoGL memory access API maps as follows:

| ProtoGL | Vulkan Equivalent |
|---|---|
| `CMD_MEM_ALLOC` | `vkAllocateMemory` |
| `CMD_MEM_FREE` | `vkFreeMemory` |
| `CMD_MEM_WRITE` | `vkMapMemory` + `memcpy` + `vkUnmapMemory` |
| `CMD_MEM_READ_REQUEST` + I2C | `vkMapMemory` (read direction) |
| `CMD_MEM_SET_RESOURCE_TIER` | `VkMemoryPropertyFlags` at bind time |
| `CMD_MEM_COPY` | `vkCmdCopyBuffer` |
| `PGL_REG_MEM_TIER_INFO` | `vkGetPhysicalDeviceMemoryProperties` |
| `PGL_REG_MEM_ALLOC_RESULT` | `VkResult` from `vkAllocateMemory` |

The key difference is that ProtoGL's read path is **asynchronous** (command → staging → I2C poll)
rather than synchronous mapping, due to the one-directional SPI transport.

### 9.8 Limitations and Future Work

| Limitation | Impact | Planned Resolution |
|---|---|---|
| I2C readback bandwidth (~50 KB/s) | Framebuffer readback takes 0.33 s for 16 KB | Phase 2: Reverse SPI channel (GPU→Host) |
| 4 KB staging buffer | Large reads must be split into multiple requests | Increase to 8–16 KB if SRAM budget allows |
| No scatter-gather writes | Multi-region uploads require separate commands | Possible CMD_MEM_WRITE_SG (0x37) in future |
| Handle limit (65534) | Exceeding requires explicit free | More than sufficient for embedded use |
| No GPU→Host notification of alloc completion | Host must poll I2C after sending alloc cmd | Phase 2: IRQ/GPIO signal on completion |

## 10. GPU Diagnostics, Thermal Management & Dynamic Clock

### 10.1 Extended Status Response

Register `PGL_REG_EXTENDED_STATUS` (0x11) returns a 32-byte `PglExtendedStatusResponse`
providing comprehensive GPU health data. This supplements the basic 8-byte `PglStatusResponse`
(register 0x0A) with thermal, timing, and VRAM telemetry.

**Key fields:**
- **GPU usage** (0–100%): fraction of frame time spent in active rendering
- **Per-core usage**: Core 0 (transforms + raster top-half) and Core 1 (raster bottom-half)
- **Die temperature**: Q8.8 fixed-point (°C × 256), read from on-chip ADC (±5 °C)
- **Clock frequency**: actual running `clk_sys` in MHz
- **Frame timing breakdown**: total frame time, rasterization time, SPI transfer time, HUB75 refresh rate
- **VRAM tier flags**: bitfield indicating which external memory tiers were detected and initialised

The host reads this register at a configurable interval (default: every 300 frames / ~5 seconds).
`GPUDriverController::Display()` logs the fields to Serial for diagnostics.

### 10.2 VRAM Detection & Reporting

At boot, the GPU firmware probes for external memory:

1. **PIO2 External Memory**: Check `GpuConfig::PIO2_MEM_MODE` (OPI PSRAM / QSPI MRAM).
   If mode ≠ `NONE`, initialize the appropriate PIO2 program and issue a read-ID command.
   On success, set `PGL_CAP_OPI_VRAM` in capability flags and `PGL_VRAM_OPI_DETECTED`
   in extended status `vramTierFlags`.
2. **QSPI CS1** (auto-detect): Check `GpuConfig::QSPI_CS1_ENABLED`. `ProbeQspiCs1()` runs a
   3-step auto-detect sequence:
   - **Step 1 — MRAM RDID** (0x4B + mode byte 0xFF → read 5 bytes): Match against MR10Q010
     signature (0x076B111111). If matched, set profile to `PROFILE_MR10Q010`.
   - **Step 2 — PSRAM RDID** (0x9F → read 3 bytes): Check manufacturer byte
     (0x0D = AP Memory APS6408L, 0x5D = Espressif ESP-PSRAM). Set matching profile.
   - **Step 3 — Unknown fallback**: If RDID responded but ID is unrecognized, assign
     `UNKNOWN_DEVICE` type with conservative defaults.
   On success, set `PGL_CAP_QSPI_VRAM` and `PGL_VRAM_QSPI_DETECTED`. The detected
   `QspiChipProfile` drives driver init (MRAM vs PSRAM path) and `MemTierManager::BaseWeight()`
   dual weight table selection. Extended status byte 31 (`qspiChipType`) reports the detected
   chip type to the host as a `PglQspiChipType` value.

The host calls `QueryCapability()` at startup and receives these flags. Convenience method
`HasExternalVram()` checks both flags in one call. The extended status reports total/free KB
per VRAM tier, allowing the host to monitor memory pressure during operation.

**Graceful degradation**: If no external memory is detected, the GPU operates in SRAM-only mode.
All VRAM fields read zero. No code path change — the M7 baseline path handles this transparently.

### 10.3 On-Die Temperature Sensor

The RP2350 has an on-chip temperature sensor connected to ADC channel `ADC_TEMPERATURE_CHANNEL_NUM`
(channel 4 on QFN-60, channel 8 on QFN-80). The conversion formula is:

$$T_{°C} = 27.0 - \frac{V_{ADC} - 0.706}{0.001721}$$

Where $V_{ADC}$ is derived from the 12-bit ADC reading: $V = \text{raw} \times 3.3 / 4096$.

Accuracy is ±5 °C — sufficient for thermal throttling decisions. The temperature sensor
derives from `clk_adc` (USB PLL), so readings remain valid when `clk_sys` changes.

The temperature is reported as Q8.8 fixed-point in `PglExtendedStatusResponse::temperatureQ8`.
Host conversion: `float tempC = temperatureQ8 / 256.0f`.

### 10.4 Dynamic Clock Management

The RP2350 PLL system supports glitch-free clock frequency changes at runtime using
`set_sys_clock_pll()`. During transition, the CPU runs on the USB PLL (48 MHz) so
no instructions are lost.

#### Clock Profiles

| Frequency | VCO (MHz) | Post Div 1 | Post Div 2 | VREG Level | Notes |
|-----------|-----------|------------|------------|------------|-------|
| 150 MHz | 1500 | 5 | 2 | 1.10V (default) | Default / safe profile |
| 200 MHz | 1200 | 6 | 1 | 1.10V | No voltage change needed |
| 250 MHz | 1500 | 6 | 1 | 1.10–1.15V | Mild overclock |
| 266 MHz | 1596 | 6 | 1 | 1.10V | Common integer ratio |
| 300 MHz | 1500 | 5 | 1 | 1.15–1.20V | Recommended max overclock |

All VCO values are within the validated 750–1600 MHz range. XOSC reference is 12 MHz.

#### Voltage Sequencing

When **increasing** frequency: raise VREG → wait stabilisation → change PLL → recalculate PIO.
When **decreasing** frequency: change PLL → recalculate PIO → lower VREG.
This prevents brownout during transitions.

#### PIO SM Clock Divider Recalculation

PIO state machines derive their clock from `clk_sys`. When `clk_sys` changes, all active
PIO SM dividers must be scaled proportionally to maintain the same output frequency:

$$\text{newDiv} = \text{baseDivider} \times \frac{\text{newClkSysMHz}}{\text{baseClkSysMHz}}$$

The `GpuClock` module tracks up to 4 active PIO SMs and recalculates their dividers
automatically when `SetFrequency()` is called with `PGL_CLOCK_RECONFIGURE_PIO`.

#### Thermal Throttling

Three temperature thresholds are enforced:
- **80 °C — Throttle**: Step down one clock profile (e.g., 300 → 250 MHz)
- **65 °C — Recover**: Restore original requested frequency if currently throttled
- **95 °C — Emergency**: Force minimum clock (150 MHz) regardless of requested frequency

The host can enable/disable automatic thermal throttling via `PGL_CLOCK_THERMAL_AUTO` flag
in `PglClockRequest::flags`.

#### Host API

```cpp
// Request 300 MHz with auto voltage selection
controller.SetGpuClock(300);

// Query current health (includes actual clock, temperature)
PglExtendedStatusResponse health = controller.QueryGpuHealth();
float temp = controller.GetGpuTemperature();   // cached °C
uint8_t usage = controller.GetGpuUsagePercent(); // cached 0–100
```

### 10.5 Peripheral Impact Summary

| Peripheral | Clock Source | Affected by `clk_sys` Change? | Mitigation |
|---|---|---|---|
| CPU cores | `clk_sys` | Yes — performance scales linearly | Expected (that's the point) |
| PIO (HUB75, PIO2 External) | `clk_sys` | Yes — output frequency changes | `RecalculatePioClocks()` fixes dividers |
| I2C | `clk_peri` (USB PLL) | No | — |
| ADC (temp sensor) | `clk_adc` (USB PLL) | No | — |
| USB | `clk_usb` (USB PLL) | No | — |
| Timers | `clk_ref` (XOSC) | No | — |