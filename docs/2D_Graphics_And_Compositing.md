# 2D Graphics, Multi-Layer Compositing & UI Support

> **ProtoGL v0.7 — Extended Graphics Pipeline**
>
> This document extends the ProtoGL API from a 3D-only rasterization pipeline
> to a full 2D+3D compositing system with multi-layer support, suitable for
> UI overlays, HUD elements, sprite-based graphics, and mixed-mode rendering.

---

## §1 Motivation

ProtoGL v0.5 provides a capable 3D rasterization pipeline (mesh + material + camera → framebuffer). However, many real-world applications require 2D graphics alongside 3D:

| Use Case | Current Support | v0.7 Goal |
|----------|----------------|-----------|
| HUD / UI text overlay | ❌ None | ✅ 2D layer composited over 3D |
| Status bars / icons | ❌ None | ✅ Sprite draw commands |
| Menu system | ❌ None | ✅ 2D primitives + text rendering |
| Background image | ❌ None | ✅ Background layer behind 3D scene |
| Split-screen | ❌ Single camera | ✅ Multiple render targets + viewports |
| Particle effects | ❌ Mesh only | ✅ Billboard sprites + blending |
| 2D games / animations | ❌ 3D pipeline only | ✅ Dedicated 2D draw commands |

### Design Philosophy

Inspired by Vulkan's render pass / subpass model and modern GPU compositing:

1. **Layers** — Ordered render layers composited back-to-front with blend modes
2. **2D Primitives** — Rectangles, lines, circles, text, sprites (no 3D transform)
3. **Viewports** — Sub-regions of a render target for split-screen or UI panels
4. **Render Passes** — Explicit ordering: background → 3D scene → 2D overlay → HUD
5. **Shared Resources** — Textures, image sequences, fonts, and materials work for both 2D and 3D (see §1.1)

### §1.1 Shared Resource Model

Resources uploaded to the GPU are **not tied to a specific pipeline**. Once created, a Texture, ImageSequence, Font, or Material can be consumed by both the 3D rasterizer and the 2D draw-command path:

| Resource | 3D Usage | 2D Usage |
|---|---|---|
| **Texture** | `ImageMaterial` (0x40) bound to a mesh via `CMD_DRAW_OBJECT` | `CMD_DRAW_SPRITE` / `CMD_SPRITE_BATCH` source texture |
| **ImageSequence** | `ImageSequenceMaterial` (0x41) — GPU auto-advances frame for 3D objects | `CMD_DRAW_SPRITE` with `PGL_SPRITE_SRC_SEQUENCE` flag — 2D animated sprites |
| **Font** | (future: 3D text meshes) | `CMD_DRAW_TEXT` with `fontSize=2` + `fontId` |
| **Material** | `CMD_DRAW_OBJECT` — any material type (color, procedural, texture) | 2D primitives use inline color; materials are referenced indirectly via texture/sequence |
| **ShaderProgram** | `CMD_BIND_SHADER_PROGRAM` on camera slots | `CMD_SET_LAYER` with `shaderId` — per-layer post-processing |

Resources that are subsystem-specific:
- **Mesh** — 3D only (vertex/index data for `CMD_DRAW_OBJECT`)
- **Layer** — 2D compositing only (render order, blend modes, viewports)
- **Camera** / **PixelLayout** — 3D pipeline only (projection, LED mapping)

**Resource lifecycle safety:** If a shared resource (e.g., a texture referenced by both a 3D material and a 2D sprite command) is destroyed mid-frame, both pipelines see the NULL handle. The GPU parser silently skips draw calls that reference destroyed resources — no crash, but the object/sprite will not render that frame.

---

## §2 Layer Architecture

### §2.1 Layer Model

```
┌─────────────────────────────── Composited Output ──────────────────────────────┐
│                                                                                 │
│  Layer 3 (HUD):      [FPS: 60]  [CPU: 45%]       ←── 2D text, always on top  │
│  Layer 2 (UI):       ┌──────────────┐              ←── 2D menu panel           │
│                       │  Menu Items  │                                          │
│                       └──────────────┘                                          │
│  Layer 1 (3D Scene): ╔═══════════════════╗        ←── 3D rasterized scene     │
│                       ║  ▲  3D Objects   ║                                      │
│                       ║  │  with depth   ║                                      │
│                       ╚═══════════════════╝                                      │
│  Layer 0 (Background): ░░░░░░░░░░░░░░░░░░░        ←── Solid color or texture │
│                                                                                 │
└─────────────────────────────────────────────────────────────────────────────────┘
```

### §2.2 Layer Types

