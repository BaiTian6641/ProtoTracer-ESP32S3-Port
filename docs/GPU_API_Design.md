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
| Data bus | PIO-driven bidirectional Octal SPI (8-bit parallel, half-duplex) |
| Management bus | Hardware I2C slave at 0x3C — low-bandwidth control plane (device ID, status, config) |
| Flow control | GPIO DIR pin (bidirectional bus direction) |

### 1.2 Architecture-Agnostic API
The ProtoGL wire protocol is **architecture-agnostic**. Phase 2 can retarget other GPU implementations without host-side changes:

| Architecture | Example | SRAM | FPU | Unaligned Access | Phase |
|---|---|---|---|---|---|
| ARM Cortex-M33 | RP2350 (ARM mode) | 520 KB | Yes (SP) | Yes (native) | **Phase 1** |
| RISC-V Hazard3 | RP2350 (RISC-V mode) | 520 KB | Yes (SP) | Optional | Phase 2 |
| Custom RISC-V | User SoC | Varies | Optional | Unlikely | Phase 2 |
| FPGA (soft-core) | Lattice/Xilinx | Block RAM | Optional | N/A | Phase 2 |
| ARM Cortex-M7 | STM32H7, i.MX RT | 1 MB+ | Yes (DP) | Yes | Phase 2 |

The GPU reports its architecture and capabilities to the host via the I2C management bus
capability query (register 0x09) or SPI read command (0xE2), returning a `PglCapabilityResponse`.

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
- **I2C OLED** for small status displays
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

4. **Capability reporting:** The GPU firmware should respond to SPI read command `0xE2`
   (or I2C register 0x09 as fallback) with a `PglCapabilityResponse` struct reporting
   `PGL_ARCH_RISCV_HAZARD3` (0x02), `PGL_ARCH_RISCV_CUSTOM` (0x03), or
   `PGL_ARCH_RISCV_RV32IMF` (0x21) as appropriate.

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
| **1 — QSPI-A** | PIO2 Channel A (up to 2 chips via CS0/CS1) | PIO2 SM0+SM1, DMA | Indirect: DMA into SRAM cache arena | ~52 MB/s per chip | ~5 cycles + DMA setup |
| **2 — QSPI-B** | PIO2 Channel B (up to 2 chips via CS0/CS1) | PIO2 SM2+SM3, DMA | Indirect: DMA into SRAM cache arena | ~52 MB/s per chip | ~5 cycles + DMA setup |

> **RP2350A vs RP2350B:** The RP2350A (QFN-60, 30 GPIO) does **not** have enough
> pins for external VRAM and operates in SRAM-only mode (Tier 0 only). The RP2350B
> (QFN-80, 48 GPIO) provides GPIO 34–47 for external VRAM, supporting up to 2×2 = 4
> external RAM chips.

#### Dual-Channel QSPI Architecture

Both external tiers use PIO2-driven QSPI (4-bit data bus). Each channel has its own
clock, data lines, and **two chip selects** — allowing up to 2 chips per channel:

- **QSPI Channel A (Tier 1):** PIO2 SM0 (write/cmd) + SM1 (read), 4 data pins +
  CLK + CS0 + CS1 = 7 GPIO. Up to 2 RAM chips. The firmware selects the active chip
  via the CS lines (only one CS asserted at a time).
- **QSPI Channel B (Tier 2):** PIO2 SM2 (write/cmd) + SM3 (read), 4 data pins +
  CLK + CS0 + CS1 = 7 GPIO. Up to 2 RAM chips. Independent of Channel A — both
  channels can DMA concurrently.

Each chip slot is auto-detected at boot via RDID commands. Supported chip types:
- **MRAM (MR10Q010):** 128 KB, non-volatile, no random-access penalty, ~52 MB/s,
  unlimited write endurance. Ideal for persistent resources, LUTs, material params.
- **PSRAM (APS6408L):** 8 MB, volatile, row-buffer miss penalty, ~52 MB/s.
  Higher capacity, best for large textures and cold meshes (sequential access).
- **Mixed configurations** are supported: e.g., Channel A = 2× PSRAM (16 MB total),
  Channel B = 2× MRAM (256 KB persistent). The tier manager detects each chip
  independently and applies chip-aware weight tables per channel.

All external data requires staging in the 64 KB SRAM cache arena before the rasterizer
can read it. Both channels use the same DMA prefetch pipeline (§8.5).

#### Why Two Channels Instead of One?

- **Concurrent DMA:** Two independent PIO state machine pairs allow overlapping
  read/write operations — Channel A can DMA-prefetch textures while Channel B
  writes back dirty cache lines.
- **Chip-type separation:** MRAM and PSRAM have different optimal use cases.
  Keeping them on separate channels avoids bus contention and allows the tier
  manager to route resources by access pattern (random → MRAM, sequential → PSRAM).
- **Capacity scaling:** Each channel independently supports 1–2 chips. A minimal
  board can populate only Channel A with 1 chip; a full board has 4 chips.

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
| ≥ QSPI-A threshold | **Tier 1 (QSPI-A)** | Critical but infrequent: DMA prefetch |
| Below QSPI-A threshold | **Tier 2 (QSPI-B)** | Cold / persistent: DMA prefetch |

Special cases:
- **Pinned resources** (framebuffer, Z-buffer, QuadTree) are **always Tier 0** regardless
  of score. They have `weight = 255` and `pinned = true`.
- **Promotion:** If a Tier 1/2 resource is accessed heavily for `promotionHysteresis`
  consecutive frames, it is promoted to SRAM (DMA copy from the external channel).
- **Demotion:** If a Tier 0 non-pinned resource has `framesSinceAccess ≥ demotionThreshold`,
  it is demoted to the appropriate lower tier (write-back if dirty, then free SRAM slot).

#### Per-Resource-Class Base Weights (Chip-Aware)

