# ProtoGL API Specification v0.7

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
- The GPU reports its architecture and capabilities via the capability query (SPI read
  command 0xE2, or I2C register 0x09 as fallback) described in §4.3, allowing the host
  to adapt resource limits and optimization paths at runtime.

---

## 2. Concepts

### 2.1 Resources (GPU-Side State)

| Resource Type | ID Range | Description |
|---|---|---|
| `Mesh` | 0-255 | Indexed triangle mesh (vertices + index groups + optional UVs) |
| `Material` | 0-255 | Procedural shader definition (type + parameters) |
| `Texture` | 0-63 | Single raster image (for Image material, sprites, etc.) |
| `ImageSequence` | 0-31 | Multi-frame animated texture atlas — stores N frames in a single allocation; GPU auto-advances based on FPS/loop mode |
| `Font` | 0-15 | Custom glyph atlas with per-glyph metrics for `CMD_DRAW_TEXT` (fontSize=2) |
| `PixelLayout` | 0-7 | Static pixel coordinate array (defines the physical LED positions per camera) |
| `Camera` | 0-3 | Camera state (transform, look offset, pixel layout binding) |
| `Layer` | 0-7 | 2D compositing layer (§4.6) — optional, for multi-layer 2D/UI rendering |
| `MemPool` | 0-15 | Memory pool (§4.7) — fixed-size block allocator within a memory tier |
| `ShaderProgram` | 0-15 | Compiled PGLSL shader program (§4.8) |

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

#### `CMD_UPDATE_TEXTURE` (0x1A)
Partially or fully replaces pixel data in an existing texture without destroying and recreating the resource handle.

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `textureId` |
| 2 | 2 | `offsetX` — pixel column to start writing at (0 = left edge) |
| 4 | 2 | `offsetY` — pixel row to start writing at (0 = top edge) |
| 6 | 2 | `regionW` — width of the updated region |
| 8 | 2 | `regionH` — height of the updated region |
| 10 | regionW×regionH×bpp | Pixel data (same format as original texture) |

If `offsetX=0, offsetY=0, regionW=width, regionH=height`, this is a full-frame replacement (useful for PreRenderedTexture refreshes). For sub-region updates, only the specified rectangle is overwritten.

#### `CMD_DESTROY_TEXTURE` (0x19)
| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `textureId` |

#### `CMD_CREATE_IMAGE_SEQUENCE` (0x1B)
Uploads a multi-frame animated texture atlas. The atlas stores all frames sequentially in a single memory allocation. The GPU auto-advances the active frame based on the specified FPS and loop mode. Both 2D sprites (`CMD_DRAW_SPRITE`) and 3D materials (`PGL_MAT_IMAGE_SEQUENCE`) can reference an image sequence.

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `sequenceId` — 0–31 |
| 2 | 2 | `frameWidth` — width of each frame in pixels |
| 4 | 2 | `frameHeight` — height of each frame in pixels |
| 6 | 2 | `frameCount` — number of frames in the sequence |
| 8 | 1 | `format` — 0=RGB565, 1=RGB888 |
| 9 | 1 | `loopMode` — 0=loop, 1=once, 2=ping-pong, 3=hold-last |
| 10 | 2 | `fps` — playback frame rate (Q8.8 fixed-point, e.g., 0x0C00 = 12.0 fps) |
| 12 | frameWidth×frameHeight×bpp×frameCount | Pixel data (all frames, sequentially) |

**Memory placement:** Image sequence atlases are typically large (KB–MB). By default, the GPU places them in external VRAM (Tier 1 QSPI-A or Tier 2 QSPI-B). Only the currently active frame is DMA-fetched into the SRAM cache arena during rasterization. For very large sequences, use `CMD_MEM_STREAM_BEGIN/CHUNK/END` to upload across multiple frames.

#### `CMD_DESTROY_IMAGE_SEQUENCE` (0x1C)
| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `sequenceId` |

#### `CMD_CREATE_FONT` (0x1D)
Uploads a custom glyph atlas with per-character metrics. Once created, the font can be used by `CMD_DRAW_TEXT` (fontSize=2, followed by a fontId byte) for both 2D overlay text and any text rendering on the GPU.

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `fontId` — 0–15 |
| 2 | 2 | `atlasWidth` — glyph atlas texture width |
| 4 | 2 | `atlasHeight` — glyph atlas texture height |
| 6 | 1 | `format` — 0=GRAYSCALE8 (1 bpp, recommended), 1=RGB565 |
| 7 | 1 | `glyphCount` — number of glyph entries (1–255) |
| 8 | 1 | `firstChar` — ASCII code of the first glyph (e.g., 0x20 = space) |
| 9 | 1 | `lineHeight` — line height in pixels |
| 10 | 1 | `baseline` — baseline offset from top of cell |
| 11 | 1 | `flags` — bit0: monospaced, bit1: anti-aliased |
| 12 | glyphCount×8 | Glyph metrics array (see below) |
| 12+glyphCount×8 | atlasWidth×atlasHeight×bpp | Atlas pixel data |

**Glyph metrics entry** (8 bytes each):

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `atlasX` — X position of glyph in atlas |
| 2 | 2 | `atlasY` — Y position of glyph in atlas |
| 4 | 1 | `width` — glyph width in pixels |
| 5 | 1 | `height` — glyph height in pixels |
| 6 | 1 | `xAdvance` — horizontal advance after rendering this glyph |
| 7 | 1 | `xOffset` — horizontal bearing (pixels from cursor to left edge) |

**Memory placement:** Font atlases are read-only after upload and benefit from XIP. Default tier: QSPI (Tier 2). Small fonts (< 2 KB) may stay in SRAM.

#### `CMD_DESTROY_FONT` (0x1E)
| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `fontId` |

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
tiers (SRAM, QSPI-A external VRAM, QSPI-B external VRAM). See GPU_API_Design.md §9 for design rationale.

