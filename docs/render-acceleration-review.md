# Render and Rasterization Acceleration Review

> Date: 2026-05-31 | Target: ESP32-S3 N8R8 | Face model: 339 vertices, 326 triangles, 85 morphs

## Executive Summary

The renderer can be accelerated, but the highest return is not another small ESP-DSP call. The current raster path spends a lot of time on dynamic spatial structure rebuilds, per-pixel virtual calls, repeated string/morph lookups, and optional screen-space blur. The best long-term speedup is to replace the per-frame QuadTree with a fixed-size triangle rasterizer or fixed tile-bin rasterizer for the 64x32 HUB75 target.

The freeze-after-boop clue also changes the performance picture: the default animation maps the first boop to `Surprised`, and `Surprised` enables `blurV`. That adds a full screen-space blur pass exactly when the user triggers the gesture sensor.

## Confirmed Runtime Model

- Active controller: `TasESP32S3KitV1`.
- Active pixel layout: `PixelGroup(2048, P3HUB75)`, not the rectangular `PixelGroup` constructor.
- Active face mesh: 339 vertices, 326 triangles, 85 morphs.
- Active visible render buffer: one 2048-pixel camera duplicated to two 64x32 HUB75 halves in `Display()`.
- Active JSON expression file: `example_animation.json` or device animation JSON with equivalent expression structure.

## Corrections to Earlier Notes

- `rectCoords` is not allocated for the current S3 `P3HUB75` path. That 16 KB item only applies to rectangular PixelGroups.
- The current face mesh has 326 triangles, not an estimated 200 triangles.
- The gesture path does more than assign `activeBoopExpression`; it can enable `VerticalBlur` through the `Surprised` expression.
- `MaterialAnimator<10>` is already filled to 10 slots by default material registration, and several loops use `<= currentMaterials`, which can access index 10 of arrays sized 10.

## Highest-Impact Speedups

### 0. Build/Placement Baseline Before Comparing Speed

The current `platformio.ini` uses a debug-oriented setup:

```ini
build_type = debug
build_flags =
  -g -Og
```

`-Og` is useful for debugging but is not a good measurement baseline for real-time raster speed. Keep a debug environment for crash work, but add a production/profile environment using `-O2` or `-Os` before judging renderer performance.

Also verify the map file for hot data placement. `P3HUB75` is a `const static Vector2D[2048]` table, about 16 KB. With PSRAM rodata options enabled, it may not be in the fastest memory path. Since `Camera::Rasterize()` reads pixel coordinates every frame, either keep this table in internal/flash-cached memory or copy `baseX/baseY` arrays into internal RAM once during camera initialization.

For hot functions, candidate IRAM placement:

- `Triangle2D::DidIntersect(float, float, ...)`
- `QuadTree::Intersect()` if QuadTree remains
- direct triangle rasterizer inner loop if QuadTree is replaced
- fixed-grid blur inner loops

Be selective: IRAM is limited, and placing too much code there can starve other internal memory consumers.

### 1. Replace QuadTree With Fixed Triangle Rasterization

Current path:

```cpp
for each frame:
  build all pixel rays
  build QuadTree from projected triangles
  for each pixel:
    find leaf
    test triangles in leaf
```

Better path for 64x32:

```cpp
clear color buffer
clear zbuffer[2048]
for each projected triangle:
  compute pixel-space bounding box
  for each pixel inside bbox:
    barycentric edge test
    interpolate depth
    if depth passes: write color
```

Why this helps:

- Removes all per-frame QuadTree heap allocation.
- Removes `Node` tree traversal per pixel.
- Avoids `Triangle2D::DidIntersect(BoundingBox2D)` SAT tests during tree subdivision.
- Works naturally with a fixed 64x32 panel.
- Gives a true per-pixel z-buffer instead of using triangle average depth.

Expected impact: largest stability improvement and likely largest render-time improvement.

Memory cost:

- `float zbuffer[2048]`: 8192 bytes internal RAM.
- Or fixed-point/uint16 depth buffer if camera depth range is constrained: 4096 bytes.

### 2. If QuadTree Must Stay: Make It Static or Arena-Based

If replacing the rasterizer is too large for the next firmware revision, use a fixed arena:

- Preallocate `Triangle2D projectedTriangles[326]`.
- Preallocate `Node nodePool[MAX_NODES]`.
- Preallocate `Triangle2D* nodeEntityPool[MAX_NODE_REFERENCES]`.
- Reset counters each frame, never call `new`, `delete`, `malloc`, or `realloc` in `Render()`.

This keeps the algorithm but removes fragmentation and most allocator latency.

### 2.5 Fix Scene Capacity Before Profiling Background/Spectrum Modes

`JsonDrivenProtogenAnimation` inherits from `Animation<1>` but `Initialize()` tries to add two objects:

```cpp
scene.AddObject(faceObject);
scene.AddObject(background.GetObject());
```

Because the scene capacity is 1, the background object is not added. This means background-driven modes such as `SpectrumAnalyzerFace` do not render as intended, and any profiling involving background material is misleading.

Options:

- Change to `Animation<2>` if background rendering is required.
- Keep `Animation<1>` and remove/defer background work if S3 performance is more important.
- Treat background/SpectrumAnalyzer as ESP32-P4 or high-performance mode only.

### 3. Throttle or Disable M5UnitGLASS2 Pixel Preview

`TasESP32S3KitV1::Display()` mirrors every HUB75 pixel to the small display:

```cpp
display.drawPixel(...); // 2048 times per frame
display.display();      // every frame
```

This is not required for the HUB75 output and is expensive on a microcontroller. In production firmware, update the preview at 2-5 Hz, only draw status icons, or compile it out.

Expected impact: large wall-time reduction in `Display()`, especially if the small display bus is I2C/SPI-bound.

### 4. Optimize Boop Expression Effects

The first boop maps to `Surprised`:

```json
"Surprised": {
  "scene_effect": { "type": "blurV", "enable": true }
}
```

`VerticalBlur` then runs across all 2048 pixels every render frame. Its current implementation:

- Recomputes `blurRange` inside the pixel loop even though it is constant for the pass.
- Uses virtual `GetUpIndex()` and `GetDownIndex()` calls in the inner loop.
- Uses a general neighbor-array path even though HUB75 is a regular 64x32 grid.

Faster options:

- Make boop expressions avoid full-screen blur on ESP32-S3.
- Use a smaller blur range for S3.
- Add a HUB75-specific vertical blur that indexes `y * 64 + x` directly.
- Compute `blurRange` once before the loop.
- Use a separable blur with rolling sums per column.

Correctness issues to fix before relying on effect profiling:

- `HorizontalBlur` and `VerticalBlur` write `B` from accumulated `G` and `G` from accumulated `B`.
- `RadialBlur` reads `pixelColors[validD]` instead of `pixelColors[indexD]`, so it usually samples index 0 or 1.
- All blur effects divide by `blurRange * 2` while also including the center pixel; this over-brightens or darkens depending on valid neighbor count.

### 5. Pre-Resolve Morph and Expression IDs

`Update()` calls `ApplyExpression()` every frame. That does string-based lookup each time:

```cpp
ExpressionConfig *target = FindExpressionConfig(name);
AddMorphFrame(ap.first, ap.second);
  -> GetMorphId(name)
     -> FindMorphIndex(name)
        -> scan 85 morph names
```

For every active expression parameter, this scans morph names every frame. Pre-resolve IDs once when parsing JSON:

```cpp
struct ResolvedAnimParameter {
    uint16_t morphId;
    float value;
};
```

Then frame update becomes:

```cpp
eEA.AddParameterFrame(param.morphId, param.value);
```

Also split expression application into:

- apply-on-change: material, effect, reset flags, blink/voice toggles
- apply-every-frame: morph target frames that must stay active

Expected impact: medium CPU improvement, less heap/String churn, cleaner boop path.

### 6. Fix MaterialAnimator Bounds Before Profiling

`MaterialAnimator<10>` has 10 slots. Default registration fills 10 slots: base material plus 9 replace layers. But the update loops use `<= currentMaterials`:

```cpp
for(uint8_t i = 1; i <= currentMaterials; i++) {
    combineMaterial.SetOpacity(i, materialRatios[i]);
}
```

When `currentMaterials == 10`, index 10 is out of bounds. This can corrupt nearby data and make performance profiling meaningless.

Recommended fixes:

- Change `currentMaterials <= materialCount` to `currentMaterials < materialCount` in `AddMaterial()`.
- Change loops using `<= currentMaterials` to `< currentMaterials`.
- Initialize `dictionary[0]` when adding the base material, or make `AddMaterialFrame()` skip unset entries safely.