Base weights are selected at boot based on the detected chip types per channel. When
MRAM is detected on a channel (`hasRandomAccessPenalty = false`), random-access resources
get lower weights so they demote to that channel more readily — MRAM's uniform latency
makes this safe. When only PSRAM is detected, conservative weights keep random-access
data in SRAM. When both channel types differ (e.g., PSRAM on QSPI-A, MRAM on QSPI-B),
the tier manager routes resources to the channel best suited to their access pattern.

| Resource Class | PSRAM Weight | MRAM Weight | Default Tier | Notes |
|---|---|---|---|---|
| FRAMEBUFFER | 255 (pinned) | 255 (pinned) | SRAM | Never leaves SRAM |
| Z_BUFFER | 255 (pinned) | 255 (pinned) | SRAM | Never leaves SRAM |
| QUADTREE | 255 (pinned) | 255 (pinned) | SRAM | Never leaves SRAM |
| VERTEX_DATA | 200 | 200 | SRAM → QSPI-A | Sequential access — same either way |
| INDEX_DATA | 200 | 200 | SRAM → QSPI-A | Sequential access — same either way |
| TEXTURE | 128 | **100** | SRAM → QSPI-A/B | ↓ MRAM handles texel sampling with uniform latency |
| IMAGE_SEQUENCE_ATLAS | 30 | **15** | QSPI-A / QSPI-B | Large (KB–MB); only active frame cached in SRAM arena |
| MATERIAL_PARAM | 160 | **80** | SRAM → MRAM ch. | ↓ Random param reads are MRAM's sweet spot |
| LAYOUT_COORDS | 100 | **50** | SRAM / MRAM ch. | ↓ Per-pixel read pattern suits MRAM |
| UV_DATA | 140 | 140 | SRAM / QSPI-A | Tightly coupled to vertex pipeline — unchanged |
| LOOKUP_TABLE | 60 | **30** | MRAM ch. | ↓ Perfect MRAM fit: uniform random reads |
| FONT_ATLAS | 40 | **20** | MRAM ch. | ↓ Persistent in MRAM, read-only |
| SHADER_PROGRAM | 80 | **40** | MRAM ch. | Bytecode is read-only; DMA-cached |
| COLD_MESH | 20 | **10** | QSPI-A / QSPI-B | ↓ MRAM persistence eliminates reload cost |

#### Memory Placement Policy (Resource Categories)

The tiering manager uses the weight table above for automatic placement, but the
following **hard rules** take precedence regardless of weight scores:

| Category | Policy | Rationale |
|---|---|---|
| **Color-based material params** (SimpleMaterial, NormalMaterial, DepthMaterial, GradientMaterial, LightMaterial, SimplexNoise, RainbowNoise, CombineMaterial, MaterialMask, MaterialAnimator) | **Always SRAM (Tier 0)** | 3–50 bytes per material. Read every pixel during rasterization — must be single-cycle. `MaterialAnimator` interpolates between two materials each frame and is equally small. |
| **ImageMaterial / ImageSequenceMaterial params** | **Always SRAM (Tier 0)** | Only the parameter block (12–13 bytes) resides in SRAM. The backing texture / image-sequence atlas is a separate resource placed by the weight table. |
| **Small textures** (≤ 4 KB) | **Prefer SRAM (Tier 0)** | Icons, small sprites, and simple textures fit comfortably in SRAM for single-cycle texel lookup. |
| **Large textures** (> 4 KB) | **External VRAM (Tier 1/2)** | Hot cache lines promoted to SRAM cache arena via DMA on demand. |
| **Image sequence atlases** | **External VRAM (Tier 1/2), never SRAM** | Atlases are typically tens of KB to several MB. Only the currently active frame (one frameWidth × frameHeight region) is DMA-fetched into the SRAM cache arena during rasterization. |
| **Font atlases** | **External VRAM (prefer MRAM channel)** | Read-only after upload. DMA-cached on demand. Small fonts (< 2 KB) may stay in SRAM. |
| **Shader program bytecode** | **External VRAM (prefer MRAM channel)** | Read-only instruction stream, DMA-cached. |

> **Key design principle:** All materials — including animated materials like
> `MaterialAnimator` that interpolate between two color-based materials each frame —
> are **guaranteed to remain in SRAM**. This is because their parameter blocks never
> exceed ~50 bytes and are accessed at per-pixel frequency during rasterization.
> In contrast, **image sequence atlases** are explicitly excluded from SRAM residency
> and must live in external VRAM, with only the active frame promoted to the cache arena.
>
> **Persistence principle:** Large resources placed in external VRAM follow the
> persistence decision tree (§9.9): if the external VRAM is MRAM, the data is
> inherently persistent across power cycles with no additional action. If the VRAM
> is volatile PSRAM and the host wants the resource to survive reboot, the host sends
> `CMD_PERSIST_RESOURCE` and the GPU asynchronously writes the data back to on-board
> flash. On the next boot the GPU auto-restores from flash, so the host can skip
> re-uploading. This is critical for large textures and image sequence atlases that
> take many frames to upload over SPI.

### 8.4 SRAM Cache Arena

When external QSPI data is needed by the rasterizer, it cannot be accessed directly — the CPU
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
- **Write-back:** Dirty lines are written back to external QSPI memory before eviction
- **Locking:** Lines prefetched for the current draw list are locked until frame end

### 8.5 Prefetch Pipeline

The key to avoiding rasterization stalls is **predictive prefetching**. The draw list is
known after command parsing (before rasterization begins), so the tier manager scans it
and pre-loads any QSPI-resident data needed this frame:

```
    Frame N Timeline:
    ┌──────────┬──────────────────┬────────────────────────────────┐
    │ SPI RX   │ Parse commands   │ QuadTree rebuild (Core 0)     │
    │          │ Scan draw list → │ DMA prefetch QSPI → SRAM cache│
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
    │             │ Bidir OctalSPI│    ↓                                  │
    │             │←─────────────→│ MemTierManager                       │
    └─────────────┘               │    ├── Tier 0: SRAM (direct)         │
                                  │    ├── Tier 1: QSPI-A ← PIO2 DMA    │
                                  │    └── Tier 2: QSPI-B ← PIO2 DMA    │
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
| `QSPI_VRAM_MODE` | `NONE` | External VRAM mode: `NONE`, `SINGLE_CHANNEL` (Tier 1 only), or `DUAL_CHANNEL` (Tier 1 + Tier 2) |
| `QSPI_A_CHIP_COUNT` | 0 | Number of chips on Channel A (0, 1, or 2) |
| `QSPI_B_CHIP_COUNT` | 0 | Number of chips on Channel B (0, 1, or 2) |
| `MEM_TIER_SRAM_CACHE_BUDGET` | 65536 | SRAM cache arena size for QSPI staging |
| `MEM_TIER_CACHE_LINE_SIZE` | 4096 | Cache line granularity |
| `MEM_TIER_ALPHA_WEIGHT` | 3 | Priority formula: weight coefficient |
| `MEM_TIER_BETA_SCORE` | 1 | Priority formula: score coefficient |
| `MEM_TIER_DEMOTION_THRESHOLD` | 30 | Frames before unused resource demotes |
| `MEM_TIER_PROMOTION_HYSTERESIS` | 50 | Min priority to trigger promotion |

When `QSPI_VRAM_MODE` is `NONE`, the tier manager is compiled out (or operates as a
no-op passthrough to SRAM), incurring zero overhead. This is the default for RP2350A boards.

### 8.8 Hardware Requirements

| Feature | Required Package | Pin Budget | Availability |
|---|---|---|---|
| SRAM-only (no external VRAM) | RP2350A (QFN-60) or RP2350B | 0 GPIO | All boards |
| Tier 1 — QSPI-A (1 chip) | RP2350B (QFN-80) | 6 GPIO (4 data + CLK + CS0) | Custom ProtoGL GPU board |
| Tier 1 — QSPI-A (2 chips) | RP2350B (QFN-80) | 7 GPIO (4 data + CLK + CS0 + CS1) | Custom board |
| Tier 1 + 2 — QSPI-A + QSPI-B (up to 4 chips) | RP2350B (QFN-80) | 14 GPIO (2× channel) | Full ProtoGL GPU board |

The system gracefully degrades:
- **RP2350A (no external memory):** Full SRAM-only mode (Phase 1 default). Same as M1–M7.
- **RP2350B, single channel (1–2 chips):** Tier 1 only. Tier manager routes overflow to QSPI-A.
- **RP2350B, dual channel (2–4 chips):** Full 3-tier system. Tier manager distributes resources
  optimally across both channels based on chip type (MRAM vs PSRAM) and access pattern.

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
│   MemAlloc(T0,4K)   │  64-80M  │    → QspiVramDriver::Write(ChA)   │
│   MemReadRequest()   │  Host→GPU│    → QspiVramDriver::Write(ChB)   │
│   FramebufferCapture │  (TX)   │    → memcpy() (SRAM)             │
│                     │          │                                  │
│  PglDevice::        │ OctalSPI │  SPI Read Handler (PIO1 SM1)     │
│   QueryStatus()     ◄──────────│  SPI_READ_STATUS → 8B resp       │
│   ReadMemData()     │  64-80M  │  SPI_READ_MEM_DATA → staging buf │
│   ReadAllocResult() │  GPU→Host│  SPI_READ_ALLOC_RESULT → 7B resp │
│   SmwReadMailbox()  │  (RX)   │  SPI_READ_SMW → SMW region       │
│                     │          │                                  │
│        DIR pin ─────┼──────────│→ GPIO 10: bus direction control   │
│        IRQ pin ◄────┼──────────│─ GPIO 13: async notification     │
└─────────────────────┘          └──────────────────────────────────┘
```

**Key constraint:** The Octal SPI bus operates in half-duplex bidirectional mode.
Host→GPU writes use the LCD_CAM peripheral; GPU→Host reads use SPI2 in Octal HD mode
with a DIR pin turnaround. I2C is available as a fallback but is ~1000× slower.

### 9.3 SPI Command Reference

All memory commands use the 0x30–0x3F opcode range and follow the standard 3-byte
`PglCommandHeader` (opcode + payloadLength) wire format.

#### 9.3.1 CMD_MEM_WRITE (0x30)

Write raw bytes to a specific GPU memory tier and address.

| Field | Type | Offset | Description |
|---|---|---|---|
| tier | uint8_t | 0 | `PglMemTier` (0=SRAM, 1=QSPI-A, 2=QSPI-B) |
| address | uint32_t | 1 | Byte offset within the tier's address space |
| size | uint32_t | 5 | Number of data bytes that follow |
| data[] | uint8_t[] | 9 | Raw bytes (variable length) |

**Total wire size:** 3 (cmd hdr) + 9 (write hdr) + size (data)

**Usage:** Bulk-upload textures to QSPI external memory, write lookup tables, fill custom
data regions in SRAM. For large transfers, the host can split across multiple frames.

#### 9.3.2 CMD_MEM_READ_REQUEST (0x31)

Request the GPU to stage a block of memory for SPI readback (or I2C fallback).

| Field | Type | Offset | Description |
|---|---|---|---|
| tier | uint8_t | 0 | Source `PglMemTier` |
| address | uint32_t | 1 | Byte offset within tier |
| size | uint16_t | 5 | Bytes to stage (max 4096) |

After processing, the GPU copies the requested range into its internal staging buffer.
The host reads the data via bidirectional Octal SPI (`SPI_READ_MEM_DATA`, 0xE4) at bus
speed (~40 MB/s), or via I2C register 0x0E as fallback (~80 ms for 4096 bytes at 400 kHz).

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

The allocation result is available via SPI read (`SPI_READ_ALLOC_RESULT`, 0xE5) or
I2C register 0x0F as fallback. Returns a `PglMemHandle` that can be used with
`CMD_MEM_WRITE`, `CMD_MEM_FREE`, and `CMD_MEM_COPY`.

#### 9.3.5 CMD_MEM_FREE (0x34)

Free a previously allocated GPU memory region.

| Field | Type | Offset | Description |
|---|---|---|---|
| handle | uint16_t | 0 | Handle from a prior `CMD_MEM_ALLOC` |