```cpp
/// Layer type identifier
enum PglLayerType : uint8_t {
    PGL_LAYER_BACKGROUND   = 0x00,  // Solid color or tiled texture
    PGL_LAYER_3D_SCENE     = 0x01,  // Full 3D rasterization (existing pipeline)
    PGL_LAYER_2D_CANVAS    = 0x02,  // 2D drawing commands (rects, sprites, text)
    PGL_LAYER_SPRITE_BATCH = 0x03,  // Batch of billboard sprites with shared texture
    PGL_LAYER_RAW_BUFFER   = 0x04,  // Pre-rendered framebuffer (host-uploaded via CMD_WRITE_FRAMEBUFFER 0x45)
};

/// Layer blend mode for compositing
enum PglLayerBlend : uint8_t {
    PGL_LAYER_BLEND_OPAQUE      = 0x00,  // Overwrite (no blending)
    PGL_LAYER_BLEND_ALPHA       = 0x01,  // Standard alpha blend (srcA * src + (1-srcA) * dst)
    PGL_LAYER_BLEND_ADDITIVE    = 0x02,  // src + dst (glow effects)
    PGL_LAYER_BLEND_MULTIPLY    = 0x03,  // src * dst (shadow overlays)
    PGL_LAYER_BLEND_KEY_COLOR   = 0x04,  // Transparent if pixel == keyColor
};

/// Shader binding sentinel — no shader attached
static constexpr uint16_t PGL_NO_SHADER = 0xFFFF;

/// Maximum number of compositing layers
static constexpr uint8_t PGL_MAX_LAYERS = 8;

/// Layer definition
struct PglLayerConfig {
    uint8_t        layerId;       // 0–7, rendering order (0 = bottom)
    PglLayerType   type;
    PglLayerBlend  blendMode;
    uint8_t        opacity;       // 0–255 (global layer opacity)
    bool           visible;
    uint8_t        renderTargetId; // Which render target this layer writes to
    uint16_t       shaderId;      // PSB shader to run on this layer (PGL_NO_SHADER = none)
    // Viewport (sub-region of render target, in pixels)
    uint16_t       viewportX;
    uint16_t       viewportY;
    uint16_t       viewportW;
    uint16_t       viewportH;
};
```

---

## §3 2D Drawing Commands

### §3.1 New Opcodes (0xA0–0xAF)

| Opcode | Name | Description |
|--------|------|-------------|
| `0xA0` | `CMD_SET_LAYER` | Configure a compositing layer |
| `0xA1` | `CMD_DRAW_RECT` | Draw filled/outlined rectangle |
| `0xA2` | `CMD_DRAW_LINE` | Draw line segment |
| `0xA3` | `CMD_DRAW_CIRCLE` | Draw filled/outlined circle or ellipse |
| `0xA4` | `CMD_DRAW_SPRITE` | Draw a textured quad (billboard) |
| `0xA5` | `CMD_DRAW_TEXT` | Draw text string using built-in or custom font (fontSize=2 + fontId for uploaded fonts) |
| `0xA6` | `CMD_DRAW_TRIANGLE_2D` | Draw a filled 2D triangle |
| `0xA7` | `CMD_SPRITE_BATCH_BEGIN` | Begin a sprite batch (shared texture) |
| `0xA8` | `CMD_SPRITE_BATCH_ENTRY` | Add sprite to current batch |
| `0xA9` | `CMD_SPRITE_BATCH_END` | End sprite batch and submit |
| `0xAA` | `CMD_SET_CLIP_RECT` | Set 2D clipping rectangle |
| `0xAB` | `CMD_SET_VIEWPORT` | Set rendering viewport |
| `0xAC` | `CMD_CLEAR_LAYER` | Clear a layer with a solid color |
| `0xAD` | `CMD_DRAW_ARC` | Draw arc or pie segment |
| `0xAE` | `CMD_DRAW_GRADIENT_RECT` | Draw rectangle with gradient fill |
| `0xAF` | `CMD_DRAW_BILLBOARD` | 2D sprite placed in 3D world space |
| `0xB0` | `CMD_SET_LAYER_SHADER` | Bind a PSB shader program to a 2D layer |

### §3.2 Wire-Format Structures