#### `CMD_MEM_WRITE` (0x30)

Write raw bytes to a specific GPU memory tier and address.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `tier` | `PglMemTier` (0=SRAM, 1=QSPI-A, 2=QSPI-B) |
| 1 | 4 | `address` | Byte offset within tier address space |
| 5 | 4 | `size` | Number of data bytes that follow |
| 9 | N | `data[]` | Raw bytes to write |

#### `CMD_MEM_READ_REQUEST` (0x31)

Request GPU to stage memory for SPI readback (via `SPI_READ_MEM_DATA`, 0xE4) or I2C readback via `MEM_READ_DATA` (0x0E) as fallback.

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

Allocate a region in a specific GPU memory tier. Result available via SPI read (`SPI_READ_ALLOC_RESULT`, 0xE5) or I2C register 0x0F as fallback.

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

Snapshot the framebuffer for SPI readback (or I2C readback as fallback).

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
| 4 | 2 | `qspiATotalKB` (QSPI Ch-A total; 0 if absent) |
| 6 | 2 | `qspiAFreeKB` (QSPI Ch-A free) |
| 8 | 1 | `qspiAEnabled` (1 if QSPI-A driver active) |
| 9 | 2 | `qspiBTotalKB` (QSPI Ch-B total; 0 if absent) |
| 11 | 2 | `qspiBFreeKB` (QSPI Ch-B free) |
| 13 | 1 | `qspiBEnabled` (1 if QSPI-B driver active) |
| 14 | 2 | `cachedEntries` |
| 16 | 1 | `qspiAChipCount` (0–2) |
| 17 | 1 | `qspiBChipCount` (0–2) |
| 18 | 1 | `cacheHitRate` (0-100%) |
| 19 | 1 | reserved |

**Alloc Result (0x0F):**
| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `handle` (`PglMemHandle`, 0xFFFF on failure) |
| 2 | 4 | `address` (tier-relative byte address) |
| 6 | 1 | `status` (0x00=OK, 0x01=OOM, 0x02=InvalidTier, 0x03=Disabled, 0x04=HandleExhausted) |

---

### 4.6 Programmable Shader Commands (SPI 0x84–0x87)

PGLSL programmable shaders allow custom screen-space post-processing via compiled PSB bytecode.
See `Shader_System_Design.md` for the PGLSL language spec and bytecode format.

#### `CMD_CREATE_SHADER_PROGRAM` (0x84)

Upload compiled PSB bytecode as a named shader program.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 2 | `programId` | Shader program handle (0–15) |
| 2 | 4 | `bytecodeSize` | Size of PSB bytecode in bytes |
| 6 | N | `bytecode[]` | Compiled PSB binary (header + uniforms + constants + instructions) |

#### `CMD_DESTROY_SHADER_PROGRAM` (0x85)

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 2 | `programId` | Shader program handle to destroy |

#### `CMD_BIND_SHADER_PROGRAM` (0x86)

Bind a compiled shader program to a camera's shader slot.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `cameraId` | Camera index (0–3) |
| 1 | 1 | `shaderSlot` | Slot index (0–3) |
| 2 | 2 | `programId` | Shader program handle |
| 4 | 4 | `intensity` | float (0.0–1.0) — global mix factor |

#### `CMD_SET_SHADER_UNIFORM` (0x87)

Set a uniform variable value on a bound shader program.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 2 | `programId` | Target shader program |
| 2 | 1 | `uniformIndex` | Uniform slot (0–15) |
| 3 | 1 | `componentCount` | 1–4 (float, vec2, vec3, vec4) |
| 4 | N×4 | `values[]` | float values (4–16 bytes) |

---

### 4.7 Display Commands (SPI 0x90–0x92, I2C 0x15–0x17)

> Full design: [Display_Frontend_Design.md](Display_Frontend_Design.md)

These commands control the unified display driver interface introduced in v0.6.

#### `CMD_DISPLAY_CONFIGURE` (0x90)

Configure display parameters for the active display driver.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `displayId` | Display index (0–3 for multi-display) |
| 1 | 2 | `width` | Requested width in pixels |
| 3 | 2 | `height` | Requested height in pixels |
| 5 | 1 | `colorDepth` | 0=RGB565, 1=RGB888, 2=RGB666 |
| 6 | 2 | `refreshHz` | Requested refresh rate (0=driver default) |
| 8 | 1 | `flags` | bit0: `vsync`, bit1: `interlaced`, bit2: `rotated180` |

#### `CMD_DISPLAY_SET_REGION` (0x91)

Define a partial-update region (for SPI/QSPI displays with windowed writes).

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `displayId` | Display index |
| 1 | 2 | `x` | Region X origin |
| 3 | 2 | `y` | Region Y origin |
| 5 | 2 | `width` | Region width |
| 7 | 2 | `height` | Region height |

#### `CMD_DISPLAY_SYNC` (0x92)

Synchronize multi-display frame timing (barrier — all displays swap together).

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `mask` | Bitmask of display IDs to synchronize (bit N = display N) |
| 1 | 1 | `flags` | bit0: `waitForAll` (block until all complete), bit1: `tearingAllowed` |

#### Display I2C Registers

| Reg | Name | Payload | Direction |
|---|---|---|---|
| 0x15 | `DISPLAY_MODE` | Write: 2 bytes (displayId, driverType: 0=HUB75, 1=DVI, 2=SPI, 3=QSPI, 4=PARALLEL) | Host → GPU |
| 0x16 | `DISPLAY_CAPS` | Read: 16 bytes (`PglDisplayCapsResponse`) | GPU → Host |
| 0x17 | `MULTI_DISPLAY_ROUTE` | Write: 8 bytes (per-display framebuffer source + viewport) | Host → GPU |