#### 9.3.6 CMD_FRAMEBUFFER_CAPTURE (0x35)

Snapshot the framebuffer for SPI readback (via SMW bulk staging).

| Field | Type | Offset | Description |
|---|---|---|---|
| bufferSelect | uint8_t | 0 | 0=front (displayed), 1=back (in-progress) |
| format | uint8_t | 1 | 0=RGB565 (native), 1=RGB888 (expanded) |

After capture, the staging buffer contains the entire framebuffer. Via bidirectional
Octal SPI, a 128×64 RGB565 framebuffer (16,384 bytes) reads back in ~0.3 ms.
(I2C fallback: 512 reads of 32 bytes each ≈ 0.33 s at 400 kHz.)

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
use DMA where available. Useful for promoting hot data from external VRAM→SRAM or
backing up SRAM resources to external VRAM.

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
4       2     qspiATotalKB        QSPI Channel A total (0 if absent)
6       2     qspiAFreeKB         QSPI Channel A free
8       1     qspiAEnabled        1 if Channel A driver active
9       1     qspiAChipCount      Number of chips on Channel A (0–2)
10      2     qspiBTotalKB        QSPI Channel B total (0 if absent)
12      2     qspiBFreeKB         QSPI Channel B free
14      1     qspiBEnabled        1 if Channel B driver active
15      1     qspiBChipCount      Number of chips on Channel B (0–2)
16      2     cachedEntries       SRAM cache arena entries
18      1     cacheHitRate        Rolling average 0–100%
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

#### Upload Texture to QSPI External Memory

```cpp
// 1. Allocate space in QSPI Channel A external memory
encoder.MemAlloc(PGL_TIER_QSPI_A, textureBytes, 0x0001);
// ... transfer frame, then read I2C for the result handle ...

// 2. Write texture data in chunks across multiple frames
for (uint32_t off = 0; off < textureBytes; off += CHUNK_SIZE) {
    uint32_t len = min(CHUNK_SIZE, textureBytes - off);
    encoder.MemWrite(PGL_TIER_QSPI_A, allocAddr + off,
                     textureData + off, len);
}

// 3. Pin the texture in QSPI-A so tier manager doesn't demote it
encoder.SetResourceTier(PGL_RES_CLASS_TEXTURE, texId,
                        PGL_TIER_QSPI_A, /*pinned=*/true);
```

#### Capture Framebuffer Screenshot

```cpp
// 1. Request capture
encoder.FramebufferCapture(0, PGL_TEX_RGB565);  // front buffer, RGB565

// 2. After frame is processed, read back via bidirectional Octal SPI
//    GPU stages captured data into SMW bulk buffer, pulses IRQ
uint8_t screenshot[16384];
for (int chunk = 0; chunk < 5; chunk++) {   // 5 × 3840 = 19200 > 16384
    encoder.SmwStageRead(fbBaseAddr + chunk * 3840,
                         std::min(3840u, 16384u - chunk * 3840));
    // Wait for IRQ or poll SmwGetSequence()
    encoder.SmwReadBulk(&screenshot[chunk * 3840],
                        std::min(3840u, 16384u - chunk * 3840));
}
// Total time: ~5 × 55 µs ≈ 0.3 ms (vs ~320 ms via I2C)
```

#### Query Memory Pressure

```cpp
PglMemTierInfoResponse info;
device.ReadRegister(PGL_REG_MEM_TIER_INFO, &info, sizeof(info));

if (info.sramFreeKB < 32) {
    // Low SRAM — reduce mesh complexity or move textures to external VRAM
    encoder.SetResourceTier(PGL_RES_CLASS_TEXTURE, coldTexId,
                            PGL_TIER_QSPI_A);
}
printf("Cache hit rate: %u%%\n", info.cacheHitRate);
```

### 9.6 Design Rationale

| Decision | Rationale |
|---|---|
| Separate opcode range (0x30–0x3F) | Clean separation from resource commands; future-proof for more memory ops |
| Bidirectional Octal SPI for readback | Half-duplex on same 8 data lines. DIR pin (GPIO 10) controls bus direction. Host uses SPI2 peripheral in Octal HD mode for reads. ~40 MB/s reverse bandwidth. |
| 4 KB max SPI read | Keeps turnaround + DMA setup overhead amortised; larger reads split into SMW bulk chunks |
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
| `CMD_MEM_READ_REQUEST` + SPI read | `vkMapMemory` (read direction) |
| `CMD_MEM_SET_RESOURCE_TIER` | `VkMemoryPropertyFlags` at bind time |
| `CMD_MEM_COPY` | `vkCmdCopyBuffer` |
| `PGL_REG_MEM_TIER_INFO` | `vkGetPhysicalDeviceMemoryProperties` |
| `PGL_REG_MEM_ALLOC_RESULT` | `VkResult` from `vkAllocateMemory` |

The key difference is that ProtoGL's read path is **asynchronous** (command → staging → SPI read)
rather than synchronous mapping, though bidirectional Octal SPI makes the readback nearly instant.

### 9.8 Limitations and Future Work

| Limitation | Impact | Planned Resolution |
|---|---|---|
| ~~I2C readback bandwidth~~ | ~~Resolved~~ — Bidirectional Octal SPI provides ~40 MB/s reverse channel | ✅ Implemented: 16 KB framebuffer in ~0.3 ms |
| 4 KB staging buffer | Large reads must be split into multiple requests | Increase to 8–16 KB if SRAM budget allows |
| No scatter-gather writes | Multi-region uploads require separate commands | Possible CMD_MEM_WRITE_SG (0x37) in future |
| Handle limit (65534) | Exceeding requires explicit free | More than sufficient for embedded use |
| No GPU→Host notification of alloc completion | Host must poll via SPI read after sending alloc cmd | GPIO IRQ on completion (SMW mailbox notification) |

### 9.9 Resource Persistence & Flash Writeback

Large resources (textures, image sequences, font atlases) are always placed in external
VRAM first (Tier 1/2). The persistence strategy adapts to the detected VRAM type:

#### PSRAM (Volatile) → Flash Writeback

When the detected external memory is PSRAM (`QspiChipType::PSRAM_APS6408L` or similar),
data stored there is lost on power cycle. The host can request persistence via
`CMD_PERSIST_RESOURCE` (0x46):

1. GPU queues async writeback (max 4 pending, 4 KB/frame after `EndFrame`)
2. DMA copies resource from external VRAM → 4 KB SRAM staging buffer → flash page-program
3. Flash manifest updated with resource class, ID, offset, size, CRC-32
4. Mailbox slot 9 + IRQ notifies host of completion
5. On next boot: GPU checks manifest → auto-restores from flash → host skips upload

**Flash budget:** 512 KB reserved at end of 4 MB flash. Max 64 persisted entries.
Write throughput: ~200 KB/s (256-byte pages). 256 KB atlas persists in ~64 frames.

#### MRAM (Non-Volatile) → Zero-Cost Persistence

When the detected external memory is MRAM (`QspiChipType::MRAM_MR10Q010`), all data
in Tier 1/2 is inherently persistent. The GPU:

- Sets `PGL_CAP_NVRAM_VRAM` in capability flags at boot
- Stores a manifest header at MRAM offset 0x0000
- On reboot, rebuilds resource table from MRAM manifest — no DMA copy needed
- `CMD_PERSIST_RESOURCE` returns `ALREADY_PERSISTENT` (status 0x04) immediately

The host detects this via `QueryCapability()` and skips explicit persist calls.

#### Weight Table Extension

The per-resource-class weight table gains a `persist` column:

| Resource Class | PSRAM Weight | MRAM Weight | Auto-Persist |
|---|---|---|---|
| TEXTURE_DATA (large) | 800 | 100 | No (host decides) |
| IMAGE_SEQUENCE_ATLAS | 900 | 90 | No (host decides) |
| FONT_ATLAS | 700 | 80 | Yes (if MRAM) |
| SHADER_PROGRAM | 600 | 70 | Yes (if MRAM) |

Auto-persist resources are automatically written to flash (PSRAM) or left in-place
(MRAM) without requiring `CMD_PERSIST_RESOURCE`. The host can override with
`CMD_MEM_SET_RESOURCE_TIER` flags.

### 9.10 Direct Framebuffer Write

`CMD_WRITE_FRAMEBUFFER` (0x45) allows the host to write raw RGB565 pixels directly
into the GPU's back buffer or a compositing layer buffer. Key properties:

- **Bypasses entire GPU pipeline** — no rasterizer, QuadTree, Z-buffer, shaders
- **Writes to back buffer** — swapped to front on `EndFrame` as normal
- **Layer-aware** — `layerId` selects main buffer (0xFF) or layer 0-7
- **Clipped** — out-of-bounds pixels silently discarded
- **Composable** — direct writes + GPU-rendered content on different layers

**Use cases:**
- Pre-rendered content from host (camera passthrough, decoded images)
- Hybrid rendering (GPU 3D + host UI overlay)
- Full host-side rendering bypass (Option B from Architecture_Split.md)

Full wire format: see [ProtoGL_API_Spec.md](ProtoGL_API_Spec.md) §4.10.
Full design: see [Memory_Management_API.md](Memory_Management_API.md) §8.4.

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

At boot, the GPU firmware probes for external memory on each QSPI channel:

1. **QSPI Channel A (Tier 1):** Check `GpuConfig::QSPI_VRAM_MODE`. If mode ≠ `NONE`,
   initialize PIO2 SM0+SM1. For each chip select (CS0, CS1 if `QSPI_A_CHIP_COUNT ≥ 2`),
   run the RDID auto-detect sequence:
   - **Step 1 — MRAM RDID** (0x4B + mode byte 0xFF → read 5 bytes): Match against MR10Q010
     signature (0x076B111111). If matched, set profile to `PROFILE_MR10Q010`.
   - **Step 2 — PSRAM RDID** (0x9F → read 3 bytes): Check manufacturer byte
     (0x0D = AP Memory APS6408L, 0x5D = Espressif ESP-PSRAM). Set matching profile.
   - **Step 3 — Unknown fallback**: If RDID responded but ID is unrecognized, assign
     `UNKNOWN_DEVICE` type with conservative defaults.
   On success, set `PGL_CAP_QSPI_A_VRAM` in capability flags and `PGL_VRAM_QSPI_A_DETECTED`
   in extended status `vramTierFlags`.
2. **QSPI Channel B (Tier 2):** If `QSPI_VRAM_MODE == DUAL_CHANNEL`, initialize PIO2 SM2+SM3
   and repeat the same RDID sequence for Channel B's CS0 and CS1 (if `QSPI_B_CHIP_COUNT ≥ 2`).
   On success, set `PGL_CAP_QSPI_B_VRAM` and `PGL_VRAM_QSPI_B_DETECTED`.

Per-chip profiles (chip type, capacity, volatile/non-volatile, random-access penalty) are
stored in `QspiChannelInfo` structs and used by `MemTierManager::BaseWeight()` for
chip-aware weight table selection. Extended status reports total/free KB per channel.

The host calls `QueryCapability()` at startup and receives these flags. Convenience method
`HasExternalVram()` checks both channel flags in one call. The extended status reports total/free KB
per VRAM channel, allowing the host to monitor memory pressure during operation.

**Graceful degradation**: If no external memory is detected (RP2350A or unpopulated board),
the GPU operates in SRAM-only mode. All VRAM fields read zero. No code path change —
the M7 baseline path handles this transparently.

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
| PIO (HUB75, QSPI VRAM) | `clk_sys` | Yes — output frequency changes | `RecalculatePioClocks()` fixes dividers |
| I2C | `clk_peri` (USB PLL) | No | — |
| ADC (temp sensor) | `clk_adc` (USB PLL) | No | — |
| USB | `clk_usb` (USB PLL) | No | — |
| Timers | `clk_ref` (XOSC) | No | — |

---

## 11. Unified Display Frontend