```cpp
#pragma pack(push, 1)

// CMD_SET_LAYER (0xA0)
struct PglCmdSetLayer {
    uint8_t  layerId;         // 0–7
    uint8_t  layerType;       // PglLayerType
    uint8_t  blendMode;       // PglLayerBlend
    uint8_t  opacity;         // 0–255
    uint8_t  flags;           // bit0: visible, bit1: clearOnBeginFrame
    uint8_t  renderTargetId;  // Target render buffer
    uint16_t shaderId;        // PSB shader ID (0xFFFF = none)
    uint16_t viewportX;
    uint16_t viewportY;
    uint16_t viewportW;
    uint16_t viewportH;
};
static_assert(sizeof(PglCmdSetLayer) == 16, "PglCmdSetLayer must be 16 bytes");

// CMD_DRAW_RECT (0xA1)
struct PglCmdDrawRect {
    uint8_t  layerId;
    uint8_t  flags;           // bit0: filled, bit1: has border, bit2: rounded corners
    int16_t  x;               // Top-left X (signed, allows off-screen)
    int16_t  y;               // Top-left Y
    uint16_t width;
    uint16_t height;
    PglColor3 fillColor;
    PglColor3 borderColor;
    uint8_t  borderWidth;
    uint8_t  cornerRadius;    // 0 = sharp corners
};

enum PglRectFlags : uint8_t {
    PGL_RECT_FILLED  = 0x01,
    PGL_RECT_BORDER  = 0x02,
    PGL_RECT_ROUNDED = 0x04,
};

// CMD_DRAW_LINE (0xA2)
struct PglCmdDrawLine {
    uint8_t  layerId;
    uint8_t  lineWidth;       // 1–8 pixels
    int16_t  x0, y0;
    int16_t  x1, y1;
    PglColor3 color;
    uint8_t  flags;           // bit0: anti-aliased
};

// CMD_DRAW_CIRCLE (0xA3)
struct PglCmdDrawCircle {
    uint8_t  layerId;
    uint8_t  flags;           // bit0: filled, bit1: border, bit2: ellipse
    int16_t  cx, cy;          // Center
    uint16_t rx;              // Radius X
    uint16_t ry;              // Radius Y (same as rx for circle)
    PglColor3 fillColor;
    PglColor3 borderColor;
    uint8_t  borderWidth;
};

// CMD_DRAW_SPRITE (0xA4)
struct PglCmdDrawSprite {
    uint8_t  layerId;
    uint16_t textureId;       // Source texture
    int16_t  x, y;            // Destination position
    uint16_t width, height;   // Destination size (stretches texture)
    uint16_t srcX, srcY;      // Source rect within texture (atlas support)
    uint16_t srcW, srcH;      // Source rect size
    uint8_t  opacity;         // 0–255 (per-sprite alpha)
    uint8_t  flags;           // bit0: flipH, bit1: flipV, bit2: colorKey
    PglColor3 keyColor;       // Transparent color (when colorKey flag set)
};

enum PglSpriteFlags : uint8_t {
    PGL_SPRITE_FLIP_H        = 0x01,
    PGL_SPRITE_FLIP_V        = 0x02,
    PGL_SPRITE_COLOR_KEY     = 0x04,
    PGL_SPRITE_ROTATE_90     = 0x08,
    PGL_SPRITE_SRC_SEQUENCE  = 0x10,  // textureId is an ImageSequence, not a Texture
};

// CMD_DRAW_TEXT (0xA5) — header; followed by text string bytes
struct PglCmdDrawTextHeader {
    uint8_t  layerId;
    int16_t  x, y;            // Baseline position
    PglColor3 color;
    uint8_t  fontSize;        // Font size: 0=5x7 (built-in), 1=8x12, 2=custom (fontId follows)
    uint8_t  flags;           // bit0: bold, bit1: background fill
    PglColor3 backgroundColor; // When background fill enabled
    uint8_t  textLength;      // Number of bytes following this header
    // If fontSize == 2: next byte is fontId (references CMD_CREATE_FONT resource)
    // Followed by textLength bytes of ASCII text
};

// CMD_DRAW_TRIANGLE_2D (0xA6)
struct PglCmdDrawTriangle2D {
    uint8_t  layerId;
    uint8_t  flags;           // bit0: filled, bit1: border
    int16_t  x0, y0;
    int16_t  x1, y1;
    int16_t  x2, y2;
    PglColor3 fillColor;
    PglColor3 borderColor;
};

// CMD_SPRITE_BATCH_BEGIN (0xA7)
struct PglCmdSpriteBatchBegin {
    uint8_t  layerId;
    uint16_t textureId;       // Shared texture atlas for the whole batch
    uint16_t spriteCount;     // Number of sprites to follow
    uint8_t  blendMode;       // PglBlendMode for all sprites in this batch
};

// CMD_SPRITE_BATCH_ENTRY (0xA8) — one per sprite in batch
struct PglCmdSpriteBatchEntry {
    int16_t  x, y;            // Dest position
    uint16_t srcX, srcY;      // Source rect in atlas
    uint16_t srcW, srcH;      // Source rect size
    uint8_t  opacity;
    uint8_t  flags;           // PglSpriteFlags
};

// CMD_SET_CLIP_RECT (0xAA)
struct PglCmdSetClipRect {
    uint8_t  layerId;
    int16_t  x, y;
    uint16_t width, height;
    uint8_t  flags;           // bit0: enable clipping, bit1: reset to full viewport
};

// CMD_SET_VIEWPORT (0xAB)
struct PglCmdSetViewport {
    uint8_t  renderTargetId;
    uint16_t x, y;
    uint16_t width, height;
};

// CMD_CLEAR_LAYER (0xAC)
struct PglCmdClearLayer {
    uint8_t  layerId;
    PglColor3 color;
    uint8_t  alpha;           // For alpha-blended layers
};

// CMD_DRAW_GRADIENT_RECT (0xAE)
struct PglCmdDrawGradientRect {
    uint8_t  layerId;
    int16_t  x, y;
    uint16_t width, height;
    PglColor3 topLeftColor;
    PglColor3 topRightColor;
    PglColor3 bottomLeftColor;
    PglColor3 bottomRightColor;
    uint8_t  flags;           // bit0: radial gradient (else linear)
};

// CMD_SET_LAYER_SHADER (0xB0)
struct PglCmdSetLayerShader {
    uint8_t  layerId;         // Target layer (0–7)
    uint16_t shaderId;        // PSB shader program ID (0xFFFF = unbind)
    uint8_t  paramCount;      // Number of float params following (0–16)
    // Followed by paramCount × 4 bytes of float shader parameters
    // These are loaded into shader registers r16–r31 before execution
};

#pragma pack(pop)
```