**Display Caps Response (0x16):**
| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `driverType` (enum: HUB75/DVI/SPI/QSPI/PARALLEL) |
| 1 | 2 | `maxWidth` |
| 3 | 2 | `maxHeight` |
| 5 | 1 | `colorDepthFlags` (bitmask: bit0=RGB565, bit1=RGB888, bit2=RGB666) |
| 6 | 2 | `maxRefreshHz` |
| 8 | 1 | `pioBlock` (0xFF=none) |
| 9 | 1 | `smCount` (PIO state machines used) |
| 10 | 1 | `dmaChannels` (DMA channels used) |
| 11 | 1 | `flags` (bit0=supportsPartialUpdate, bit1=supportsRotation, bit2=supportsVsync) |
| 12 | 4 | reserved |

---

### 4.8 2D Graphics / Layer Commands (SPI 0xA0–0xAE)

> Full design: [2D_Graphics_And_Compositing.md](2D_Graphics_And_Compositing.md)

These commands support multi-layer 2D rendering and compositing.

#### `CMD_LAYER_CREATE` (0xA0)

Create a 2D compositing layer.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `layerId` | Layer index (0–7) |
| 1 | 2 | `width` | Layer width in pixels |
| 3 | 2 | `height` | Layer height in pixels |
| 5 | 1 | `format` | 0=RGB565, 1=RGBA4444 |
| 6 | 1 | `blendMode` | Blend mode enum (§5.7) |
| 7 | 1 | `opacity` | Global opacity (0–255) |
| 8 | 2 | `zOrder` | Sort key for compositing order |

#### `CMD_LAYER_DESTROY` (0xA1)

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `layerId` |

#### `CMD_LAYER_SET_PROPS` (0xA2)

Update layer properties without destroying/recreating.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `layerId` | Layer index |
| 1 | 1 | `opacity` | Global opacity (0–255) |
| 2 | 1 | `blendMode` | Blend mode enum |
| 3 | 2 | `offsetX` | Viewport X offset |
| 5 | 2 | `offsetY` | Viewport Y offset |
| 7 | 2 | `clipX` | Clip rect X |
| 9 | 2 | `clipY` | Clip rect Y |
| 11 | 2 | `clipW` | Clip rect width (0=full) |
| 13 | 2 | `clipH` | Clip rect height (0=full) |

#### `CMD_DRAW_RECT_2D` (0xA3)

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `layerId` | Target layer |
| 1 | 2 | `x` | X position |
| 3 | 2 | `y` | Y position |
| 5 | 2 | `width` | Rectangle width |
| 7 | 2 | `height` | Rectangle height |
| 9 | 2 | `color` | RGB565 color |
| 11 | 1 | `flags` | bit0: `filled` (0=outline, 1=solid) |

#### `CMD_DRAW_LINE_2D` (0xA4)

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `layerId` | Target layer |
| 1 | 2 | `x0` | Start X |
| 3 | 2 | `y0` | Start Y |
| 5 | 2 | `x1` | End X |
| 7 | 2 | `y1` | End Y |
| 9 | 2 | `color` | RGB565 color |

#### `CMD_DRAW_CIRCLE_2D` (0xA5)

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `layerId` | Target layer |
| 1 | 2 | `cx` | Center X |
| 3 | 2 | `cy` | Center Y |
| 5 | 2 | `radius` | Radius in pixels |
| 7 | 2 | `color` | RGB565 color |
| 9 | 1 | `flags` | bit0: `filled` |

#### `CMD_DRAW_SPRITE` (0xA6)

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `layerId` | Target layer |
| 1 | 2 | `x` | Destination X |
| 3 | 2 | `y` | Destination Y |
| 5 | 2 | `textureId` | Source texture resource |
| 7 | 1 | `flags` | bit0: `flipH`, bit1: `flipV`, bit2: `hasColorKey` |
| 8 | 2 | `colorKey` | RGB565 transparent color (if hasColorKey) |

#### `CMD_DRAW_TEXT` (0xA7)

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `layerId` | Target layer |
| 1 | 2 | `x` | Text X position |
| 3 | 2 | `y` | Text Y position |
| 5 | 2 | `fontId` | Font atlas texture ID |
| 7 | 2 | `color` | RGB565 text color |
| 9 | 1 | `fontSize` | Scale factor (1=native, 2=2x, etc.) |
| 10 | 1 | `length` | String byte count |
| 11 | N | `text[]` | UTF-8 string data |

#### `CMD_DRAW_SPRITE_BATCH` (0xA8)

Batch multiple sprites in a single command (reduces command overhead).

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `layerId` | Target layer |
| 1 | 2 | `count` | Number of sprites |
| 3 | N×9 | `sprites[]` | Array of `{int16_t x, y; uint16_t texId; uint8_t flags; uint16_t colorKey}` |

#### `CMD_LAYER_CLEAR` (0xA9)

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `layerId` | Target layer |
| 1 | 2 | `color` | RGB565 fill color (0x0000 for transparent) |

#### `CMD_DRAW_ROUNDED_RECT` (0xAA)

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `layerId` | Target layer |
| 1 | 2 | `x` | X position |
| 3 | 2 | `y` | Y position |
| 5 | 2 | `width` | Width |
| 7 | 2 | `height` | Height |
| 9 | 1 | `cornerRadius` | Corner radius in pixels |
| 10 | 2 | `color` | RGB565 color |
| 12 | 1 | `flags` | bit0: `filled` |

#### `CMD_DRAW_ARC` (0xAB)

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `layerId` | Target layer |
| 1 | 2 | `cx` | Center X |
| 3 | 2 | `cy` | Center Y |
| 5 | 2 | `radius` | Radius |
| 7 | 4 | `startAngle` | Start angle (float, degrees) |
| 11 | 4 | `endAngle` | End angle (float, degrees) |
| 15 | 2 | `color` | RGB565 color |