> Full design: [Display_Frontend_Design.md](Display_Frontend_Design.md)

### 11.1 Problem Statement

Phase 1 hardcodes the display output to HUB75 (PIO0). As ProtoGL targets more display
technologies — DVI-D (PIO TMDS encoding), SPI LCDs, QSPI LCDs, parallel-interface panels —
the firmware needs a **unified display driver abstraction** so the rasterizer, shader pipeline,
and frame lifecycle are completely decoupled from the physical display interface.

### 11.2 Architecture Overview

```
                 ┌──────────────────────────┐
                 │      Rasterizer          │
                 │  (writes RGB565 to FB)   │
                 └───────────┬──────────────┘
                             │
                      ┌──────▼──────┐
                      │DisplayManager│
                      │ (routes FB) │
                      └──┬──────┬───┘
           ┌─────────────┼──────┼─────────────┐
     ┌─────▼─────┐ ┌────▼────┐ ┌──▼──────┐ ┌─▼────────┐
     │Hub75Driver│ │DviDriver│ │SpiDriver│ │QspiDriver│
     │(PIO0 BCM) │ │(PIO TMDS)│ │(SPI+DMA)│ │(PIO)     │
     │HOST-BUF   │ │HOST-BUF │ │SELF-BUF│ │SELF-BUF │
     └───────────┘ └─────────┘ └─────────┘ └──────────┘
```

**Key abstractions:**
- `DisplayDriver` — abstract base class: `Init()`, `SwapBuffers()`, `SetBrightness()`, `GetTimingInfo()`
- `DisplayManager` — singleton that manages 1–4 active drivers, framebuffer routing, format conversion
- Each driver self-reports its `DisplayCaps` (resolution, color depth, refresh rate, PIO usage)
- **Framebuffer Ownership:** `PGL_FB_HOST_OWNED` (HUB75, DVI-D) vs `PGL_FB_DISPLAY_OWNED` (SSD1306, SSD1331, SSD1351, ST7789, RM67162) — self-buffered displays use GDDRAM and only need dirty-rect push
- **DMA Chunk-Fill:** `DmaFillEngine` detects large single-color regions and offloads to DMA constant-fill, freeing the CPU for rendering
- **Chunk Skip:** Dirty-rect tracking skips unchanged scanlines during SPI push to GDDRAM displays

### 11.3 PIO Resource Allocation

| Driver | PIO Block | SMs Used | DMA Channels | Notes |
|--------|-----------|----------|--------------|-------|
| HUB75 | PIO0 | 2 SM | 2 | Current Phase 1 |
| Octal SPI (bidir) | PIO1 | 2 SM | 2 | SM0 RX + SM1 TX (bidirectional, fixed) |
| QSPI VRAM Ch-A | PIO2 | 2 SM | 2 | SM0 cmd/write + SM1 read (RP2350B only) |
| QSPI VRAM Ch-B | PIO2 | 2 SM | 2 | SM2 cmd/write + SM3 read (RP2350B only) |
| DVI-D | PIO0 | 3 SM | 3 | TMDS encoding (replaces HUB75) |
| I2C HUD | I2C1 | — | — | Hardware I2C peripheral (SSD1306/SSD1309) |
| SPI LCD | — | 0 SM | 1 | Hardware SPI peripheral |
| QSPI LCD | PIO0 | 1 SM | 1 | Shares PIO0 with HUB75 or DVI |

**Constraint:** PIO0 is shared — HUB75, DVI-D, and QSPI LCD cannot coexist.
The `DisplayManager` validates PIO allocation at `Init()` and rejects conflicting drivers.

**Constraint:** PIO2 is used by external VRAM (RP2350B). When DVI-D output is needed
and PIO2 is occupied by VRAM channels, DVI must use PIO0 (replacing HUB75).

### 11.4 Framebuffer Format Conversion

The rasterizer always writes RGB565. Drivers that need different formats perform conversion
during `SwapBuffers()`:
- **HUB75**: RGB565 → split BCM bitplanes (existing PIO program)
- **DVI-D**: RGB565 → RGB888 → TMDS 10b/8b encoding (PIO program)
- **SPI LCD**: RGB565 pass-through (most SPI displays accept RGB565)
- **QSPI LCD**: RGB565 pass-through or RGB666 expansion

### 11.5 Host API Extensions

New `PglDevice` methods for display configuration:

| Method | I2C Register | Description |
|--------|-------------|-------------|
| `SetDisplayMode(mode)` | 0x15 | Select active display driver |
| `QueryDisplayCaps()` | 0x16 | Get display capabilities |
| `SetMultiDisplayRoute(...)` | 0x17 | Configure multi-display routing |

New `PglEncoder` SPI commands:

| Opcode | Command | Description |
|--------|---------|-------------|
| 0x90 | `CMD_DISPLAY_CONFIGURE` | Set resolution, refresh, color depth |
| 0x91 | `CMD_DISPLAY_SET_REGION` | Define update region for partial refresh |
| 0x92 | `CMD_DISPLAY_SYNC` | Synchronize multi-display frame timing |

---

## 12. 2D Graphics & Multi-Layer Compositing

> Full design: [2D_Graphics_And_Compositing.md](2D_Graphics_And_Compositing.md)

### 12.1 Problem Statement

ProtoGL currently supports only 3D triangle rasterization. Many GUI/HUD elements —
status bars, text overlays, 2D sprites, notification banners — require efficient 2D
rendering without the overhead of the full 3D pipeline (transforms, QuadTree, Z-buffer).

### 12.2 Layer Architecture

```
  Layer Stack (back to front):
  ┌──────────────────────────────┐
  │ Layer 0: 3D Scene (existing) │  ← Full raster pipeline
  │ Layer 1: 2D Background       │  ← Rect/line/sprite
  │ Layer 2: 2D HUD Overlay      │  ← Text/icons/bars
  │ Layer 3: 2D Alert Banner     │  ← High-priority overlay
  └──────────────────────────────┘
         ↓ Compositor
  ┌──────────────────────────────┐
  │ Final Framebuffer → Display  │
  └──────────────────────────────┘
```