This is a correctness issue before it is a speed issue. If memory near `materialRatios`, `dictionary`, or `combineMaterial` is corrupted, frame-rate and freeze measurements will not be trustworthy.

### 7. Convert Object Transform to Matrix/SoA

Current `Object3D::UpdateTransform()` rotates each vertex through quaternion helpers and creates many temporary `Vector3D` objects:

```cpp
modifiedVector = transform.GetRotation().RotateVector(...);
```

For the face mesh:

- 339 vertices transformed every frame.
- 326 triangles projected every frame.

Faster path:

- Build one 3x3 matrix + translation per frame.
- Transform vertices with direct multiply-add.
- Optionally store vertex arrays as SoA (`x[]`, `y[]`, `z[]`) so ESP-DSP can process them in batches.

This is a medium-size refactor, but it removes many temporary C++ objects and repeated quaternion normalization.

### 8. Use Direct Color Pointers in Display()

Current `Display()` calls `camPixels1->GetColor(pixelNum)` repeatedly for R/G/B and for each output target.

Faster:

```cpp
ProtoRGBColor *colors = camPixels1->GetColors();
const ProtoRGBColor &c = colors[pixelNum];
```

This avoids repeated method calls and pointer calculations inside the 2048-pixel loop.

### 9. Prefer `uint16_t` Neighbor Indices

The panel has 2048 pixels, so neighbor indices fit in `uint16_t`. Current `up/down/left/right` use `unsigned int`, which costs 32 KB total. With `uint16_t`, that becomes 16 KB. If a direct grid path is used, these arrays can disappear entirely for HUB75.

### 10. Avoid Per-Pixel Expensive Materials on S3

`RainbowNoise`, `RainbowSpiral`, `GradientMaterial`, and `SpectrumAnalyzer` all can be much more expensive than `SimpleMaterial`:

- `SimplexNoise::GetRGB()` uses simplex noise per shaded pixel.
- `SpiralMaterial::GetRGB()` uses `atan2` and `powf` per shaded pixel.
- `GradientMaterial::GetRGB()` uses `fmodf`, and radial mode uses `sqrtf`.
- `SpectrumAnalyzer::GetRGB()` uses interpolation and optional hue shifting.

Recommendations:

- For ESP32-S3, treat animated procedural materials as optional high-cost modes.
- Precompute material output into a 64x32 palette/texture when the material changes slowly.
- For static/simple expressions, use `SimpleMaterial` or simple gradient only.

`SpectrumAnalyzer` also has bounds issues that should be fixed before enabling background analyzer modes on S3:

- The range checks use impossible `&&` conditions, so out-of-range coordinates are not rejected.
- `if(bins > x && 0 > x)` is impossible for `uint8_t x`.
- `data[x + 1]` and `bounceData[x + 1]` can read past the end when `x` maps to the last bin.

## Recommended Implementation Order

1. Add a release/profile build environment (`-O2` or `-Os`) separate from debug `-Og`.
2. Fix `MaterialAnimator` bounds and timer error handling so profiling is reliable.
3. Add frame timing telemetry: animation update, rasterize, effect, display.
4. Decide whether S3 should render background objects; fix `Animation<1>` vs two `AddObject()` calls accordingly.
5. Throttle `M5UnitGLASS2` preview to prove display-side savings.
6. Disable or optimize boop-triggered `blurV` and compare frame time.
7. Pre-resolve expression morph IDs.
8. Replace QuadTree with fixed triangle rasterizer or static tile-bin rasterizer.
9. Refactor transform/morph buffers toward matrix + SoA if more speed is needed.

## Profiling Counters to Add

```cpp
uint32_t t0 = micros();
animation.UpdateTime(ratio);
uint32_t t1 = micros();
controller.Render(animation.GetScene());
uint32_t t2 = micros();
controller.Display();
uint32_t t3 = micros();

Serial.printf("anim=%lu render=%lu display=%lu free_int=%u largest=%u psram=%u\n",
    t1 - t0,
    t2 - t1,
    t3 - t2,
    heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
    heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
    heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
```

Add separate timers inside `Controller::Render()` if possible:

- triangle projection/build
- spatial structure build
- pixel shade loop
- screen-space effect pass