---

## §4 GPU-Side Compositing Pipeline

### §4.1 Render Pass Order

The GPU processes layers in a defined order each frame:

```
BeginFrame
├── Phase 1: Layer Setup
│   └── Process CMD_SET_LAYER commands → build layer table (including shader bindings)
├── Phase 2: 3D Render Pass (existing pipeline)
│   ├── PrepareFrame (transform, project, cull, QuadTree)
│   ├── DispatchTilePass (dual-core rasterization, Z-buffer)
│   └── ScreenspaceShaders (post-processing on 3D layer)
├── Phase 3: 2D Render Pass (NEW)
│   ├── For each 2D layer (sorted by layerId):
│   │   ├── Set clip rect / viewport
│   │   ├── Process 2D draw commands (rects, lines, sprites, text)
│   │   └── Apply per-layer opacity
│   └── Sprite batch processing (sorted by texture for cache efficiency)
├── Phase 3.5: Per-Layer Shader Pass (NEW)
│   ├── For each layer with shaderId ≠ PGL_NO_SHADER:
│   │   ├── Bind layer framebuffer as shader input texture (sampler 0)
│   │   ├── Execute PSB shader program per pixel
│   │   └── Write result back to layer framebuffer (in-place)
│   └── Shader runs on Core 1 while Core 0 begins compositing setup
├── Phase 4: Layer Compositing (NEW)
│   ├── Composite all layers back-to-front into final framebuffer
│   ├── Apply layer blend modes and global opacity
│   └── Handle color key transparency
└── Phase 5: Display Output
    └── Swap framebuffers → DisplayManager::PresentFrame()
```

### §4.2 2D Rasterizer

A lightweight 2D rasterizer running on the GPU, separate from the 3D pipeline:

```cpp
namespace Rasterizer2D {
    /// Draw a filled rectangle with optional border and rounded corners.
    void DrawRect(uint16_t* fb, uint16_t fbW, uint16_t fbH,
                  int16_t x, int16_t y, uint16_t w, uint16_t h,
                  uint16_t fillColor, uint16_t borderColor,
                  uint8_t borderWidth, uint8_t cornerRadius,
                  const ClipRect* clip);

    /// Draw a line using Bresenham's algorithm with thickness support.
    void DrawLine(uint16_t* fb, uint16_t fbW, uint16_t fbH,
                  int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                  uint16_t color, uint8_t width,
                  const ClipRect* clip);

    /// Draw a filled/outlined circle or ellipse using midpoint algorithm.
    void DrawCircle(uint16_t* fb, uint16_t fbW, uint16_t fbH,
                    int16_t cx, int16_t cy, uint16_t rx, uint16_t ry,
                    uint16_t fillColor, uint16_t borderColor,
                    uint8_t borderWidth, bool filled,
                    const ClipRect* clip);

    /// Draw a textured sprite with atlas support, flipping, and color keying.
    void DrawSprite(uint16_t* fb, uint16_t fbW, uint16_t fbH,
                    const TextureSlot* texture,
                    int16_t dstX, int16_t dstY, uint16_t dstW, uint16_t dstH,
                    uint16_t srcX, uint16_t srcY, uint16_t srcW, uint16_t srcH,
                    uint8_t opacity, uint8_t flags, uint16_t keyColor,
                    const ClipRect* clip);

    /// Draw text using built-in bitmap font.
    void DrawText(uint16_t* fb, uint16_t fbW, uint16_t fbH,
                  int16_t x, int16_t y, const char* text, uint8_t textLen,
                  uint16_t color, uint8_t fontSize,
                  uint16_t bgColor, bool hasBg,
                  const ClipRect* clip);

    /// Draw a 2D filled triangle using scanline fill.
    void DrawTriangle(uint16_t* fb, uint16_t fbW, uint16_t fbH,
                      int16_t x0, int16_t y0,
                      int16_t x1, int16_t y1,
                      int16_t x2, int16_t y2,
                      uint16_t fillColor, uint16_t borderColor,
                      bool filled, bool border,
                      const ClipRect* clip);

    /// Draw gradient-filled rectangle (bilinear interpolation).
    void DrawGradientRect(uint16_t* fb, uint16_t fbW, uint16_t fbH,
                          int16_t x, int16_t y, uint16_t w, uint16_t h,
                          uint16_t tlColor, uint16_t trColor,
                          uint16_t blColor, uint16_t brColor,
                          const ClipRect* clip);
}
```

### §4.3 Sprite Batch Optimization

Sprite batches share a single texture atlas and are sorted to minimize texture switches:

```
Batch Processing:
1. Sort batch entries by Y-coordinate (for cache-friendly scanline access)
2. For each sprite in batch:
   a. Clip to viewport
   b. Sample from texture atlas (nearest-neighbor or bilinear)
   c. Apply per-sprite opacity
   d. Write to layer framebuffer
3. Single texture binding per batch → maximizes memory cache hits
```

Performance target: 128 sprites per frame at 16x16 pixels = ~32K pixel writes → ~0.2 ms @ 150 MHz.

### §4.4 Layer Compositing Engine

```cpp
namespace Compositor {
    /// Composite source layer onto destination buffer.
    /// Handles all blend modes, per-pixel alpha, and global opacity.
    void CompositeLayer(uint16_t* dst, const uint16_t* src,
                        uint16_t width, uint16_t height,
                        PglLayerBlend blendMode, uint8_t opacity);

    /// Color-key composite: transparent where src matches keyColor.
    void CompositeLayerKeyed(uint16_t* dst, const uint16_t* src,
                             uint16_t width, uint16_t height,
                             uint16_t keyColor, uint8_t opacity);
}
```

---

## §5 Host-Side API Extensions (PglEncoder)

### §5.1 Layer Management