**Properties per layer:**
- Global opacity (0–255)
- Blend mode (alpha, additive, multiply)
- Viewport offset + clip rectangle
- Visibility flag (enable/disable without destroying)
- Z-order (sort key for compositing order)

### 12.3 2D Draw Commands (Opcodes 0xA0–0xAE)

| Opcode | Command | Parameters |
|--------|---------|------------|
| 0xA0 | `CMD_LAYER_CREATE` | id, width, height, format, blendMode, opacity |
| 0xA1 | `CMD_LAYER_DESTROY` | id |
| 0xA2 | `CMD_LAYER_SET_PROPS` | id, opacity, blendMode, offset, clipRect |
| 0xA3 | `CMD_DRAW_RECT_2D` | layerId, x, y, w, h, color, filled |
| 0xA4 | `CMD_DRAW_LINE_2D` | layerId, x0, y0, x1, y1, color |
| 0xA5 | `CMD_DRAW_CIRCLE_2D` | layerId, cx, cy, r, color, filled |
| 0xA6 | `CMD_DRAW_SPRITE` | layerId, x, y, textureId, flags |
| 0xA7 | `CMD_DRAW_TEXT` | layerId, x, y, fontId, color, string |
| 0xA8 | `CMD_DRAW_SPRITE_BATCH` | layerId, count, spriteArray[] |
| 0xA9 | `CMD_LAYER_CLEAR` | layerId, color |
| 0xAA | `CMD_DRAW_ROUNDED_RECT` | layerId, x, y, w, h, r, color, filled |
| 0xAB | `CMD_DRAW_ARC` | layerId, cx, cy, r, startAngle, endAngle, color |
| 0xAC | `CMD_DRAW_TRIANGLE_2D` | layerId, x0, y0, x1, y1, x2, y2, color |
| 0xAD | `CMD_BILLBOARD_SPRITE` | cameraId, worldPos, textureId, size, flags |
| 0xAE | `CMD_LAYER_SET_VISIBILITY`| layerId, visible |

### 12.4 GPU-Side Pipeline Integration

```
Frame Pipeline (Extended):
┌─────────────────────────────────────────────────────────────┐
│ 1. Parse commands (Core 0)                                  │
│ 2. Transform + Project + QuadTree (Core 0)                  │
│ 3. Rasterize 3D into Layer 0 (Core 0 + Core 1)             │
│ 4. Execute 2D draw lists into Layer 1..N (Core 0)           │
│ 4.5 Per-layer PSB shaders (Core 1, NEW)                     │
│    For layers with bound shader: run shader per-pixel        │
│    on layer framebuffer (blur, glow, color grade, etc.)     │
│ 5. Screen-space shaders per layer (optional)                │
│ 6. ──── Layer Compositor (Core 0) ────                      │
│     For each layer (back to front, by Z-order):             │
│       BlendLayer(finalFB, layer, opacity, blendMode)        │
│ 7. Swap framebuffers → Display                              │
└─────────────────────────────────────────────────────────────┘
```

**New in v0.7:** Step 4.5 runs programmable PSB shaders on individual 2D layers.
Shaders are bound via `CMD_SET_LAYER_SHADER` (0xB0) with host-supplied parameters.
This enables per-layer blur, glow, dissolve, scanline effects, etc. without re-issuing
2D draw commands. See [2D_Graphics_And_Compositing.md §6.3](2D_Graphics_And_Compositing.md) for details.

### 12.5 Memory Budget

Each 2D layer at 128×64 RGB565 costs 16 KB. With the SRAM cache arena repurposed:
- 1 layer (3D scene only): 0 KB extra (existing framebuffer)
- 2 layers: +16 KB
- 4 layers: +48 KB (max recommended for 520 KB SRAM budget)
- 8 layers: +112 KB (requires external PSRAM for layer storage)

---

## 13. Refined Memory Management

> Full design: [Memory_Management_API.md](Memory_Management_API.md)

### 13.1 Enhancements Over §8–§9

The §8 tiered model and §9 direct access API provide the foundation. The refined memory
management API adds four new subsystems:

| Subsystem | New Opcodes | Purpose |
|-----------|-------------|---------|
| **Memory Pools** | 0x38–0x3B | Pool-based allocation with fixed-size blocks for zero-fragmentation |
| **Defragmentation** | 0x3C | Online compaction of free-list allocators |
| **Streaming Upload** | 0x3D–0x3F | Multi-frame chunked upload with progress tracking |
| **Resource Binding** | 0x40–0x41 | Explicit bind/unbind of memory handles to resource slots |

### 13.2 Memory Pools

Pools provide predictable, fragmentation-free allocation for fixed-size objects:

```cpp
// Host-side: create a pool of 64-byte blocks in QSPI-A VRAM
encoder.MemPoolCreate(PGL_TIER_QSPI_A, /*blockSize=*/64, /*blockCount=*/256,
                      /*tag=*/0x0010);
// ... read I2C for pool handle ...

// Allocate a block from the pool (O(1) — free-list pop)
encoder.MemPoolAlloc(poolHandle, /*tag=*/0x0011);

// Free a block back to the pool (O(1) — free-list push)
encoder.MemPoolFree(poolHandle, blockHandle);
```

**Implementation:** Each pool is a contiguous allocation from the tier's free-list allocator.
Internally, a singly-linked free list of fixed-size blocks provides O(1) alloc/free with
zero fragmentation within the pool.

### 13.3 Defragmentation

The first-fit free-list allocators in Tier 0 and Tier 1 can fragment over time when
resources are created and destroyed in arbitrary order. The defragmentation command
compacts allocations:

```
GPU_CMD_MEM_DEFRAG (0x3C):
  tier       uint8_t    Target tier (0=SRAM, 1=QSPI-A, 2=QSPI-B)
  maxMoveKB  uint16_t   Max data to relocate (bounds CPU cost)
  flags      uint8_t    bit0: urgent (block frame), bit1: incremental
```

- **Incremental mode** (default): Moves at most `maxMoveKB` per frame. Multiple frames
  complete a full compaction. No frame stall.
