# Rendering Pipeline Review — ProtoTracer ESP32-S3

> **Date**: 2026-05-31 | **Key Files**: `Render/Camera.h`, `Render/Scene.h`, `Render/PixelGroup.h`, `Render/QuadTree.h`, `Render/Node.h`, `Render/Triangle2D.h`

## Current Face/Panel Facts

- Face model: 339 vertices, 326 triangles, 85 morph targets.
- Active S3 pixel group: `PixelGroup(2048, P3HUB75)`.
- The active `P3HUB75` path does not allocate `rectCoords`; that allocation only applies to rectangular PixelGroups.

## 1. Pipeline Overview

```
loop() every frame:
  animation.UpdateTime(ratio)     → updates morphs, materials, face mesh
  controller.Render(scene)        → Camera::Rasterize()
  controller.Display()            → HUB75 DMA push (virtualDisp->drawPixelRGB888 × 4096)
```

### Render Flow (Camera::Rasterize, ~250 lines in Camera.h):

1. **2D Fast Path**: If `is2D`, iterate pixels, sample material color directly (O(N), no quadtree)
2. **3D Path** (the main rendering path):
   a. Compute inverse view rotation + camera position
   b. **Batch scale** all 2048 pixel rays by transform scale (loop)
   c. **ESP-DSP rotate** all rays using 2×2 rotation matrix (dsps_mulc_f32 + dsps_add_f32)
   d. Build transformed bounding box
   e. **Create QuadTree** from scratch
   f. For each enabled object in scene → for each triangle → project to 2D → insert into QuadTree
   g. **QuadTree::Rebuild()** → subdivide nodes
   h. For each pixel → QuadTree::Intersect(ray) → get leaf node → CheckRasterPixel (depth test + barycentric)
   i. Write final color to pixelGroup->GetColors()[i]

## 2. ESP-DSP Acceleration — Review

### What's Accelerated:
```cpp
// Camera.h lines 215-222: Batch 2D rotation of all 2048 pixel rays
dsps_mulc_f32(tmpX, rotX, pixelCount, rot2.m00, 1, 1);
dsps_mulc_f32(tmpY, rotY, pixelCount, rot2.m10, 1, 1);
dsps_add_f32(rotX, rotY, rotX, pixelCount, 1, 1, 1);
dsps_mulc_f32(tmpY, rotY, pixelCount, rot2.m11, 1, 1);
dsps_mulc_f32(tmpX, tmpX, pixelCount, rot2.m01, 1, 1);
dsps_add_f32(rotY, tmpX, rotY, pixelCount, 1, 1, 1);
```

This replaces 2048 per-pixel scalar multiply-add pairs with SIMD vector operations.
The ESP32-S3 has a **vector/SIMD unit** that `esp-dsp` leverages. This is good.

### What's NOT Accelerated (and should be):
- **Triangle projection** (Quaternion::RotateVector × 3 per triangle) — runs per-triangle in the QuadTree Insert loop
- **Barycentric intersection test** (Triangle2D::DidIntersect) — runs per-pixel × per-leaf-entity
- **Depth test loop** in CheckRasterPixel
- The `for(unsigned int i = 0; i < pixelCount; ++i)` loop after DSP that copies to cachedRays and updates bounds

### ✅ DSP Instruction Usage is Correct
The `dsps_mulc_f32` and `dsps_add_f32` calls use proper stride (1,1) and correct buffer management. The rotate-then-copy pattern is a valid optimization.

## 3. QuadTree — CRITICAL MEMORY ISSUE ⚠️

### Problem: Per-Frame Rebuild Causes Internal DRAM Thrash

```cpp
// Camera::Rasterize() — EVERY frame:
QuadTree tree(transformedBounds);  // stack, but...

// For each object, for each triangle:
tree.Insert(Triangle2D(invView, camPos, &triangles[j], material));
// ↑ This creates a temporary Triangle2D on stack, then COPIES it into
//   QuadTree's internal `entities` array (heap via realloc)

tree.Rebuild();  // Subdivides: new Node[4], new Triangle2D*[] per node
```

**Allocation pattern per frame**:
- `QuadTree::Expand()`: `realloc(entities, ...)` — may move the entities array
- `Node::Subdivide()`: `new Node[4]` per subdivision level
- `Node::Expand()`: `new Triangle2D*[newCount]`, `delete[] tmp` per node
- Then everything is freed in `~QuadTree()` and `~Node()`

