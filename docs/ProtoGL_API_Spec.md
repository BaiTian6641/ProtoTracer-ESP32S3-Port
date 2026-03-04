# ProtoGL API Specification v0.5

## 1. Overview

**ProtoGL** is a Vulkan-inspired, architecture-agnostic graphics command buffer API. It defines a serialized wire protocol between a **host** (scene/animation controller) and a **GPU** (rasterizer/display driver). The protocol is independent of the specific MCU on either side.

**Reference implementation:** ESP32-S3 (host) → RP2350 (GPU), but the wire format is designed to work with any host/GPU combination including custom RISC-V cores, FPGAs, or other ARM microcontrollers.

Core principles (shared with Vulkan):

1. **Explicit resource management** — the host creates, updates, and destroys GPU-side resources by ID.
2. **Command buffers** — all rendering work is recorded into a serialized byte stream, then submitted in one shot.
3. **Pipeline state** — the GPU maintains persistent state (meshes, materials, camera) between frames; only deltas are transmitted.
4. **Double-buffered presentation** — the GPU renders into a back buffer while the display scan-out reads the front buffer.

The API is designed to minimize per-frame bandwidth by separating **resource creation** (heavy, infrequent) from **draw submission** (lightweight, every frame).

### 1.1 Architecture Independence

ProtoGL makes **no assumptions** about the GPU core architecture:
- Wire format is a flat byte stream with explicit lengths — no pointer-sized fields.
- All data types are fixed-width (`uint8_t`, `uint16_t`, `uint32_t`, `float` IEEE 754).
- Packed structs are defined by byte offsets, not C struct layout. GPU implementations
  that lack hardware unaligned-access support (e.g., RISC-V without Zicclsm) should
  use `memcpy`-based deserialization (see `PglParser.h`).
- The GPU reports its architecture and capabilities via the I2C capability query (§4.3),
  allowing the host to adapt resource limits and optimization paths at runtime.

---

## 2. Concepts

### 2.1 Resources (GPU-Side State)

| Resource Type | ID Range | Description |
|---|---|---|
| `Mesh` | 0-255 | Indexed triangle mesh (vertices + index groups + optional UVs) |
| `Material` | 0-255 | Procedural shader definition (type + parameters) |
| `Texture` | 0-63 | Small raster image (for Image material etc.) |
| `PixelLayout` | 0-7 | Static pixel coordinate array (defines the physical LED positions per camera) |
| `Camera` | 0-3 | Camera state (transform, look offset, pixel layout binding) |

Resources persist on the RP2350 until explicitly destroyed. The host only sends creation commands on first use or when the resource definition changes.

### 2.2 Command Buffer

A **Command Buffer** is a linear byte array containing a sequence of commands. It is built on the ESP32-S3 and transmitted over Octal SPI as a single DMA burst each frame.

```
┌──────────────┐
│ Frame Header │  (sync word + frame number + total length)
├──────────────┤
│ Command 0    │  (e.g., CMD_CREATE_MESH)
├──────────────┤
│ Command 1    │  (e.g., CMD_UPDATE_MATERIAL)
├──────────────┤
│ ...          │
├──────────────┤
│ Command N    │  (CMD_END_FRAME)
├──────────────┤
│ CRC-16       │
└──────────────┘
```

### 2.3 Frame Lifecycle

```
Host (any MCU)                           GPU (any core)
─────────────────                        ────────────────
BeginFrame(N)                    
  CreateMesh(...)        ─────┐         
  CreateMaterial(...)         │ High-speed data bus
  DrawObject(...)             ├────────►  CommandParser
  DrawObject(...)             │            ├─ Update MeshTable
  SetCamera(...)              │            ├─ Update MaterialTable
EndFrame()               ─────┘            ├─ Build DrawList
                                           ├─ Project triangles
                                           ├─ Build QuadTree
                                           │
                                           ├─ Rasterize (multi-core optional)
                                           │
                                           ├─ Apply screen-space shaders
                                           │
                                           └─ Swap framebuffer → Display output
BeginFrame(N+1)                              (frame N visible on display)
  ...  (pipelined)
```

---

## 3. Wire Format

### 3.1 Byte Order
All multi-byte values are **little-endian**. This is native to ESP32-S3 (Xtensa), ARM Cortex-M, and standard RISC-V (RV32I/RV64I). Big-endian GPU implementations must byte-swap after reading.

### 3.2 Frame Header
| Offset | Size | Field | Value |
|---|---|---|---|
| 0 | 2 | `syncWord` | `0x55AA` |
| 2 | 4 | `frameNumber` | Monotonic frame counter |
| 6 | 4 | `totalLength` | Total bytes in this frame (header + commands + CRC) |
| 10 | 2 | `commandCount` | Number of commands in this frame |

**Total header: 12 bytes**

### 3.3 Command Header
Every command starts with:
| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `opcode` |
| 1 | 2 | `payloadLength` (bytes, excluding this 3-byte header) |

**Total per-command overhead: 3 bytes**

### 3.4 Frame Footer
| Offset | Size | Field |
|---|---|---|
| 0 | 2 | CRC-16/CCITT over entire frame (header + all commands) |

---

## 4. Command Reference

### 4.1 Resource Creation Commands

#### `CMD_CREATE_MESH` (0x01)
Creates or replaces a mesh resource on the GPU.

| Offset | Size | Field | Type |
|---|---|---|---|
| 0 | 2 | `meshId` | `uint16_t` |
| 2 | 2 | `vertexCount` | `uint16_t` |
| 4 | 2 | `triangleCount` | `uint16_t` |
| 6 | 1 | `flags` | bit0: `hasUV` |
| 7 | N×12 | `vertices[]` | `float x, y, z` × vertexCount |
| 7+N×12 | M×6 | `indices[]` | `uint16_t A, B, C` × triangleCount |
| (if hasUV) | 2 | `uvVertexCount` | `uint16_t` |
| | U×8 | `uvVertices[]` | `float u, v` × uvVertexCount |
| | M×6 | `uvIndices[]` | `uint16_t A, B, C` × triangleCount |