#### `CMD_DRAW_TRIANGLE_2D` (0xAC)

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `layerId` | Target layer |
| 1 | 2 | `x0` | Vertex 0 X |
| 3 | 2 | `y0` | Vertex 0 Y |
| 5 | 2 | `x1` | Vertex 1 X |
| 7 | 2 | `y1` | Vertex 1 Y |
| 9 | 2 | `x2` | Vertex 2 X |
| 11 | 2 | `y2` | Vertex 2 Y |
| 13 | 2 | `color` | RGB565 color |
| 15 | 1 | `flags` | bit0: `filled` |

#### `CMD_BILLBOARD_SPRITE` (0xAD)

Render a world-space sprite through the 3D camera projection (always faces camera).

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `cameraId` | Camera for projection |
| 1 | 12 | `worldPos` | float x, y, z — world-space center position |
| 13 | 2 | `textureId` | Sprite texture |
| 15 | 4 | `size` | float — world-space size |
| 19 | 1 | `flags` | bit0: `depthTest`, bit1: `depthWrite` |

#### `CMD_LAYER_SET_VISIBILITY` (0xAE)

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `layerId` | Target layer |
| 1 | 1 | `visible` | 0=hidden, 1=visible |

---

### 4.9 Memory Pool & Streaming Commands (SPI 0x38–0x41)

> Full design: [Memory_Management_API.md](Memory_Management_API.md)

These commands extend the basic memory access API (§4.4) with pool allocators,
defragmentation, streaming upload, and resource binding.

#### `CMD_MEM_POOL_CREATE` (0x38)

Create a pool of fixed-size blocks in a memory tier.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `tier` | Target `PglMemTier` |
| 1 | 2 | `blockSize` | Size of each block in bytes |
| 3 | 2 | `blockCount` | Number of blocks |
| 5 | 2 | `tag` | User-defined tag |

Result available via SPI read (`SPI_READ_ALLOC_RESULT`, 0xE5) or I2C register 0x0F as fallback.

#### `CMD_MEM_POOL_ALLOC` (0x39)

Allocate a single block from a pool. O(1) free-list pop.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 2 | `poolHandle` | Pool handle from `CMD_MEM_POOL_CREATE` |
| 1 | 2 | `tag` | User-defined tag |

#### `CMD_MEM_POOL_FREE` (0x3A)

Free a block back to its pool. O(1) free-list push.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 2 | `poolHandle` | Pool handle |
| 2 | 2 | `blockHandle` | Block handle from `CMD_MEM_POOL_ALLOC` |

#### `CMD_MEM_POOL_DESTROY` (0x3B)

Destroy an entire pool and return its memory to the tier's free list.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 2 | `poolHandle` | Pool handle |

#### `CMD_MEM_DEFRAG` (0x3C)

Compact fragmented memory in a tier.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `tier` | Target `PglMemTier` (0=SRAM, 1=QSPI-A, 2=QSPI-B) |
| 1 | 2 | `maxMoveKB` | Max data to relocate per invocation |
| 3 | 1 | `flags` | bit0: `urgent` (block frame), bit1: `incremental` |

#### `CMD_STREAM_BEGIN` (0x3D)

Start a multi-frame streaming upload.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `streamId` | Stream identifier (0–3) |
| 1 | 1 | `tier` | Destination `PglMemTier` |
| 2 | 4 | `totalSize` | Total bytes to upload |
| 6 | 2 | `tag` | User-defined tag |

#### `CMD_STREAM_DATA` (0x3E)

Send a data chunk for an active stream.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `streamId` | Stream identifier |
| 1 | 4 | `offset` | Byte offset within stream |
| 5 | 2 | `chunkSize` | Size of this chunk |
| 7 | N | `data[]` | Raw bytes |

#### `CMD_STREAM_COMMIT` (0x3F)

Commit a completed stream: bind the uploaded data to a resource.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `streamId` | Stream identifier |
| 1 | 1 | `resourceClass` | `PglMemResourceClass` |
| 2 | 2 | `resourceId` | Handle to bind the data to |

#### `CMD_MEM_BIND_RESOURCE` (0x40)

Explicitly bind a memory allocation to a resource handle (Vulkan-style).

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 2 | `memHandle` | Memory alloc handle |
| 2 | 1 | `resourceClass` | `PglMemResourceClass` |
| 3 | 2 | `resourceId` | Resource handle |

#### `CMD_MEM_UNBIND_RESOURCE` (0x41)

Unbind memory from a resource (resource becomes unresolvable until re-bound).

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `resourceClass` | `PglMemResourceClass` |
| 1 | 2 | `resourceId` | Resource handle |

#### Memory Pool & Streaming I2C Registers

| Reg | Name | Payload | Direction |
|---|---|---|---|
| 0x18 | `MEM_POOL_STATUS` | Read: 16 bytes (per-pool total/free blocks, run length) | GPU → Host |
| 0x19 | `MEM_DEFRAG_STATUS` | Read: 8 bytes (moved/total KB, fragments remaining) | GPU → Host |
| 0x1A | `MEM_STREAM_STATUS` | Read: 12 bytes (received/total bytes, status) | GPU → Host |
| 0x1B | `MEM_BINDING_TABLE` | Read: 32 bytes (active resource↔memory bindings, paginated) | GPU → Host |

---

### 4.10 Persistence & Direct Framebuffer Commands (SPI 0x45–0x48)

> Full design: [Memory_Management_API.md](Memory_Management_API.md) §8.3–§8.4

These commands extend the memory system with flash persistence (for volatile PSRAM)
and direct host-to-framebuffer pixel writes.

#### `CMD_WRITE_FRAMEBUFFER` (0x45)

Write raw RGB565 pixel data directly into the GPU's back buffer or a compositing
layer buffer. Bypasses the rasterizer, QuadTree, Z-buffer, and shaders entirely.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 2 | `x` | Left column of target region (pixels) |
| 2 | 2 | `y` | Top row of target region (pixels) |
| 4 | 2 | `w` | Width of region (pixels) |
| 6 | 2 | `h` | Height of region (pixels) |
| 8 | 1 | `layerId` | 0xFF = main back buffer, 0–7 = compositing layer |
| 9 | W×H×2 | `pixels[]` | RGB565 pixel data, row-major |