**Fragment count per frame**: For the current 326-triangle face mesh, expect:
- 1 QuadTree entities array (realloc'd ~3-4 times)
- ~15-30 Node objects with entity arrays
- Each allocation/deallocation cycle contributes to fragmentation

### Current Mitigation Attempts (partial):
- `maxEntities = 16` in both Node and QuadTree (reasonable)
- `maxDepth = 8` (stops infinite recursion for degenerate meshes)
- `maxSubdivRatio = 0.5f` (stops subdividing if not improving)
- `Expand()` doubles capacity (2× growth factor is standard)

### ⚠️ Missing: No Pool/Persistent Allocation
The QuadTree is **destroyed at end of Rasterize()** and **recreated next frame**. This is the single largest source of per-frame heap churn.

## 4. PixelGroup Memory Layout

### Internal RAM Allocations (2048 pixels):
| Array | Size | Heap | Notes |
|---|---|---|---|
| pixelColors | 6,144 B | internal preferred | Hot: written every frame, read by Display() |
| pixelBuffer | 6,144 B | internal preferred | Used by effects (blur, AA) |
| up/down/left/right | 32,768 B | **`new[]` = internal only** | Neighbor indices for effects |
| rectCoords | 0 B on current P3HUB75 path | only rectangular constructor | Not active for `PixelGroup(2048, P3HUB75)` |

**Total internal DRAM**: ~45 KB for the active non-rectangular 2048-pixel group (`pixelColors`, `pixelBuffer`, and four neighbor arrays).
The second PixelGroup (camPixels2, 4 pixels) is negligible.

### ⚠️ Issue: Neighbor Arrays Are Heavy
The `up/down/left/right` arrays at 32 KB combined represent pixel adjacency for screen-space effects (blur, anti-aliasing). These are **read-only after initialization** and could safely live in PSRAM without performance impact, since screen-space effects run in a separate pass.

## 5. Camera SIMD Buffers

```cpp
tmpX = heap_caps_malloc(2048*4, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL); // 8 KB
tmpY = heap_caps_malloc(2048*4, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL); // 8 KB
rotX = heap_caps_malloc(2048*4, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL); // 8 KB
rotY = heap_caps_malloc(2048*4, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL); // 8 KB
cachedRays = new Vector2D[2048];  // 16 KB → internal only
```

**Total**: ~48 KB internal DRAM for render scratch buffers.

### ✅ These buffers are correctly in internal RAM
DSP vector operations need fast access. However, the `cachedRays` array (used only for the QuadTree intersect loop after DSP is done) could move to PSRAM.

## 6. Scene Object Storage

```cpp
// Scene constructor:
objects = heap_caps_aligned_alloc(16, maxObjects * sizeof(Object3D*),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
objectsShadow = heap_caps_malloc(maxObjects * sizeof(Object3D*),
                                  MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
```

- `objects`: PSRAM, aligned, DMA-capable — correct
- `objectsShadow`: Internal RAM mirror — **questionable**. For `maxObjects=1` (the typical case with a single face model), this is just 8 bytes. But the code comment says "iteration speed" — iterating a pointer array from PSRAM is slightly slower but safe. The shadow adds complexity for negligible gain here.

## 7. Triangle3D::DidIntersect — IRAM Placement

```cpp
// Triangle3D.h line 47:
bool IRAM_ATTR DidIntersect(const Vector3D& ray, ...);
```

This is the ONLY function explicitly placed in IRAM. It is indeed performance-critical (called per-pixel × per-triangle). However:

### ⚠️ Missing IRAM Functions:
- `Triangle2D::DidIntersect()` — the 2D barycentric test used in CheckRasterPixel (MORE frequently called than the 3D version)
- `Node::Intersect()` — quadtree traversal per pixel
- The DSP batch loops in Camera::Rasterize — the `for` loops calling `pixelGroup->GetCoordinate(i)` run 2048 times per frame from flash

## 8. Object3D Per-Frame Vertex Transform

```cpp
// Object3D::UpdateTransform() — called every frame for each object:
for (int i = 0; i < modifiedTriangles->GetVertexCount(); i++) {
    Vector3D modifiedVector = modifiedTriangles->GetVertices()[i];
    modifiedVector = (modifiedVector - scaleOffset) * scale + scaleOffset;
    modifiedVector = rotation.RotateVector(modifiedVector - rotOffset) + rotOffset;
    modifiedVector = modifiedVector + position;
    modifiedTriangles->GetVertices()[i] = modifiedVector;
}
```

This runs on every vertex of the face mesh (typically ~500-2000 vertices). Each iteration does:
- Multiple Vector3D operations (subtract, multiply, add)
- Quaternion rotation (expensive: 2 quaternion multiplies internally)
- Array write-back

This could benefit from ESP-DSP vectorization since the operations are uniform across all vertices.

## 9. Critical Questions

## 9. New Acceleration Findings

- A conventional 64x32 triangle rasterizer with a fixed z-buffer should be faster and safer than rebuilding a QuadTree every frame.
- `TasESP32S3KitV1::Display()` mirrors every pixel to `M5UnitGLASS2`; this should be throttled or disabled for production.
- Boop-triggered `Surprised` enables `blurV`, which adds a full screen-space effect pass.
- `JsonDrivenProtogenAnimation::Update()` calls `ApplyExpression()` every frame, causing repeated string lookups and morph-name scans.
- `MaterialAnimator<10>` has loop bounds that can access one past fixed arrays when all default material slots are registered.
- `JsonDrivenProtogenAnimation` is `Animation<1>` but adds face and background; the background is dropped by `Scene::AddObject()`.
- Current debug build (`-Og`) should not be used as the final speed baseline.
- Screen-space blur effects contain correctness bugs, and `SpectrumAnalyzer` has ineffective bounds checks. Fix these before profiling analyzer/background modes.

For the detailed acceleration plan, see `render-acceleration-review.md`.

1. **Q: How many triangles does the typical face model have?** This determines QuadTree node count and intersection test load.

2. **Q: Has the team considered persisting the QuadTree across frames?** Since camera position changes smoothly, reusing the previous frame's tree structure (rebuilding only when needed) could eliminate the per-frame alloc/free storm.

3. **Q: Is the `objectsShadow` array providing measurable performance benefit?** If not, removing it saves internal RAM and complexity.

4. **Q: Are the `up/down/left/right` neighbor arrays actually used?** If screen effects are disabled, these 32 KB of internal RAM are wasted.

5. **Q: What is the measured frame time breakdown?** (animation update vs render vs display)