```cpp
class PglEncoder {
    // ... existing methods ...

    // ─── Layer Management ────────────────────────────────────────────

    /// Configure a compositing layer.
    void SetLayer(uint8_t layerId, PglLayerType type, PglLayerBlend blendMode,
                  uint8_t opacity, bool visible, uint8_t renderTargetId,
                  uint16_t viewX, uint16_t viewY, uint16_t viewW, uint16_t viewH,
                  uint16_t shaderId = PGL_NO_SHADER);

    /// Clear a layer to solid color.
    void ClearLayer(uint8_t layerId, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);

    // ─── 2D Drawing Commands ─────────────────────────────────────────

    /// Draw a filled or outlined rectangle.
    void DrawRect(uint8_t layerId, int16_t x, int16_t y, uint16_t w, uint16_t h,
                  uint8_t r, uint8_t g, uint8_t b,
                  bool filled = true, uint8_t borderWidth = 0,
                  uint8_t borderR = 0, uint8_t borderG = 0, uint8_t borderB = 0,
                  uint8_t cornerRadius = 0);

    /// Draw a line segment.
    void DrawLine(uint8_t layerId, int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                  uint8_t r, uint8_t g, uint8_t b, uint8_t width = 1);

    /// Draw a filled or outlined circle (or ellipse).
    void DrawCircle(uint8_t layerId, int16_t cx, int16_t cy, uint16_t rx, uint16_t ry,
                    uint8_t r, uint8_t g, uint8_t b, bool filled = true,
                    uint8_t borderWidth = 0,
                    uint8_t borderR = 0, uint8_t borderG = 0, uint8_t borderB = 0);

    /// Draw a textured sprite (atlas-aware).
    void DrawSprite(uint8_t layerId, PglTexture textureId,
                    int16_t x, int16_t y, uint16_t w, uint16_t h,
                    uint16_t srcX = 0, uint16_t srcY = 0,
                    uint16_t srcW = 0, uint16_t srcH = 0,  // 0 = full texture
                    uint8_t opacity = 255, uint8_t flags = 0,
                    const PglColor3* keyColor = nullptr);

    /// Draw text using built-in GPU font.
    void DrawText(uint8_t layerId, int16_t x, int16_t y,
                  const char* text, uint8_t textLen,
                  uint8_t r, uint8_t g, uint8_t b,
                  uint8_t fontSize = 0, bool bold = false,
                  const PglColor3* bgColor = nullptr);

    /// Draw a filled 2D triangle.
    void DrawTriangle2D(uint8_t layerId,
                        int16_t x0, int16_t y0,
                        int16_t x1, int16_t y1,
                        int16_t x2, int16_t y2,
                        uint8_t r, uint8_t g, uint8_t b, bool filled = true);

    /// Draw a gradient-filled rectangle.
    void DrawGradientRect(uint8_t layerId, int16_t x, int16_t y, uint16_t w, uint16_t h,
                          const PglColor3& topLeft, const PglColor3& topRight,
                          const PglColor3& bottomLeft, const PglColor3& bottomRight);

    // ─── Sprite Batching ─────────────────────────────────────────────

    /// Begin a sprite batch with shared texture atlas.
    void SpriteBatchBegin(uint8_t layerId, PglTexture textureId,
                          uint16_t spriteCount, PglBlendMode blendMode);

    /// Add a sprite entry to the current batch.
    void SpriteBatchEntry(int16_t x, int16_t y,
                          uint16_t srcX, uint16_t srcY,
                          uint16_t srcW, uint16_t srcH,
                          uint8_t opacity = 255, uint8_t flags = 0);

    /// End and submit the sprite batch.
    void SpriteBatchEnd();

    // ─── Clipping & Viewport ─────────────────────────────────────────

    /// Set 2D clipping rectangle for a layer.
    void SetClipRect(uint8_t layerId, int16_t x, int16_t y,
                     uint16_t w, uint16_t h, bool enable = true);

    /// Set viewport (rendering sub-region) for a render target.
    void SetViewport(uint8_t renderTargetId,
                     uint16_t x, uint16_t y, uint16_t w, uint16_t h);

    // ─── Per-Layer Shader ───────────────────────────────────────────

    /// Bind a PSB shader program to a 2D layer.
    /// The shader executes per-pixel on the layer’s framebuffer after all
    /// 2D draw commands for the layer have completed (Phase 3.5).
    /// Pass PGL_NO_SHADER (0xFFFF) to unbind.
    void SetLayerShader(uint8_t layerId, uint16_t shaderId,
                        const float* params = nullptr, uint8_t paramCount = 0);
};
```

---

## §6 Enhanced 3D Pipeline Integration

### §6.1 Depth-Tested 2D (Billboard Sprites in 3D)

For integrating 2D elements into the 3D scene (e.g., particle effects, labels):

```cpp
/// CMD_DRAW_BILLBOARD (0xAF) — 2D sprite placed in 3D world space
struct PglCmdDrawBillboard {
    uint8_t  layerId;         // Must target a PGL_LAYER_3D_SCENE layer
    uint16_t textureId;
    PglVec3  worldPosition;   // 3D position → projected to screen
    float    worldWidth;      // Size in world units
    float    worldHeight;
    uint16_t srcX, srcY;      // Texture atlas source rect
    uint16_t srcW, srcH;
    uint8_t  opacity;
    uint8_t  flags;           // PglSpriteFlags + bit4: depthTest, bit5: depthWrite
};
```

### §6.2 Stencil Buffer (Future)

A 1-bit stencil buffer for masking regions:

```cpp
/// Stencil operations (future extension)
enum PglStencilOp : uint8_t {
    PGL_STENCIL_KEEP     = 0,
    PGL_STENCIL_SET      = 1,
    PGL_STENCIL_CLEAR    = 2,
    PGL_STENCIL_INVERT   = 3,
};
```

### §6.3 Per-Layer Shader Effects

The PSB (ProtoGL Shader Bytecode) programmable shader system — already used for
3D screenspace post-processing — can also be applied to individual 2D layers.
This enables GPU-accelerated visual effects on UI elements without re-issuing
draw commands from the host.