**Notes:**
- Out-of-bounds pixels are clipped (no error). If the region extends beyond the
  framebuffer dimensions, only the in-bounds portion is written.
- For full-frame writes (128×64 RGB565), total payload = 9 + 16384 = 16393 bytes,
  transferable in ~0.2 ms at 80 MHz Octal SPI.
- Multiple `CMD_WRITE_FRAMEBUFFER` calls per frame are allowed (e.g., update
  different regions incrementally).
- Direct writes happen **before** the compositor runs. If compositing layers are
  active, direct writes to a layer will be blended with other layers at `EndFrame`.

#### `CMD_PERSIST_RESOURCE` (0x46)

Request the GPU to persist a resource's data to on-board flash for survival across
power cycles. If the external VRAM is MRAM (non-volatile), this is a no-op — the GPU
acknowledges immediately with status `ALREADY_PERSISTENT`.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `resourceClass` | `PglMemResourceClass` |
| 1 | 2 | `resourceId` | Resource handle |
| 3 | 1 | `flags` | bit0: `urgent` (block until flash write complete), bit1: `delete-on-fail` (free resource if flash write fails) |

**Asynchronous completion:** By default (flags=0), the GPU queues the writeback and
completes it incrementally (4 KB/frame). Completion is signalled via GPU→Host mailbox
slot 9 (`PGL_MBOX_GPU_PERSIST_STATUS`) and IRQ pulse. The host can poll
`MEM_PERSIST_STATUS` (I2C 0x1C) for progress.

**Status codes** (via `MEM_PERSIST_STATUS`):
- 0x00 = Idle (no pending persist)
- 0x01 = In progress (partial write)
- 0x02 = Complete
- 0x03 = Failed (flash write error)
- 0x04 = Already persistent (MRAM or already in flash)
- 0x05 = Flash full (no space in persistence region)

#### `CMD_RESTORE_RESOURCE` (0x47)

Request the GPU to restore a previously persisted resource from flash to VRAM.
The GPU checks the flash manifest for a matching entry and DMA-loads the data.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `resourceClass` | `PglMemResourceClass` |
| 1 | 2 | `resourceId` | Resource handle to assign |
| 3 | 1 | `flags` | bit0: `auto-allocate` (GPU allocates VRAM; 0=host pre-allocated) |

**Completion:** Signalled via mailbox slot 9 and IRQ. On success, the resource table
is populated and the resource is ready for immediate use (draw calls, material refs).

#### `CMD_QUERY_PERSISTENCE` (0x48)

Query the persistence state of a specific resource or the overall flash manifest.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | `resourceClass` | `PglMemResourceClass` (0xFF = query manifest summary) |
| 1 | 2 | `resourceId` | Resource handle (ignored if resourceClass=0xFF) |

**Result** available via SPI read (`SPI_READ_PERSIST_STATUS`, 0xEB) or I2C register
0x1C as fallback. Returns 12 bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `status` (status code — see above) |
| 1 | 1 | `resourceClass` |
| 2 | 2 | `resourceId` |
| 4 | 4 | `flashOffset` (0 if not persisted) |
| 8 | 4 | `dataSize` (0 if not persisted) |

When `resourceClass=0xFF` (manifest summary):

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `persistedCount` — total persisted resources |
| 2 | 4 | `flashUsedBytes` — total flash bytes consumed |
| 6 | 4 | `flashFreeBytes` — remaining flash space |
| 10 | 2 | `maxEntries` — manifest capacity |

#### Persistence I2C Registers

| Reg | Name | Payload | Direction |
|---|---|---|---|
| 0x1C | `MEM_PERSIST_STATUS` | Read: 12 bytes (per-resource or manifest summary) | GPU → Host |

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
| 0x41 | `ImageSequenceMaterial` | `uint16_t sequenceId; float offsetX, offsetY, scaleX, scaleY; uint8_t playbackFlags` (13 bytes) — references a `CMD_CREATE_IMAGE_SEQUENCE` resource. The GPU auto-advances the active frame based on the sequence's FPS and `frameTimeUs`. `playbackFlags` bit0: paused, bit1: reverse. |

> **Memory placement note:** `ImageMaterial` (0x40) stores only a 12-byte parameter block in SRAM; the actual pixel data lives in the referenced Texture resource (placed by the tiering manager based on size — small textures in SRAM, large ones in external VRAM). `ImageSequenceMaterial` (0x41) similarly stores only a 13-byte parameter block in SRAM; the backing image sequence atlas resides in external VRAM (Tier 1/2) with only the active frame DMA-fetched to the SRAM cache arena.

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
PglStatus pglQueryStatus(ProtoGLDevice* dev);  // reads via SPI (or I2C fallback)

// --- Configuration ---
void  pglSetBrightness(ProtoGLDevice* dev, uint8_t brightness);
void  pglSetPanelConfig(ProtoGLDevice* dev, uint16_t width, uint16_t height, uint8_t scanRate);
void  pglSetGammaTable(ProtoGLDevice* dev, uint8_t table);

// --- Programmable Shaders (PGLSL) ---
PglShaderProgram pglCreateShaderProgram(ProtoGLDevice* dev, const uint8_t* bytecode, uint32_t size);
void  pglDestroyShaderProgram(ProtoGLDevice* dev, PglShaderProgram prog);
void  pglBindShaderProgram(ProtoGLDevice* dev, PglCamera cam, uint8_t slot,
                            PglShaderProgram prog, float intensity);
void  pglSetShaderUniform(ProtoGLDevice* dev, PglShaderProgram prog,
                           uint8_t index, float value);
void  pglSetShaderUniformVec2(ProtoGLDevice* dev, PglShaderProgram prog,
                               uint8_t index, float x, float y);