- **Urgent mode**: Blocks the frame pipeline until compaction finishes. Use only during
  loading screens or initialization.

### 13.4 Streaming Upload

Large assets (textures > 16 KB, mesh banks) cannot fit in a single SPI frame. The streaming
API provides a managed multi-frame upload with progress tracking:

| Opcode | Command | Wire Fields |
|--------|---------|-------------|
| 0x3D | `CMD_STREAM_BEGIN` | streamId, tier, totalSize, tag |
| 0x3E | `CMD_STREAM_DATA` | streamId, offset, chunkSize, data[] |
| 0x3F | `CMD_STREAM_COMMIT` | streamId, resourceClass, resourceId |

The GPU assembles chunks into a staging buffer (or directly into the target tier address)
and signals completion via SPI read status (or I2C fallback). `CMD_STREAM_COMMIT` binds the fully-uploaded
data to a resource handle, making it available for rendering.

### 13.5 Resource Binding Model

Inspired by Vulkan's `vkBindBufferMemory`:

| Opcode | Command | Description |
|--------|---------|-------------|
| 0x40 | `CMD_MEM_BIND_RESOURCE` | Bind a memory handle to a resource (mesh, texture, material) |
| 0x41 | `CMD_MEM_UNBIND_RESOURCE` | Release the binding (resource becomes unresolvable) |

This separates allocation lifetime from resource lifetime — a texture can be destroyed
and re-created without freeing the underlying memory. Useful for texture atlases and
sprite sheet reuse.

### 13.6 Shared Memory Window (Bidirectional Access)

A 4 KB region in GPU SRAM at a fixed address serves as a **Shared Memory Window (SMW)**
accessible by both the host (via bidirectional Octal SPI read/write) and the GPU
(native SRAM access). The SMW is divided into:

- **Host→GPU Mailbox** (64 bytes, 16 × `uint32_t` slots) — quick parameter passing
- **GPU→Host Mailbox** (64 bytes, 16 × `uint32_t` slots) — status/FPS/temperature/free-SRAM
- **Bulk Staging Buffer** (3840 bytes) — GPU stages data for host to read at SPI speed

This provides **1000×+ speedup** over I2C for GPU→Host reads by using the same 8 data
lines in half-duplex mode (DIR pin on GPIO 10 controls direction). The GPU notifies
the host via a GPIO IRQ pulse when new data is available. A sequence counter ensures
coherency (no torn reads). See [Memory_Management_API.md §9.5](Memory_Management_API.md)
for details.

New SPI commands: `CMD_MEM_SMW_WRITE` (0x42), `CMD_MEM_SMW_READ` (0x43),
`CMD_MEM_STAGE_READ` (0x44). New SPI read commands: `SPI_READ_SMW` (0xEA) and
the full SPI read command range (0xE0–0xEB) that replaces all I2C register reads.

### 13.7 Diagnostic Register Access

All diagnostic registers are accessible via **bidirectional Octal SPI read commands**
(0xE0–0xE9 range, see [Communication_Protocol.md](Communication_Protocol.md)). I2C
access is retained as an optional fallback for backward compatibility.

| Register | I2C Addr | SPI Read Cmd | Size | Content |
|----------|----------|-------------|------|---------|
| `MEM_POOL_STATUS` | 0x18 | `0xE6` | 16 B | Per-pool: total/free blocks, largest consecutive run |
| `MEM_DEFRAG_STATUS` | 0x19 | `0xE7` | 8 B | Defrag progress: moved/total KB, fragments remaining |
| `MEM_STREAM_STATUS` | 0x1A | — | 12 B | Per-stream: received/total bytes, status code |
| `MEM_BINDING_TABLE` | 0x1B | — | 32 B | Active resource↔memory bindings (paginated) |
| `MEM_PERSIST_STATUS` | 0x1C | `0xEB` | 12 B | Persistence query result (per-resource or manifest summary) *(v0.7.1)* |

### 13.8 Vulkan Parallels (Extended)

| ProtoGL | Vulkan Equivalent |
|---------|-------------------|
| `CMD_MEM_POOL_CREATE` | `vkCreateDescriptorPool` (conceptually) |
| `CMD_MEM_POOL_ALLOC` | Sub-allocation from `VkDeviceMemory` |
| `CMD_MEM_DEFRAG` | `VMA` defragmentation pass |
| `CMD_STREAM_BEGIN/DATA/COMMIT` | Staging buffer + `vkCmdCopyBufferToImage` |
| `CMD_MEM_BIND_RESOURCE` | `vkBindBufferMemory` / `vkBindImageMemory` |
| Shared Memory Window | `VkBuffer` with `HOST_VISIBLE` \| `HOST_COHERENT` |
| `SmwReadMailbox` | `vkMapMemory` + read (fast coherent path) |
| `SmwStageRead` | `vkCmdCopyBuffer` to host-visible staging |

---

## 14. Related Documents

- [Architecture_Split.md](Architecture_Split.md) — High-level ESP32↔GPU split
- [ProtoGL_API_Spec.md](ProtoGL_API_Spec.md) — Full wire-format specification and host API
- [Communication_Protocol.md](Communication_Protocol.md) — Bidirectional Octal SPI protocol, I2C fallback register map
- [Display_Frontend_Design.md](Display_Frontend_Design.md) — Unified display driver interface (§11 detail)
- [2D_Graphics_And_Compositing.md](2D_Graphics_And_Compositing.md) — Multi-layer 2D graphics (§12 detail)
- [Memory_Management_API.md](Memory_Management_API.md) — Refined memory management (§13 detail)
- [Implementation_Plan.md](Implementation_Plan.md) — Phase-by-phase implementation milestones
- [Project_Schedule.md](Project_Schedule.md) — Week-by-week schedule with exit criteria
- [Shader_System_Design.md](Shader_System_Design.md) — PGLSL compiler and shader VM
- [Shader_Backend_And_Scheduler_Design.md](Shader_Backend_And_Scheduler_Design.md) — Per-pixel shader backend math