> **Wire encoding note:** Vertex indices are encoded as `uint16_t` (2 bytes each)
> on the wire, even though `IndexGroup` uses `unsigned int` (4 bytes) on the ESP32.
> This saves 50% bandwidth. The RP2350 also stores indices as `uint16_t` internally
> (max 65535 vertices per mesh is more than sufficient for LED matrix rendering).

**Size estimate:** 200-vertex mesh with 300 triangles, no UV:
`7 + 200×12 + 300×6 = 7 + 2400 + 1800 = 4207 bytes` (sent once)

#### `CMD_DESTROY_MESH` (0x02)
| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `meshId` |

#### `CMD_UPDATE_VERTICES` (0x03)
Updates vertex positions of an existing mesh (for morph targets / animations).

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `meshId` |
| 2 | 2 | `vertexCount` |
| 4 | N×12 | `vertices[]` — full replacement of vertex positions |

**Size estimate:** 200-vertex morph update = `4 + 2400 = 2404 bytes` per frame (only when morphing)

#### `CMD_UPDATE_VERTICES_DELTA` (0x04)
Sparse vertex update — only changed vertices.

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `meshId` |
| 2 | 2 | `deltaCount` |
| 4 | N×14 | `deltas[]` each: `uint16_t index, float x, y, z` |

#### `CMD_CREATE_MATERIAL` (0x10)
Creates or replaces a material resource.

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `materialId` |
| 2 | 1 | `materialType` (see Material Type Registry §5) |
| 3 | 1 | `blendMode` (Method enum: 0=Base, 1=Add, 2=Sub, ...) |
| 4 | varies | Type-specific parameters (see §5) |

#### `CMD_UPDATE_MATERIAL` (0x11)
Updates parameters of an existing material without changing its type.

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `materialId` |
| 2 | varies | Type-specific parameters |

#### `CMD_DESTROY_MATERIAL` (0x12)
| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `materialId` |

#### `CMD_CREATE_TEXTURE` (0x18)
Uploads a small raster image for use with Image materials.

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `textureId` |
| 2 | 2 | `width` |
| 4 | 2 | `height` |
| 6 | 1 | `format` (0=RGB565, 1=RGB888) |
| 7 | W×H×bpp | Pixel data |

#### `CMD_DESTROY_TEXTURE` (0x19)
| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `textureId` |

#### `CMD_SET_PIXEL_LAYOUT` (0x20)
Defines the physical pixel coordinate map for a camera (sent once at startup).

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `layoutId` |
| 1 | 2 | `pixelCount` |
| 3 | 1 | `flags` bit0: `isRectangular`, bit1: `reversed` |
| 4 | (if rect) 8+8+2+2 | `size` (Vector2D), `position` (Vector2D), `rowCount`, `colCount` |
| 4 | (if irreg) N×8 | `coordinates[]` float x, y × pixelCount |

---

### 4.2 Per-Frame Rendering Commands

#### `CMD_BEGIN_FRAME` (0x80)
Starts a new frame. Clears the draw list.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `frameNumber` |
| 4 | 4 | `frameTimeUs` (microseconds since last frame — for material animation) |

#### `CMD_DRAW_OBJECT` (0x81)
Submits a draw call: render a mesh with a material at a given transform.

The transform fields mirror `Transform.h`: position, rotation, scale plus the three
offset fields (`scaleRotationOffset`, `scaleOffset`, `rotationOffset`) needed by
the vertex transformation pipeline `Object3D::UpdateTransform()`.

| Offset | Size | Field | Type |
|---|---|---|---|
| 0 | 2 | `meshId` | `uint16_t` |
| 2 | 2 | `materialId` | `uint16_t` |
| 4 | 1 | `flags` | bit0: `enabled`, bit1: `hasVertexOverride` |
| 5 | 12 | `position` | `float x, y, z` |
| 17 | 16 | `rotation` | `float W, X, Y, Z` (Quaternion) |
| 33 | 12 | `scale` | `float x, y, z` |
| 45 | 16 | `baseRotation` | `float W, X, Y, Z` (Quaternion, default identity) |
| 61 | 16 | `scaleRotationOffset` | `float W, X, Y, Z` (Quaternion, default identity) |
| 77 | 12 | `scaleOffset` | `float x, y, z` (default 0,0,0) |
| 89 | 12 | `rotationOffset` | `float x, y, z` (default 0,0,0) |

**Size: 101 bytes per draw call.** For 20 objects per frame = 2020 bytes.

> The GPU applies the same vertex pipeline as `Object3D::UpdateTransform()`:
> 1. `v -= scaleOffset; v *= scale; v += scaleOffset`
> 2. `v -= rotationOffset; v = (rotation * baseRotation).Rotate(v); v += rotationOffset`
> 3. `v += position`
>
> If offsets are all zero/identity (common case), the GPU can skip the subtract/add steps.

If `hasVertexOverride` is set, an inline vertex array follows (for morph-animated objects).
These vertices replace the mesh's stored vertices for this draw call only:
| Offset | Size | Field |
|---|---|---|
| 101 | 2 | `vertexCount` |
| 103 | N×12 | `vertices[]` |

#### `CMD_SET_CAMERA` (0x82)
Sets the camera state for subsequent rendering.

| Offset | Size | Field | Type |
|---|---|---|---|
| 0 | 1 | `cameraId` | `uint8_t` |
| 1 | 1 | `pixelLayoutId` | `uint8_t` |
| 2 | 12 | `position` | `float x, y, z` |
| 14 | 16 | `rotation` | `float W, X, Y, Z` |
| 30 | 12 | `scale` | `float x, y, z` |
| 42 | 16 | `lookOffset` | `float W, X, Y, Z` (Quaternion) |
| 58 | 16 | `baseRotation` | `float W, X, Y, Z` — from CameraLayout::GetRotation() |
| 74 | 1 | `is2D` | `uint8_t` (bool) |

**Size: 75 bytes** (sent once per camera per frame; typically 1-2 cameras)

> **Note:** `baseRotation` is the orientation quaternion derived from CameraLayout's
> ForwardAxis/UpAxis enums. It is pre-computed on the ESP32 and sent as 4 floats
> to avoid replicating the axis-to-quaternion conversion on the RP2350.