#### How It Works

1. **Host binds a shader** to a layer via `SetLayerShader(layerId, shaderId, params)`
   or includes `shaderId` in `CMD_SET_LAYER`.
2. **GPU draws the layer** normally (rects, sprites, text, etc.) into the layer’s
   framebuffer during Phase 3.
3. **Phase 3.5** runs the bound PSB shader as a full-screen (or viewport-scoped)
   pass over the layer’s framebuffer:
   - The layer framebuffer is bound as **sampler 0** (read).
   - The shader output overwrites the layer framebuffer (write).
   - Shader registers `r16–r31` are pre-loaded with the host-supplied float params.
   - Built-in uniforms: `r0` = pixel X, `r1` = pixel Y, `r2` = layer width,
     `r3` = layer height, `r4` = time (seconds), `r5` = frame number.
4. **Phase 4** then composites the shader-processed layer into the final output.

#### Example Per-Layer Shader Effects

| Effect | Shader Approach | Use Case |
|--------|----------------|----------|
| **Gaussian Blur** | Multi-tap weighted sample from sampler 0 | Frosted-glass menu background |
| **Drop Shadow** | Offset sample + darken | UI panel shadow |
| **Glow / Bloom** | Bright-pass + blur + additive blend | Health bar pulsing glow |
| **Color Correction** | Matrix multiply on RGB | Layer-specific color grading |
| **Scanline Effect** | Darken every other row | Retro CRT aesthetic |
| **Dissolve / Fade** | Noise threshold vs. opacity param | Menu transition animation |
| **Pixelate** | Snap UV to grid, sample at grid center | Stylized effect |
| **Chromatic Aberration** | Offset R/G/B channel samples | Damage indication |

#### Shader Execution Budget

At 128×64 pixels, a shader with ~20 instructions per pixel requires:

$$
128 \times 64 \times 20 = 163{,}840 \text{ instructions} \approx 1.1\text{ ms at 150 MHz}
$$

This is acceptable for one or two shader-enhanced layers per frame at 30 fps.
For complex effects (blur with 9+ taps), restrict the shader to the layer’s
viewport rather than the full framebuffer, or reduce the working resolution and
upscale.

#### Wire Format Example

```cpp
// Host: blur the UI layer with radius=2.0
enc->SetLayerShader(2, blurShaderId, &blurRadius, 1);

// Equivalent wire: 0xB0 | layerId=2 | shaderId=blurId | paramCount=1 | float(2.0)
```

#### Restrictions

- Only one shader per layer (chain effects by using multiple layers).
- Shader runs in-place: no separate output buffer (saves SRAM).
- Texture samplers 1–3 remain available for the shader to read other textures.
- Layer shaders execute **after** all 2D draw commands, **before** compositing.

---

## §7 GPU-Side State Changes

### §7.1 Scene State Additions

```cpp
// Added to SceneState
struct LayerSlot {
    bool           active;
    PglLayerType   type;
    PglLayerBlend  blendMode;
    uint8_t        opacity;
    bool           visible;
    uint8_t        renderTargetId;
    uint16_t       shaderId;        // PSB shader bound to this layer (0xFFFF = none)
    float          shaderParams[16]; // Custom params loaded into r16–r31
    uint8_t        shaderParamCount;
    uint16_t       viewportX, viewportY, viewportW, viewportH;
    ClipRect       clipRect;
    bool           clipEnabled;
};

// 2D draw command queue (ring buffer within command frame)
struct Draw2DCommand {
    uint8_t  opcode;    // 0xA1–0xAE
    uint8_t  layerId;
    uint8_t  data[64];  // Serialized command payload
};

// SceneState extensions
struct SceneState {
    // ... existing fields ...

    // Layer system (NEW)
    LayerSlot  layers[PGL_MAX_LAYERS];
    uint8_t    activeLayerCount;

    // 2D draw commands (processed in order per layer, NEW)
    Draw2DCommand draw2dQueue[128];  // Max 128 2D draw commands per frame
    uint16_t      draw2dCount;

    // Render targets (NEW)
    uint16_t* renderTargetBuffers[PGL_MAX_RENDER_TARGETS];
    uint16_t  renderTargetWidths[PGL_MAX_RENDER_TARGETS];
    uint16_t  renderTargetHeights[PGL_MAX_RENDER_TARGETS];
};
```

### §7.2 SRAM Impact