void  pglSetShaderUniformVec3(ProtoGLDevice* dev, PglShaderProgram prog,
                               uint8_t index, float x, float y, float z);
void  pglSetShaderUniformVec4(ProtoGLDevice* dev, PglShaderProgram prog,
                               uint8_t index, float x, float y, float z, float w);

// --- Display Frontend ---
void  pglSetDisplayMode(ProtoGLDevice* dev, uint8_t displayId, PglDisplayDriver driverType);
PglDisplayCaps pglQueryDisplayCaps(ProtoGLDevice* dev);
void  pglConfigureDisplay(ProtoGLDevice* dev, uint8_t displayId,
                           uint16_t width, uint16_t height,
                           uint8_t colorDepth, uint16_t refreshHz, uint8_t flags);
void  pglSetDisplayRegion(ProtoGLDevice* dev, uint8_t displayId,
                           uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void  pglSyncDisplays(ProtoGLDevice* dev, uint8_t displayMask, uint8_t flags);

// --- 2D Layers & Drawing ---
void  pglCreateLayer(ProtoGLDevice* dev, uint8_t layerId,
                      uint16_t width, uint16_t height,
                      uint8_t format, uint8_t blendMode,
                      uint8_t opacity, uint16_t zOrder);
void  pglDestroyLayer(ProtoGLDevice* dev, uint8_t layerId);
void  pglSetLayerProps(ProtoGLDevice* dev, uint8_t layerId,
                        uint8_t opacity, uint8_t blendMode,
                        int16_t offsetX, int16_t offsetY,
                        uint16_t clipX, uint16_t clipY,
                        uint16_t clipW, uint16_t clipH);
void  pglSetLayerVisibility(ProtoGLDevice* dev, uint8_t layerId, bool visible);
void  pglClearLayer(ProtoGLDevice* dev, uint8_t layerId, uint16_t color);

void  pglDrawRect2D(ProtoGLDevice* dev, uint8_t layerId,
                     int16_t x, int16_t y, uint16_t w, uint16_t h,
                     uint16_t color, bool filled);
void  pglDrawLine2D(ProtoGLDevice* dev, uint8_t layerId,
                     int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                     uint16_t color);
void  pglDrawCircle2D(ProtoGLDevice* dev, uint8_t layerId,
                       int16_t cx, int16_t cy, uint16_t radius,
                       uint16_t color, bool filled);
void  pglDrawSprite(ProtoGLDevice* dev, uint8_t layerId,
                     int16_t x, int16_t y, PglTexture tex, uint8_t flags);
void  pglDrawText(ProtoGLDevice* dev, uint8_t layerId,
                   int16_t x, int16_t y, PglTexture fontId,
                   uint16_t color, uint8_t fontSize,
                   const char* text, uint8_t length);
void  pglDrawSpriteBatch(ProtoGLDevice* dev, uint8_t layerId,
                          const PglSpriteBatchEntry* sprites, uint16_t count);
void  pglDrawRoundedRect(ProtoGLDevice* dev, uint8_t layerId,
                          int16_t x, int16_t y, uint16_t w, uint16_t h,
                          uint8_t cornerRadius, uint16_t color, bool filled);
void  pglDrawArc(ProtoGLDevice* dev, uint8_t layerId,
                  int16_t cx, int16_t cy, uint16_t radius,
                  float startAngle, float endAngle, uint16_t color);
void  pglDrawTriangle2D(ProtoGLDevice* dev, uint8_t layerId,
                         int16_t x0, int16_t y0,
                         int16_t x1, int16_t y1,
                         int16_t x2, int16_t y2,
                         uint16_t color, bool filled);
void  pglDrawBillboardSprite(ProtoGLDevice* dev, PglCamera cam,
                              float worldX, float worldY, float worldZ,
                              PglTexture tex, float size, uint8_t flags);

// --- Memory Pools ---
PglMemHandle pglCreateMemPool(ProtoGLDevice* dev, PglMemTier tier,
                               uint16_t blockSize, uint16_t blockCount, uint16_t tag);
PglMemHandle pglPoolAlloc(ProtoGLDevice* dev, PglMemHandle pool, uint16_t tag);
void  pglPoolFree(ProtoGLDevice* dev, PglMemHandle pool, PglMemHandle block);
void  pglDestroyMemPool(ProtoGLDevice* dev, PglMemHandle pool);

// --- Memory Defragmentation ---
void  pglDefragMemory(ProtoGLDevice* dev, PglMemTier tier,
                       uint16_t maxMoveKB, uint8_t flags);

// --- Streaming Upload ---
void  pglStreamBegin(ProtoGLDevice* dev, uint8_t streamId, PglMemTier tier,
                      uint32_t totalSize, uint16_t tag);
void  pglStreamData(ProtoGLDevice* dev, uint8_t streamId,
                     uint32_t offset, const uint8_t* data, uint16_t chunkSize);
void  pglStreamCommit(ProtoGLDevice* dev, uint8_t streamId,
                       uint8_t resourceClass, uint16_t resourceId);

// --- Resource Binding ---
void  pglBindResource(ProtoGLDevice* dev, PglMemHandle mem,
                       uint8_t resourceClass, uint16_t resourceId);
void  pglUnbindResource(ProtoGLDevice* dev,
                         uint8_t resourceClass, uint16_t resourceId);

// --- Memory Diagnostics ---
PglMemPoolStatus   pglQueryPoolStatus(ProtoGLDevice* dev);
PglDefragStatus    pglQueryDefragStatus(ProtoGLDevice* dev);
PglStreamStatus    pglQueryStreamStatus(ProtoGLDevice* dev);
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
    .dirPin = GPIO_NUM_10,
    .irqPin = GPIO_NUM_13,
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
> memory (dual PIO2-driven QSPI channels, each supporting up to 2 chip selects for
> PSRAM or MRAM) to extend resource capacity beyond SRAM. The RP2350B reference GPU
> (QFN-80) supports two QSPI VRAM channels (up to 4 external chips total). The RP2350A
> (QFN-60) operates SRAM-only. Per-channel auto-detection probes each chip select at boot.
> See `GPU_API_Design.md` §8 for the full design.

---

## 9. Error Handling

| Condition | GPU Behavior |
|---|---|
| CRC mismatch | Drop frame, render last good frame, increment `droppedFrames` counter |
| Unknown opcode | Skip command (advance by `payloadLength`), set `flags.bufferOverflow` |
| Resource ID overflow | Ignore command, set error flag |
| SPI clock loss | PIO timeout → re-sync on next `0x55AA` header |
| Framebuffer not ready | DIR held in TX direction; host polls SPI status or waits for IRQ before next submission |

---

## 10. Version History

| Version | Date | Changes |
|---|---|---|
| 0.1 | 2026-03-04 | Initial draft — opcodes, wire format, material registry, dual-core model |
| 0.2 | 2026-03-04 | **API FROZEN.** Added Transform offset fields (scaleOffset, rotationOffset, scaleRotationOffset, baseRotation) to CMD_DRAW_OBJECT. Added CameraLayout baseRotation to CMD_SET_CAMERA. Documented blend mode enum (12 values). Clarified IndexGroup uint16 wire encoding. |
| 0.3 | 2026-03-04 | **Architecture-agnostic update.** Added GPU architecture identification enum (PglGpuArch). Added I2C capability query (0x09) returning PglCapabilityResponse (16 bytes: arch, core count, freq, SRAM, limits, flags). Added PglParser.h for alignment-safe deserialization on RISC-V/other cores. Generalized all documentation to be GPU-implementation-neutral. Wire format unchanged — fully backward-compatible with v0.2. |
| 0.3.1 | 2026-03-04 | **General shader system.** Replaced 8 hardcoded effect types with 3 general shader classes: CONVOLUTION (configurable kernel shape/radius/direction/auto-rotation), DISPLACEMENT (axis/chromatic-split/waveform), COLOR_ADJUST (7 operations: feather, threshold, gamma, invert, brightness, contrast, edge-detect). Renamed `CMD_SET_EFFECT` → `CMD_SET_SHADER` (opcode unchanged at 0x83), expanded payload from 23 → 27 bytes (`params[]` 16 → 20). All 8 original ProtoTracer effects expressible via convenience wrappers. Added configurable oscillator waveforms (sawtooth, sine, triangle, square) for animated shaders. |
| 0.3.2 | 2026-03-04 | **Tiered memory architecture (design).** Added optional external memory support via dual PIO2-driven QSPI channels: Tier 1 QSPI-A (up to 2 chips via CS0/CS1), Tier 2 QSPI-B (up to 2 chips via CS0/CS1). Each chip-select auto-detected: PSRAM (APS6408L, 8 MB, ~66 MB/s) or MRAM (MR10Q010, 128 KB, ~52 MB/s, non-volatile). Weight-based placement policy with chip-aware scored demotion. SRAM cache arena (64 KB, 4 KB lines). RP2350B (QFN-80) supports up to 2×2 external VRAM; RP2350A (QFN-60) operates SRAM-only. Full design documented in `GPU_API_Design.md` §8; firmware README updated. No wire-format changes — tiered memory is internal to the GPU. Graceful degradation: builds without external memory operate SRAM-only. |
| 0.4.0 | 2026-03-04 | **M4 rasterizer implementation.** Full vertex transform → perspective/ortho projection → Triangle2D pool → QuadTree spatial indexing → per-pixel barycentric rasterization with Z-buffer. SimpleMaterial evaluation (solid colour). Back-face culling, frustum culling, near-plane culling. Dual-core parallel RasterizeRange on non-overlapping Y bands. No wire-format changes. |
| 0.5.0 | 2026-03-04 | **GPU memory access API.** Added 7 new SPI commands (0x30–0x3F): `CMD_MEM_WRITE`, `CMD_MEM_READ_REQUEST`, `CMD_MEM_SET_RESOURCE_TIER`, `CMD_MEM_ALLOC`, `CMD_MEM_FREE`, `CMD_FRAMEBUFFER_CAPTURE`, `CMD_MEM_COPY`. Added 4 new I2C registers (0x0C–0x0F): `MEM_TIER_INFO`, `MEM_READ_ADDR`, `MEM_READ_DATA`, `MEM_ALLOC_RESULT`. Host can now read/write all GPU memory tiers (SRAM, QSPI-A VRAM, QSPI-B VRAM), allocate/free regions, control resource tier placement, and capture framebuffer screenshots. Readback uses I2C (32-byte chunks, ~50 KB/s at 400 kHz). Wire format remains backward-compatible — older parsers skip unknown 0x30+ opcodes via payload length. New types: `PglMemTier`, `PglMemHandle`, `PglMemResourceClass`, `PglMemTierInfoResponse`, `PglMemAllocResult`, `PglMemAllocStatus`. Updated `protoVersion` to 5. See GPU_API_Design.md §9 for design rationale and Vulkan parallels. |
| 0.5.1 | 2026-03-04 | **Per-chip-select auto-detection + chip-aware tier weights.** Firmware now auto-detects installed chip on each QSPI VRAM chip-select via 3-step RDID probe (MRAM 0x4B, PSRAM 0x9F, unknown fallback). Detected `QspiChipProfile` drives driver init (MRAM vs PSRAM path) and selects dual `BaseWeight()` tables: MRAM weights aggressively demote random-access resources (LUTs ↓ 30, materials ↓ 80, textures ↓ 100) from SRAM to external VRAM; PSRAM weights are conservative. Extended status byte 31 reports Channel A CS0 chip type (`qspiChipType` / `PglQspiChipType` enum). Full per-chip info via `MEM_TIER_INFO` (0x0C). No wire-format changes for command buffers. |
| 0.5.2 | 2026-03-04 | **Dual QSPI VRAM channels.** External VRAM architecture changed from PIO2 single-mode (OPI/QSPI) + QMI CS1 to **two independent PIO2-driven QSPI channels** (Channel A: PIO2 SM0+SM1, Channel B: PIO2 SM2+SM3), each with 2 chip selects — up to 4 external RAM chips total. `QSPI_VRAM_MODE` enum: `NONE`, `SINGLE_CHANNEL`, `DUAL_CHANNEL`. Per-chip-select RDID auto-detect supports mixed PSRAM/MRAM per channel. RP2350A (QFN-60, 30 GPIO): no external VRAM (always `NONE`). RP2350B (QFN-80, 48 GPIO): up to 2×2. Pin map: Ch-A data GPIO 34–37, CS1 GPIO 38; Ch-B data GPIO 39–42, CLK GPIO 43, CS0 GPIO 44, CS1 GPIO 45. New config keys: `QSPI_VRAM_MODE`, `QSPI_A_CHIP_COUNT`, `QSPI_B_CHIP_COUNT`. All access indirect via PIO2 DMA (no XIP). HUD display changed from SPI1 to **I2C1 OLED** (SSD1306/SSD1309, 128×64 mono, 2 pins). No wire-format changes for command buffers. |
| 0.6.0 | 2026-03-04 | **Unified display frontend.** Added `DisplayDriver` abstract interface and `DisplayManager` singleton for multi-display support. New SPI commands: `CMD_DISPLAY_CONFIGURE` (0x90), `CMD_DISPLAY_SET_REGION` (0x91), `CMD_DISPLAY_SYNC` (0x92). New I2C registers: `DISPLAY_MODE` (0x15), `DISPLAY_CAPS` (0x16), `MULTI_DISPLAY_ROUTE` (0x17). Supported driver types: HUB75 (PIO BCM), DVI-D (PIO TMDS), SPI LCD (SPI+DMA), QSPI LCD (PIO), Parallel. PIO resource validation prevents conflicting drivers. Framebuffer format conversion (RGB565→BCM, RGB565→TMDS, etc.) in `SwapBuffers()`. See `Display_Frontend_Design.md`. |
| 0.6.1 | 2026-03-04 | **Programmable shaders.** Added PGLSL shader compilation pipeline (`PglShaderCompiler`). New SPI commands: `CMD_CREATE_SHADER_PROGRAM` (0x84), `CMD_DESTROY_SHADER_PROGRAM` (0x85), `CMD_BIND_SHADER_PROGRAM` (0x86), `CMD_SET_SHADER_UNIFORM` (0x87). PSB bytecode format: 16-byte header, 8-byte uniform descriptors, 4-byte constants, 4-byte instructions, 32 registers, ~50 opcodes. See `Shader_System_Design.md`. |
| 0.7.0 | 2026-03-04 | **2D graphics & multi-layer compositing.** Added 15 new SPI commands (0xA0–0xAE): `CMD_LAYER_CREATE/DESTROY/SET_PROPS`, `CMD_DRAW_RECT_2D`, `CMD_DRAW_LINE_2D`, `CMD_DRAW_CIRCLE_2D`, `CMD_DRAW_SPRITE`, `CMD_DRAW_TEXT`, `CMD_DRAW_SPRITE_BATCH`, `CMD_LAYER_CLEAR`, `CMD_DRAW_ROUNDED_RECT`, `CMD_DRAW_ARC`, `CMD_DRAW_TRIANGLE_2D`, `CMD_BILLBOARD_SPRITE`, `CMD_LAYER_SET_VISIBILITY`. Up to 8 compositing layers with independent opacity, blend mode, viewport offset, clip rectangle, and Z-order. `Layer` resource type added. GPU-side compositor blends layers back-to-front after rasterization. See `2D_Graphics_And_Compositing.md`. **Refined memory management.** Added 10 new SPI commands (0x38–0x41): memory pools (`CMD_MEM_POOL_CREATE/ALLOC/FREE/DESTROY`), defragmentation (`CMD_MEM_DEFRAG`), streaming upload (`CMD_STREAM_BEGIN/DATA/COMMIT`), resource binding (`CMD_MEM_BIND_RESOURCE/UNBIND_RESOURCE`). New I2C registers: `MEM_POOL_STATUS` (0x18), `MEM_DEFRAG_STATUS` (0x19), `MEM_STREAM_STATUS` (0x1A), `MEM_BINDING_TABLE` (0x1B). `MemPool` resource type added. See `Memory_Management_API.md`. Updated `protoVersion` to 7. |
| 0.7.1 | 2026-03-04 | **Resource persistence & direct framebuffer write.** Added 4 new SPI commands (0x45–0x48): `CMD_WRITE_FRAMEBUFFER` (host writes raw RGB565 pixels directly to GPU back buffer or layer buffer, bypassing rasterizer), `CMD_PERSIST_RESOURCE` (request GPU to write-back volatile PSRAM resource to on-board flash for power-cycle survival), `CMD_RESTORE_RESOURCE` (restore persisted resource from flash to VRAM), `CMD_QUERY_PERSISTENCE` (query per-resource or manifest-level persistence status). New I2C register: `MEM_PERSIST_STATUS` (0x1C). New SPI read: `SPI_READ_PERSIST_STATUS` (0xEB). Persistence decision tree: MRAM = inherently persistent (no flash write needed, zero-cost); PSRAM = host-triggered flash writeback (async 4 KB/frame, background after EndFrame). Flash manifest (64-entry, last 512 KB of GPU flash) tracks persisted resources. Boot-time auto-restore: GPU checks manifest and DMA-loads persisted resources back to VRAM, host skips redundant uploads. New GPU→Host mailbox slot 9 for persist completion notification. New config: `FLASH_PERSIST_ENABLED`, `MRAM_AUTO_PERSIST`. See `Memory_Management_API.md` §8.3–§8.4. |