#### `CMD_SET_SHADER` (0x83)
Sets a screen-space shader for a camera. Up to 4 shaders per camera (slots 0–3),
applied in slot order after rasterization. Set `shaderClass` to `0x00` (NONE) to clear a slot.

| Offset | Size | Field | Type |
|---|---|---|---|
| 0 | 1 | `cameraId` | `uint8_t` |
| 1 | 1 | `shaderSlot` | `uint8_t` (0–3) |
| 2 | 1 | `shaderClass` | `uint8_t` (see §5.9 Shader Class Registry) |
| 3 | 4 | `intensity` | `float` (0.0–1.0, shader mix factor) |
| 7 | 20 | `params` | Class-specific parameters (see §5.9) |

**Size: 27 bytes.** Typically 0–2 per frame (shaders don't change every frame).

> **Design note:** Time-based animated shaders (convolution with `anglePeriod`,
> displacement oscillators) run their own waveform generators on the GPU using
> `frameTimeUs` from `CMD_BEGIN_FRAME`. The host only sends static configuration
> (periods, axis, waveform type) — not per-frame phase values. This saves bandwidth
> and ensures smooth animation even if frames are dropped.

#### `CMD_END_FRAME` (0x8F)
Signals command buffer is complete. GPU begins rendering.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `frameNumber` (echo, for verification) |

---

### 4.3 Configuration Commands (I2C)

These are sent over the I2C bus, not the Octal SPI command buffer:

| Reg | Name | Payload | Direction |
|---|---|---|---|
| 0x01 | `SET_BRIGHTNESS` | 1 byte (0-255) | Host → GPU |
| 0x02 | `SET_PANEL_CONFIG` | 4 bytes: W(16), H(16) | Host → GPU |
| 0x03 | `SET_SCAN_RATE` | 1 byte (8/16/32) | Host → GPU |
| 0x04 | `CLEAR_DISPLAY` | 0 bytes | Host → GPU |
| 0x05 | `SET_COLOR_ORDER` | 1 byte (0=RGB, 1=RBG, ...) | Host → GPU |
| 0x06 | `SET_GAMMA_TABLE` | 1 byte (0=linear, 1=CIE, 2=sRGB) | Host → GPU |
| 0x07 | `SET_CLOCK_SPEED` | 1 byte (MHz: 40/64/80) | Host → GPU |
| 0x09 | `CAPABILITY_QUERY` | Read: 16 bytes | GPU → Host |
| 0x0A | `STATUS_REQUEST` | Read: 8 bytes | GPU → Host |
| 0x0B | `RESET_GPU` | 0 bytes | Host → GPU |

**Capability Response (0x09):**
| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `protoVersion` (ProtoGL version, currently 5 = v0.5) |
| 1 | 1 | `gpuArch` (PglGpuArch enum: 0x01=ARM-CM33, 0x02=RISC-V Hazard3, 0x03=RISC-V custom, 0x10=FPGA, 0x20=RV32I, 0x21=RV32IMF) |
| 2 | 1 | `coreCount` (number of render cores) |
| 3 | 1 | `coreFreqMHz` (clock speed in MHz) |
| 4 | 2 | `sramKB` (total SRAM in KB) |
| 6 | 2 | `maxVertices` |
| 8 | 2 | `maxTriangles` |
| 10 | 2 | `maxMeshes` |
| 12 | 2 | `maxMaterials` |
| 14 | 1 | `maxTextures` |
| 15 | 1 | `flags` bit0: `hasHWFloat`, bit1: `hasUnalignedAccess`, bit2: `hasDSP`, bit3: `hasSIMD` |

The host should query capabilities on startup and use the reported limits instead of
hardcoded `PGL_MAX_*` constants when deciding resource allocation.

**Status Response (0x0A):**
| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `currentFPS` (uint16) |
| 2 | 2 | `droppedFrames` (uint16) |
| 4 | 2 | `freeMemory` (uint16, in bytes/16) |
| 6 | 1 | `temperature` (°C, int8) |
| 7 | 1 | `flags` bit0: `renderBusy`, bit1: `bufferOverflow` |

---

### 4.4 GPU Memory Access Commands (SPI 0x30–0x3F)

These SPI commands allow the host to directly access GPU device memory across all
tiers (SRAM, PIO2 external, QSPI MRAM/PSRAM). See GPU_API_Design.md §9 for design rationale.

#### `CMD_MEM_WRITE` (0x30)

Write raw bytes to a specific GPU memory tier and address.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `tier` | `PglMemTier` (0=SRAM, 1=PIO2, 2=QSPI) |
| 1 | 4 | `address` | Byte offset within tier address space |
| 5 | 4 | `size` | Number of data bytes that follow |
| 9 | N | `data[]` | Raw bytes to write |

#### `CMD_MEM_READ_REQUEST` (0x31)

Request GPU to stage memory for I2C readback via `MEM_READ_DATA` (0x0E).

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `tier` | Source `PglMemTier` |
| 1 | 4 | `address` | Byte offset within tier |
| 5 | 2 | `size` | Bytes to stage (max 4096) |

#### `CMD_MEM_SET_RESOURCE_TIER` (0x32)

Tier placement hint for an existing resource.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `resourceClass` | 0=mesh, 1=material, 2=texture, 3=layout, 4=generic |
| 1 | 2 | `resourceId` | Handle of the resource |
| 3 | 1 | `preferredTier` | `PglMemTier` (0xFF=AUTO) |
| 4 | 1 | `flags` | bit0: pinned (never auto-migrate) |

#### `CMD_MEM_ALLOC` (0x33)

Allocate a region in a specific GPU memory tier. Result available via I2C 0x0F.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `tier` | Target `PglMemTier` (AUTO not allowed) |
| 1 | 4 | `size` | Bytes to allocate |
| 5 | 2 | `tag` | User-defined identification tag |

#### `CMD_MEM_FREE` (0x34)

Free a previously allocated GPU memory region.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 2 | `handle` | `PglMemHandle` from prior alloc |

#### `CMD_FRAMEBUFFER_CAPTURE` (0x35)

Snapshot the framebuffer for I2C readback.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `bufferSelect` | 0=front (displayed), 1=back (in-progress) |
| 1 | 1 | `format` | 0=RGB565 (native), 1=RGB888 (expanded) |

#### `CMD_MEM_COPY` (0x36)

GPU-internal copy between memory regions or tiers.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `srcTier` | Source `PglMemTier` |
| 1 | 4 | `srcAddress` | Source byte offset |
| 5 | 1 | `dstTier` | Destination `PglMemTier` |
| 6 | 4 | `dstAddress` | Destination byte offset |
| 10 | 4 | `size` | Bytes to copy |

---

### 4.5 GPU Memory I2C Registers (0x0C–0x0F)

| Reg | Name | Payload | Direction |
|---|---|---|---|
| 0x0C | `MEM_TIER_INFO` | Read: 20 bytes (`PglMemTierInfoResponse`) | GPU → Host |
| 0x0D | `MEM_READ_ADDR` | Write: 7 bytes (`PglMemReadSetup`) | Host → GPU |
| 0x0E | `MEM_READ_DATA` | Read: 32 bytes (auto-incrementing) | GPU → Host |
| 0x0F | `MEM_ALLOC_RESULT` | Read: 7 bytes (`PglMemAllocResult`) | GPU → Host |

**Memory Tier Info (0x0C):**
| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `sramTotalKB` |
| 2 | 2 | `sramFreeKB` |
| 4 | 2 | `opiTotalKB` (PIO2 total; 0 if absent) |
| 6 | 2 | `opiFreeKB` (PIO2 free) |
| 8 | 1 | `opiEnabled` (1 if PIO2 driver active) |
| 9 | 2 | `qspiTotalKB` (0 if absent) |
| 11 | 2 | `qspiFreeKB` |
| 13 | 1 | `qspiEnabled` (1 if driver active) |
| 14 | 2 | `cachedEntries` |
| 16 | 2 | `totalManagedAllocs` |
| 18 | 1 | `cacheHitRate` (0-100%) |
| 19 | 1 | reserved |

**Alloc Result (0x0F):**
| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `handle` (`PglMemHandle`, 0xFFFF on failure) |
| 2 | 4 | `address` (tier-relative byte address) |
| 6 | 1 | `status` (0x00=OK, 0x01=OOM, 0x02=InvalidTier, 0x03=Disabled, 0x04=HandleExhausted) |

---

## 5. Material Type Registry

Each material type has a unique ID and fixed parameter layout. Parameters are sent with `CMD_CREATE_MATERIAL` and can be updated with `CMD_UPDATE_MATERIAL`.

### 5.1 Simple Materials

| Type ID | Name | Parameters |
|---|---|---|
| 0x00 | `SimpleMaterial` | `uint8_t R, G, B` (3 bytes) |
| 0x01 | `NormalMaterial` | (no params — color derived from surface normal) |
| 0x02 | `DepthMaterial` | `uint8_t nearR, nearG, nearB, farR, farG, farB; float nearZ, farZ` (14 bytes) |

### 5.2 Gradient Materials

| Type ID | Name | Parameters |
|---|---|---|
| 0x10 | `GradientMaterial` | `uint8_t stopCount; [float position, uint8_t R, G, B][] × stopCount; uint8_t axis (0=X,1=Y,2=Z); float rangeMin, rangeMax` |

### 5.3 Lighting Materials

| Type ID | Name | Parameters |
|---|---|---|
| 0x20 | `LightMaterial` | `float lightDirX, lightDirY, lightDirZ; uint8_t ambientR, ambientG, ambientB; uint8_t diffuseR, diffuseG, diffuseB` (18 bytes) |

### 5.4 Noise / Procedural Materials

| Type ID | Name | Parameters |
|---|---|---|
| 0x30 | `SimplexNoise` | `float scaleX, scaleY, scaleZ, speed; uint8_t colorAR, colorAG, colorAB, colorBR, colorBG, colorBB` (22 bytes) |
| 0x31 | `RainbowNoise` | `float scale, speed` (8 bytes) |

### 5.5 Texture Materials

| Type ID | Name | Parameters |
|---|---|---|
| 0x40 | `ImageMaterial` | `uint16_t textureId; float offsetX, offsetY, scaleX, scaleY` (12 bytes) — references a `CMD_CREATE_TEXTURE` resource |

### 5.6 Composite Materials

| Type ID | Name | Parameters |
|---|---|---|
| 0x50 | `CombineMaterial` | `uint16_t materialIdA, materialIdB; uint8_t blendMode; float opacity` (9 bytes) |
| 0x51 | `MaterialMask` | `uint16_t baseMaterialId, maskMaterialId; float threshold` (8 bytes) |
| 0x52 | `MaterialAnimator` | `uint16_t materialIdA, materialIdB; uint8_t interpMode; float ratio` (9 bytes) — ratio updated each frame |

### 5.7 Blend Mode Enum (for CombineMaterial and CMD_CREATE_MATERIAL)

| Value | Name | Description |
|---|---|---|
| 0 | `Base` | Default / passthrough |
| 1 | `Add` | Additive blending |
| 2 | `Subtract` | Subtractive blending |
| 3 | `Multiply` | Multiplicative blending |
| 4 | `Divide` | Division blending |
| 5 | `Darken` | Minimum of both |
| 6 | `Lighten` | Maximum of both |
| 7 | `Screen` | Screen blending |
| 8 | `Overlay` | Overlay blending |
| 9 | `SoftLight` | Soft light blending |
| 10 | `Replace` | Full replacement |
| 11 | `EfficientMask` | Mask-based compositing |

### 5.8 Special / Pre-Rendered Materials

| Type ID | Name | Parameters |
|---|---|---|
| 0xF0 | `PreRenderedTexture` | `uint16_t textureId` — the ESP32 pre-renders complex materials (TextEngine, SpectrumAnalyzer, AudioReactiveGradient, Clock) into a texture and uploads via `CMD_CREATE_TEXTURE`. This is the GPU equivalent of a "render-to-texture" pass done on the CPU side. |

### 5.9 Shader Class Registry (for `CMD_SET_SHADER`)

Three general shader classes.  Each defines interpretation for the 20-byte `params[]` block.
`intensity` (float, 0.0–1.0) is a global mix/strength factor consumed by the GPU.

#### 5.9.1 CONVOLUTION (0x01)

Configurable blur/smooth kernel.  `params` layout (`PglShaderParamsConvolution`, 16 bytes):

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `kernelShape` | `PglKernelShape`: BOX (0), GAUSSIAN (1), TRIANGLE (2) |
| 1 | 1 | `radius` | Kernel half-width in pixels (1–32) |
| 2 | 1 | `separable` | 0 = 1D directional, 1 = 2D separable (4-neighbour) |
| 3 | 1 | `_pad` | Reserved |
| 4 | 4 | `angle` | Direction angle in degrees (0° = horizontal, 90° = vertical) |
| 8 | 4 | `anglePeriod` | If > 0, angle auto-rotates with this period (seconds) |
| 12 | 4 | `sigma` | Gaussian σ, or smoothing weight for separable mode |

**ProtoTracer equivalents:**
- Horizontal blur → `angle=0, separable=0`
- Vertical blur → `angle=90, separable=0`
- Radial blur → `anglePeriod > 0` (auto-rotating direction from centre)
- Anti-aliasing → `separable=1, radius=1, sigma=smoothing_weight`

#### 5.9.2 DISPLACEMENT (0x02)

Coordinate-space warp with optional per-channel chromatic split.
`params` layout (`PglShaderParamsDisplacement`, 20 bytes):

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `axis` | `PglDisplacementAxis`: X (0), Y (1), RADIAL (2) |
| 1 | 1 | `perChannel` | 0 = uniform, 1 = chromatic R/G/B split (120° apart) |
| 2 | 1 | `amplitude` | Max displacement in pixels (1–32) |
| 3 | 1 | `waveform` | `PglWaveform`: SAWTOOTH (0), SINE (1), TRIANGLE (2), SQUARE (3) |
| 4 | 4 | `period` | Primary oscillator period (seconds), 0 = static |
| 8 | 4 | `frequency` | Spatial frequency multiplier (default 1.0) |
| 12 | 4 | `phase1Period` | Secondary oscillator (for radial mode) |
| 16 | 4 | `phase2Period` | Tertiary oscillator (for radial mode) |

**ProtoTracer equivalents:**
- PhaseOffsetX → `axis=X, perChannel=1, waveform=SINE`
- PhaseOffsetY → `axis=Y, perChannel=1, waveform=SINE`
- PhaseOffsetR → `axis=RADIAL, perChannel=1, waveform=SINE`

#### 5.9.3 COLOR_ADJUST (0x03)

Per-pixel colour transform.  `params` layout (`PglShaderParamsColorAdjust`, 12 bytes):

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `operation` | `PglColorAdjustOp` (see below) |
| 1 | 3 | `_pad` | Reserved |
| 4 | 4 | `strength` | Primary control (meaning varies per operation) |
| 8 | 4 | `param2` | Secondary control (gamma exponent, etc.) |

Operations:

| Op | Name | `strength` meaning | `param2` meaning |
|---|---|---|---|
| 0x00 | EDGE_FEATHER | Dim factor (0=transparent, 1=no dimming) | — |
| 0x01 | THRESHOLD | Luminance cutoff (0.0–1.0) | — |
| 0x02 | GAMMA | — | Gamma exponent (default 2.2) |
| 0x03 | INVERT | — | — |
| 0x04 | BRIGHTNESS | Delta (-1.0 to +1.0) | — |
| 0x05 | CONTRAST | Scale (0=grey, 1=unchanged, 2=double) | — |
| 0x06 | EDGE_DETECT | Sobel magnitude scale | — |

**ProtoTracer equivalents:**
- EdgeFeather → `operation=EDGE_FEATHER`

> **GPU-side animation:** CONVOLUTION shaders with `anglePeriod > 0` and DISPLACEMENT
> shaders with `period > 0` run waveform-driven oscillators on the GPU using
> accumulated wall time from `CMD_BEGIN_FRAME::frameTimeUs`. The host sends only
> static configuration at setup time — not per-frame phase values.

> **Scratch buffer:** Shaders that need read/write separation (CONVOLUTION, DISPLACEMENT,
> COLOR_ADJUST with EDGE_DETECT) use the GPU's Z-buffer as scratch memory after
> rasterization completes. This avoids any extra SRAM allocation.

---

## 6. Per-Frame Bandwidth Analysis

### Typical Frame: Protogen Face Animation (20 objects, 500 total triangles)

| Command | Count | Size | Total |
|---|---|---|---|
| `CMD_BEGIN_FRAME` | 1 | 11 bytes | 11 |
| `CMD_DRAW_OBJECT` (no morph) | 15 | 104 bytes | 1560 |
| `CMD_DRAW_OBJECT` (with 100-vert morph) | 5 | 104 + 1202 | 6530 |
| `CMD_UPDATE_MATERIAL` | 3 | 3+~10 bytes | 39 |
| `CMD_SET_CAMERA` | 1 | 78 bytes | 78 |
| `CMD_END_FRAME` | 1 | 7 bytes | 7 |
| Frame Header + CRC | 1 | 14 bytes | 14 |
| **Total** | | | **~8.2 KB** |

**At 80 MB/s Octal SPI: transfer time = 0.103 ms** (negligible)

### Worst Case: Full mesh re-upload (2000 vertices)

| Command | Size |
|---|---|
| `CMD_CREATE_MESH` (2000 verts, 1000 tris) | 30,007 bytes |
| **At 80 MB/s: 0.375 ms** | Still under 1 ms |

---

## 7. Host-Side API (C++ Library)

The host-side library (`lib/ProtoGL/`) provides a C++ API for building and submitting
command buffers. It is currently implemented for ESP32-S3 (PlatformIO/Arduino) but
the encoder and types are platform-neutral C++ and can be compiled on any host.

```cpp
// lib/ProtoGL/ProtoGL.h

class ProtoGLDevice; // forward

// --- Resource Handles ---
using PglMesh     = uint16_t;
using PglMaterial = uint16_t;
using PglTexture  = uint16_t;
using PglCamera   = uint8_t;
using PglLayout   = uint8_t;

// --- Initialization ---
ProtoGLDevice* pglCreateDevice(const PglDeviceConfig& config);
void           pglDestroyDevice(ProtoGLDevice* device);

// --- Resource Management ---
PglMesh     pglCreateMesh(ProtoGLDevice* dev, const Vector3D* vertices, uint16_t vertexCount,
                           const IndexGroup* indices, uint16_t triangleCount);
void        pglDestroyMesh(ProtoGLDevice* dev, PglMesh mesh);
void        pglUpdateMeshVertices(ProtoGLDevice* dev, PglMesh mesh,
                                   const Vector3D* vertices, uint16_t vertexCount);

PglMaterial pglCreateMaterial(ProtoGLDevice* dev, uint8_t type, const void* params, uint16_t paramSize);
void        pglUpdateMaterial(ProtoGLDevice* dev, PglMaterial mat, const void* params, uint16_t paramSize);
void        pglDestroyMaterial(ProtoGLDevice* dev, PglMaterial mat);

PglTexture  pglCreateTexture(ProtoGLDevice* dev, uint16_t width, uint16_t height,
                              uint8_t format, const void* pixelData);
void        pglDestroyTexture(ProtoGLDevice* dev, PglTexture tex);

PglLayout   pglCreatePixelLayout(ProtoGLDevice* dev, const Vector2D* coords, uint16_t pixelCount);
PglLayout   pglCreateRectPixelLayout(ProtoGLDevice* dev, Vector2D size, Vector2D position,
                                      uint16_t rowCount, uint16_t colCount);

// --- Frame Recording ---
void  pglBeginFrame(ProtoGLDevice* dev);

void  pglSetCamera(ProtoGLDevice* dev, PglCamera cam, PglLayout layout,
                    const Vector3D& position, const Quaternion& rotation,
                    const Vector3D& scale, const Quaternion& lookOffset, bool is2D);

void  pglDrawObject(ProtoGLDevice* dev, PglMesh mesh, PglMaterial material,
                     const Vector3D& position, const Quaternion& rotation,
                     const Vector3D& scale, bool enabled);

// Draw with inline vertex override (morph targets)
void  pglDrawObjectMorphed(ProtoGLDevice* dev, PglMesh mesh, PglMaterial material,
                            const Vector3D& position, const Quaternion& rotation,
                            const Vector3D& scale, bool enabled,
                            const Vector3D* morphedVertices, uint16_t vertexCount);

void  pglSetShader(ProtoGLDevice* dev, PglCamera cam, uint8_t slot,
                    uint8_t shaderClass, float intensity,
                    const void* params, uint8_t paramSize);
void  pglClearShader(ProtoGLDevice* dev, PglCamera cam, uint8_t slot);

// --- Shader class helpers ---
void  pglSetConvolution(ProtoGLDevice* dev, PglCamera cam, uint8_t slot,
                         float intensity, PglKernelShape kernel, uint8_t radius,
                         bool separable, float angleDeg,
                         float anglePeriod, float sigma);
void  pglSetDisplacement(ProtoGLDevice* dev, PglCamera cam, uint8_t slot,
                          float intensity, PglDisplacementAxis axis,
                          bool perChannel, uint8_t amplitude, PglWaveform waveform,
                          float period, float frequency,
                          float phase1Period, float phase2Period);
void  pglSetColorAdjust(ProtoGLDevice* dev, PglCamera cam, uint8_t slot,
                         float intensity, PglColorAdjustOp operation,
                         float strength, float param2);

// --- Convenience wrappers (ProtoTracer-compatible) ---
void  pglSetHorizontalBlur(ProtoGLDevice* dev, PglCamera cam, uint8_t slot,
                            float intensity, uint8_t radius);
void  pglSetVerticalBlur(ProtoGLDevice* dev, PglCamera cam, uint8_t slot,
                          float intensity, uint8_t radius);
void  pglSetRadialBlur(ProtoGLDevice* dev, PglCamera cam, uint8_t slot,
                        float intensity, uint8_t radius, float rotationPeriod);
void  pglSetAntiAliasing(ProtoGLDevice* dev, PglCamera cam, uint8_t slot,
                          float intensity, float smoothing);
void  pglSetEdgeFeather(ProtoGLDevice* dev, PglCamera cam, uint8_t slot,
                         float intensity, float featherStrength);
void  pglSetPhaseOffsetR(ProtoGLDevice* dev, PglCamera cam, uint8_t slot,
                          float intensity, uint8_t amplitude,
                          float rotPeriod, float phase1Period, float phase2Period);
void  pglSetPhaseOffsetX(ProtoGLDevice* dev, PglCamera cam, uint8_t slot,
                          float intensity, uint8_t amplitude);
void  pglSetPhaseOffsetY(ProtoGLDevice* dev, PglCamera cam, uint8_t slot,
                          float intensity, uint8_t amplitude);

void  pglEndFrame(ProtoGLDevice* dev);  // triggers DMA transfer

// --- Query ---
PglStatus pglQueryStatus(ProtoGLDevice* dev);  // reads via I2C

// --- Configuration ---
void  pglSetBrightness(ProtoGLDevice* dev, uint8_t brightness);
void  pglSetPanelConfig(ProtoGLDevice* dev, uint16_t width, uint16_t height, uint8_t scanRate);
void  pglSetGammaTable(ProtoGLDevice* dev, uint8_t table);
```

### Usage Example (replacing SmartMatrixHUB75 controller)

This example uses the ESP32-S3 as host and assumes a ProtoGL GPU on the
other end of the Octal SPI link. The GPU could be RP2350 (ARM or RISC-V
mode), a custom RISC-V SoC, or any other implementation.

```cpp
// In GPUDriverController::Initialize()
device = pglCreateDevice({
    .spiClockMHz = 80,
    .i2cAddress = 0x3C,
    .rdyPin = GPIO_NUM_xx,
});

// Upload static meshes (once)
for (auto& obj : scene.objects) {
    auto tg = obj->GetTriangleGroup();
    meshHandles[i] = pglCreateMesh(device,
        tg->GetVertices(), tg->GetVertexCount(),
        tg->GetIndexGroup(), tg->GetTriangleCount());
}

// Upload pixel layout (once)
layout = pglCreateRectPixelLayout(device, {128, 64}, {0, 0}, 64, 128);

// Per-frame in Render():
pglBeginFrame(device);

pglSetCamera(device, 0, layout,
    camera->GetTransform()->GetPosition(),
    camera->GetTransform()->GetRotation(),
    camera->GetTransform()->GetScale(),
    lookOffset, false);

for (int i = 0; i < objectCount; i++) {
    auto* obj = objects[i];
    auto* t = obj->GetTransform();

    if (obj->IsMorphed()) {
        pglDrawObjectMorphed(device, meshHandles[i], matHandles[i],
            t->GetPosition(), t->GetRotation(), t->GetScale(),
            obj->IsEnabled(),
            obj->GetTriangleGroup()->GetVertices(),
            obj->GetTriangleGroup()->GetVertexCount());
    } else {
        pglDrawObject(device, meshHandles[i], matHandles[i],
            t->GetPosition(), t->GetRotation(), t->GetScale(),
            obj->IsEnabled());
    }
}

pglEndFrame(device);  // DMA fires, ESP32 is free to compute next frame
```

---

## 8. GPU-Side Architecture (Reference Implementation)

This section describes the **reference implementation** using the RP2350 (dual-core ARM Cortex-M33
or Hazard3 RISC-V). Other GPU implementations (custom RISC-V, FPGA, etc.) should adapt the
execution model to their specific architecture while maintaining the same command parsing semantics.

### 8.1 Internal State

```
┌─────────────────────────────────────────────────┐
│ GPU Internal State (SRAM)                       │
│                                                 │
│  MeshTable[256]        ← vertex/index arrays    │
│  MaterialTable[256]    ← type + params          │
│  TextureTable[64]      ← small raster images    │
│  PixelLayoutTable[8]   ← static coord arrays    │
│  CameraState[4]        ← transform + layout ref │
│                                                 │
│  DrawList[MAX_DRAW=64] ← per-frame draw calls   │
│  drawCount             ← count of draw calls    │
│                                                 │
│  framebuffer[2][W×H]   ← double-buffered RGB565 │
│  zbuffer[W×H]          ← float depth buffer     │
│                                                 │
│  quadTree              ← spatial acceleration    │
│  triangle2D_pool[]     ← projected triangles     │
└─────────────────────────────────────────────────┘
```

### 8.2 Core Execution Model (Reference: RP2350 Dual-Core)

This is the recommended scheduling for a dual-core GPU. Single-core GPUs should
execute the same stages sequentially. Quad-core GPUs can split rasterization into
four screen quadrants.

```
Core 0                              Core 1
──────                              ──────
 │                                   │
 ├─ DMA IRQ: new frame arrived       ├─ (idle, waiting on signal)
 ├─ Parse CMD_BEGIN_FRAME             │
 ├─ Parse CMD_DRAW_OBJECT ×N         │
 ├─ Parse CMD_SET_CAMERA              │
 ├─ Parse CMD_END_FRAME              │
 │                                   │
 ├─ Apply transforms to meshes       │
 ├─ Project all triangles to 2D      │
 ├─ Build QuadTree                   │
 │                                   │
 ├─ Signal: START_RENDER ─────────►├─ Receive: START_RENDER
 │                                   │
 ├─ Rasterize Y=[0, H/2)            ├─ Rasterize Y=[H/2, H)
 │                                   │
 ├─ Barrier: wait ◄───────────────├─ Signal: CORE1_DONE
 │                                   │
 ├─ Apply screen-space shaders        │
 │   (Z-buffer repurposed as scratch) │
 ├─ Swap framebuffer pointer          │
 ├─ (Display scan-out continues)     │
 └─ Loop                             └─ Loop (wait on signal)
```

> **RISC-V note:** The multicore FIFO / atomic flag mechanism is RP2350-specific.
> A RISC-V GPU implementation should use its own inter-core IPC (e.g., CLINT
> software interrupts, shared-memory mailbox, or hardware semaphores).

### 8.3 Memory Budget (128×64 panel, worst case — RP2350 reference)

| Allocation | Size |
|---|---|
| Framebuffer A (RGB565) | 128×64×2 = 16,384 B |
| Framebuffer B (RGB565) | 16,384 B |
| Z-Buffer (float32) | 128×64×4 = 32,768 B |
| Scene state pools (vtx, idx, UV, tex, layout) | ~138,000 B |
| Scene slot arrays (mesh, mat, tex, layout, cam, draw) | ~40,000 B |
| Triangle2D pool (max 1024) | 1024×80 ≈ 81,920 B |
| Transform scratch (max 2048 verts) | 2048×12 = 24,576 B |
| QuadTree nodes (max 512) | 512×44 ≈ 22,528 B |
| SPI ring buffer | 32,768 B |
| SDK / stack / misc | ~60,000 B |
| **TOTAL** | **~460 KB** |
| **RP2350 SRAM** | **520 KB** |
| **Headroom** | **~60 KB (12%)** |

> A GPU with more or less SRAM can adjust `PGL_MAX_*` limits accordingly and
> report its actual limits via the capability query (§4.3).
>
> **External memory (optional):** GPU implementations may include tiered external
> memory (PIO2-based OPI PSRAM or QSPI MRAM, plus QMI CS1 MRAM or PSRAM) to extend
> resource capacity beyond SRAM. The RP2350 reference GPU supports three PIO2 modes:
> OPI PSRAM (8 MB), Dual QSPI MRAM (2×128 KB = 256 KB), or Single QSPI MRAM (128 KB).
> Additionally, auto-detected QSPI CS1 memory — either MRAM (MR10Q010) or PSRAM
> (APS6408L) — provides Tier 2 with chip-aware dual weight tables.
> See `GPU_API_Design.md` §8 for the full design.

---

## 9. Error Handling

| Condition | GPU Behavior |
|---|---|
| CRC mismatch | Drop frame, render last good frame, increment `droppedFrames` counter |
| Unknown opcode | Skip command (advance by `payloadLength`), set `flags.bufferOverflow` |
| Resource ID overflow | Ignore command, set error flag |
| SPI clock loss | PIO timeout → re-sync on next `0x55AA` header |
| Framebuffer not ready | `RDY` pin stays low; host must wait or drop submission |

---

## 10. Version History

| Version | Date | Changes |
|---|---|---|
| 0.1 | 2026-03-04 | Initial draft — opcodes, wire format, material registry, dual-core model |
| 0.2 | 2026-03-04 | **API FROZEN.** Added Transform offset fields (scaleOffset, rotationOffset, scaleRotationOffset, baseRotation) to CMD_DRAW_OBJECT. Added CameraLayout baseRotation to CMD_SET_CAMERA. Documented blend mode enum (12 values). Clarified IndexGroup uint16 wire encoding. |
| 0.3 | 2026-03-04 | **Architecture-agnostic update.** Added GPU architecture identification enum (PglGpuArch). Added I2C capability query (0x09) returning PglCapabilityResponse (16 bytes: arch, core count, freq, SRAM, limits, flags). Added PglParser.h for alignment-safe deserialization on RISC-V/other cores. Generalized all documentation to be GPU-implementation-neutral. Wire format unchanged — fully backward-compatible with v0.2. |
| 0.3.1 | 2026-03-04 | **General shader system.** Replaced 8 hardcoded effect types with 3 general shader classes: CONVOLUTION (configurable kernel shape/radius/direction/auto-rotation), DISPLACEMENT (axis/chromatic-split/waveform), COLOR_ADJUST (7 operations: feather, threshold, gamma, invert, brightness, contrast, edge-detect). Renamed `CMD_SET_EFFECT` → `CMD_SET_SHADER` (opcode unchanged at 0x83), expanded payload from 23 → 27 bytes (`params[]` 16 → 20). All 8 original ProtoTracer effects expressible via convenience wrappers. Added configurable oscillator waveforms (sawtooth, sine, triangle, square) for animated shaders. |
| 0.3.2 | 2026-03-04 | **Tiered memory architecture (design).** Added optional external memory support: Tier 1 OPI PSRAM (APS6408L-OBQ via PIO2, ~150 MB/s DMA), Tier 2 QSPI MRAM (Everspin MR10Q010 via QMI CS1, 128 KB, ~52 MB/s XIP, non-volatile, unlimited endurance). Weight-based placement policy with scored demotion. SRAM cache arena (64 KB, 4 KB lines). Full design documented in `GPU_API_Design.md` §8; firmware README updated. No wire-format changes — tiered memory is internal to the GPU. Graceful degradation: builds without external memory operate SRAM-only. |
| 0.4.0 | 2026-03-04 | **M4 rasterizer implementation.** Full vertex transform → perspective/ortho projection → Triangle2D pool → QuadTree spatial indexing → per-pixel barycentric rasterization with Z-buffer. SimpleMaterial evaluation (solid colour). Back-face culling, frustum culling, near-plane culling. Dual-core parallel RasterizeRange on non-overlapping Y bands. No wire-format changes. |
| 0.5.0 | 2026-03-04 | **GPU memory access API.** Added 7 new SPI commands (0x30–0x3F): `CMD_MEM_WRITE`, `CMD_MEM_READ_REQUEST`, `CMD_MEM_SET_RESOURCE_TIER`, `CMD_MEM_ALLOC`, `CMD_MEM_FREE`, `CMD_FRAMEBUFFER_CAPTURE`, `CMD_MEM_COPY`. Added 4 new I2C registers (0x0C–0x0F): `MEM_TIER_INFO`, `MEM_READ_ADDR`, `MEM_READ_DATA`, `MEM_ALLOC_RESULT`. Host can now read/write all GPU memory tiers (SRAM, OPI PSRAM, QSPI MRAM/PSRAM), allocate/free regions, control resource tier placement, and capture framebuffer screenshots. Readback uses I2C (32-byte chunks, ~50 KB/s at 400 kHz). Wire format remains backward-compatible — older parsers skip unknown 0x30+ opcodes via payload length. New types: `PglMemTier`, `PglMemHandle`, `PglMemResourceClass`, `PglMemTierInfoResponse`, `PglMemAllocResult`, `PglMemAllocStatus`. Updated `protoVersion` to 5. See GPU_API_Design.md §9 for design rationale and Vulkan parallels. |
| 0.5.1 | 2026-03-04 | **QSPI CS1 auto-detection + chip-aware tier weights.** Firmware now auto-detects installed chip on QMI CS1 via 3-step RDID probe (MRAM 0x4B, PSRAM 0x9F, unknown fallback). Detected `QspiChipProfile` drives driver init (MRAM vs PSRAM path) and selects dual `BaseWeight()` tables: MRAM weights aggressively demote random-access resources (LUTs ↓ 30, materials ↓ 80, textures ↓ 100) from SRAM to QSPI; PSRAM weights are conservative. Extended status byte 31 changed from `reserved` to `qspiChipType` (`PglQspiChipType` enum). New types: `QspiChipType`, `QspiChipProfile`, `PglQspiChipType`. `QSPI_MRAM_ENABLED` → `QSPI_CS1_ENABLED` (legacy alias kept). No wire-format changes for command buffers. |
| 0.5.2 | 2026-03-04 | **PIO2 multi-mode external memory.** Tier 1 (PIO2) now supports three operating modes via `PIO2_MEM_MODE`: (1) `OPI_PSRAM` — 8-bit DQ, APS6408L, 8 MB, 75 MHz, QFN-80 only; (2) `DUAL_QSPI_MRAM` — 4-bit DQ + dual CS, 2×MR10Q010, 256 KB total, 104 MHz, non-volatile; (3) `SINGLE_QSPI_MRAM` — 4-bit DQ + single CS, 1×MR10Q010, 128 KB, 104 MHz, non-volatile. QSPI MRAM modes work on any RP2350 package (GPIO 34–37 only). GPIO 38 repurposed as CS1 in dual mode. New enum `Pio2MemMode` + host mirror `PglPio2MemMode`. New derived helpers: `Pio2MemCapacity()`, `Pio2DataPinCount()`, `Pio2IsMram()`. Legacy `OPI_PSRAM_ENABLED` alias preserved. `OpiPsramConfig` → `Pio2MemConfig` (alias kept). Driver detects mode at boot via `ProbePio2Memory()`. No wire-format changes. |