| Component | SRAM Cost | Notes |
|-----------|-----------|-------|
| `LayerSlot[8]` | ~640 bytes | Layer metadata + shader params (16 floats each) |
| `Draw2DCommand[128]` | ~8.5 KB | 2D command queue |
| Layer framebuffers | 16 KB each | Only for active 2D layers |
| Sprite batch staging | ~2 KB | Temporary batch storage |
| Built-in font data | ~1.5 KB | 5×7 + 8×12 glyphs (always resident) |
| Custom font atlases | 0 bytes SRAM | Stored in external VRAM (prefer MRAM channel); glyph metrics ~8 bytes × count cached |
| Image sequence atlases | 0 bytes SRAM | Stored in external VRAM (Tier 1/2); active frame cached in arena |
| Image sequence active frame cache | up to 8 KB | One frame cached in SRAM cache arena per active sequence |
| Per-layer shader exec | ~0 bytes extra | Runs in-place on layer framebuffer |
| **Total new overhead** | **~12.6 KB** | Without extra layer framebuffers |

Since layer framebuffers share the existing render target system, the actual cost depends on how many layers need separate buffers. For the common case (1 3D layer + 1 2D overlay), the 2D layer can composite directly into the 3D framebuffer, requiring no extra buffer.

---

## §8 Host-Side Usage Example

```cpp
PglDevice device;
// ... initialize device ...

device.BeginFrame(frameNum, deltaTimeUs);
auto* enc = device.GetEncoder();

// Configure layers
enc->SetLayer(0, PGL_LAYER_BACKGROUND, PGL_LAYER_BLEND_OPAQUE,
              255, true, 0, 0, 0, 128, 64);
enc->SetLayer(1, PGL_LAYER_3D_SCENE, PGL_LAYER_BLEND_OPAQUE,
              255, true, 0, 0, 0, 128, 64);
enc->SetLayer(2, PGL_LAYER_2D_CANVAS, PGL_LAYER_BLEND_ALPHA,
              200, true, 0, 0, 0, 128, 64);

// Layer 0: Clear background to dark blue
enc->ClearLayer(0, 0, 0, 32);

// Layer 1: 3D scene (existing API — cameras, draw calls, shaders)
enc->SetCamera(0, 0, position, rotation, scale, lookOffset, baseRotation, false);
enc->DrawObject(meshId, materialId, pos, rot, scl, baseRot, scrOff, sclOff, rotOff, true);

// Layer 2: UI overlay
enc->DrawRect(2, 2, 2, 40, 12, 255, 255, 255,
              false, 1, 128, 128, 128, 2);   // Rounded white border

enc->DrawText(2, 5, 10, "FPS: 60", 7, 0, 255, 0, 0); // Green text

// Health bar
enc->DrawRect(2, 80, 4, 44, 8, 64, 64, 64, true);      // Background
enc->DrawRect(2, 81, 5, 30, 6, 0, 255, 0, true);        // Fill (green)

// Sprite icon
enc->DrawSprite(2, iconTexture, 60, 2, 10, 10, 0, 0, 0, 0, 255, 0, nullptr);

// Apply a blur shader to the UI layer (frosted-glass effect)
float blurRadius = 1.5f;
enc->SetLayerShader(2, blurShaderId, &blurRadius, 1);

device.EndFrame();
```

---

## §9 Implementation Plan

### Phase 1: Layer Infrastructure (M11, Week 28-29)
- Layer table in SceneState
- CMD_SET_LAYER / CMD_CLEAR_LAYER parsing
- Background layer (solid color clear)
- No compositing yet — layers written directly to primary framebuffer

### Phase 2: 2D Primitives (M11, Week 30-31)
- Implement `Rasterizer2D` namespace (rect, line, circle, triangle)
- Clip rect support
- 2D draw command queue processing

### Phase 3: Sprite & Text (M12, Week 32-33)
- Sprite drawing with atlas support
- Color keying transparency
- Built-in font rendering (5×7 + 8×12)
- Sprite batching with texture-sort optimization

### Phase 4: Full Compositing (M13, Week 34-35)
- Compositor engine (alpha blend, additive, multiply, color key)
- Per-layer opacity
- Multi-layer ordering and render pass scheduling

### Phase 5: Host API Integration (M13, Week 36)
- PglEncoder 2D methods
- GPUDriverController UI helpers
- Usage documentation and examples

---

## Related Documents

- [Display_Frontend_Design.md](Display_Frontend_Design.md) — Unified display driver interface
- [GPU_API_Design.md](GPU_API_Design.md) — Full GPU design
- [ProtoGL_API_Spec.md](ProtoGL_API_Spec.md) — Wire-format specification
- [Memory_Management_API.md](Memory_Management_API.md) — Refined memory management
