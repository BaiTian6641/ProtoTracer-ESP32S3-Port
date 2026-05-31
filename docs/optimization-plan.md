# ProtoTracer ESP32-S3 Optimization Plan

> Date: 2026-05-31 | Target board: ESP32-S3 N8R8 | Panel path: HUB75 64x32 x2 virtual halves | Face model: 339 vertices, 326 triangles, 85 morph targets

## 0. Purpose

This document turns the review findings into an execution plan. It covers stability, memory pressure, render speed, rasterization, display output, JSON animation, gesture/audio paths, networking, BLE, build configuration, and verification.

The plan is intentionally conservative about PSRAM. The HUB75 DMA driver and the hot render path need fast internal memory, so the goal is not to push everything into PSRAM. The goal is to make memory placement explicit: hot data stays internal, cold or large data moves to PSRAM, and no render-frame code performs heap allocation.

## 1. Optimization Principles

1. Correctness before speed.
   - Fix memory corruption and out-of-bounds risks before profiling.
   - A corrupted heap or material stack makes every FPS number unreliable.

2. No heap allocation in the render frame.
   - `loop()`, `animation.UpdateTime()`, `controller.Render()`, `Camera::Rasterize()`, screen-space effects, and `controller.Display()` should not call `new`, `delete`, `malloc`, `free`, `realloc`, `String` growth, or `std::vector` growth during normal animation.

3. Preserve internal RAM for truly hot paths.
   - Keep HUB75 DMA buffers, camera DSP scratch arrays, active color buffers, and tight raster inner-loop data in internal memory.
   - Move cold, read-only, or rarely touched data to PSRAM.

4. Measure every phase separately.
   - Do not optimize based on whole-frame timing alone.
   - Measure animation update, render build/projection, raster/shade, effect pass, HUB75 output, M5 preview, BLE/HTTP stalls, and heap state separately.

5. Prefer fixed-size structures on ESP32-S3.
   - The active panel is only 2048 camera pixels and the current face mesh is 326 triangles.
   - Static arrays, arenas, fixed bins, and pre-resolved IDs are a good fit.

6. Keep debug and performance builds separate.
   - `-Og` is for debugging. FPS and stall measurements need `-O2` or `-Os`.

## 2. Success Criteria

### 2.1 Stability Criteria

- Device runs for at least 8 hours with animation, gesture sensor, microphone, WiFi, BLE advertising, and periodic BLE connection attempts enabled.
- Repeated boop stress test does not freeze the HUB75 panel.
- BLE manifest transfer does not pause rendering for visible multi-frame stalls.
- Internal heap largest free block does not trend downward frame after frame during normal animation.
- All allocation failures in setup and runtime produce logs and graceful fallback, not null dereference.
- No watchdog resets, hard faults, or silent stalls during the test matrix.

### 2.2 Performance Criteria

Define actual target FPS after baseline measurement. Suggested initial targets:

- Render plus display under 33 ms for 30 FPS in normal face mode.
- No single main-loop iteration above 80 ms except OTA or explicitly blocking setup mode.
- BLE notification path never blocks the main loop for more than 10 ms at a time.
- M5 preview, if enabled, consumes less than 5 percent of frame time averaged over 5 seconds.

### 2.3 Memory Criteria

- No heap allocation in `Camera::Rasterize()` after setup.
- No repeated `esp_timer_create`/`esp_timer_delete` during every animation frame.
- `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)` remains above a configured safety threshold during long-run tests. Start with 32 KB as a warning threshold and tune after real measurements.
- PSRAM is used for large/cold JSON, metadata, cached manifests, immutable face data, and optional read-only neighbor tables.

## 3. Phase 0 - Baseline and Instrumentation

Do this before optimization. It prevents guessing and makes later changes comparable.

### 3.1 Create Build Profiles

Keep at least three PlatformIO environments:

1. Debug/crash environment.
   - `build_type = debug`
   - `-g -Og`
   - heap poisoning and verbose diagnostics allowed

2. Profile environment.
   - `build_type = release`
   - `-O2` or `-Os`
   - frame timing enabled
   - heap telemetry enabled
   - no heavy heap poisoning

3. Production environment.
   - `build_type = release`
   - timing logs off by default
   - asserts/logging limited to fatal or sampled events

Important: do not use `-Og` numbers as the final render-speed baseline.

### 3.2 Add Frame Timing

Add sampled timing in `loop()` and inside `Controller::Render()` or `Camera::Rasterize()`.

Minimum counters:

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

Render sub-counters:

- object transform/morph update
- camera ray preparation
- triangle projection
- spatial structure build
- pixel raster/shade
- effect pass
- display copy to HUB75
- M5 preview drawing

Use sampled logging, for example once every 60 frames, so serial output does not become the bottleneck.

### 3.3 Add Heap and Stack Telemetry

Replace or supplement `FreeMem()` with real heap metrics:

```cpp
heap_caps_get_free_size(MALLOC_CAP_INTERNAL)
heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)
heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)
uxTaskGetStackHighWaterMark(nullptr)
```

Add warning logs when:

- internal largest block drops below 32 KB
- internal largest block drops below 16 KB
- PSRAM allocation fails
- any render-path allocation would have failed

### 3.4 Capture Map File and Placement

Review the build map file for:

- HUB75 DMA buffers
- `P3HUB75` coordinate table
- camera scratch arrays
- face mesh data
- JSON buffers
- large static material data
- functions placed in IRAM
- rodata placed in PSRAM because of `CONFIG_SPIRAM_RODATA`

Special check: `P3HUB75` is about 16 KB of coordinate data. If it lands in slow PSRAM rodata and is read every frame, either keep it in flash/internal cached memory or copy compact `baseX[]`/`baseY[]` arrays into internal RAM at setup.

### 3.5 Define Repro Scenarios

Run these before and after each phase:

- normal face animation for 15 minutes
- long-run face animation for 8 hours
- boop once, then boop repeatedly for 5 minutes
- BLE connect/disconnect loop
- BLE manifest request during animation
- WiFi connected with stable network
- WiFi disconnected or slow network during startup
- OTA page loaded but no upload
- screen effect modes enabled one by one
- SpectrumAnalyzer/background mode after scene-capacity decision

## 4. Phase 1 - Correctness and Crash-Safety Fixes

These fixes should happen before serious speed work.

### 4.1 Fix `MaterialAnimator<10>` Bounds

Problem:

- Default registration fills all 10 slots.
- `AddMaterial()` allows `currentMaterials <= materialCount`.
- `AddMaterialFrame()` loops `i <= currentMaterials`.
- `Update()` loops `i <= currentMaterials`.
- When `currentMaterials == 10`, index 10 is out of bounds for arrays sized 10.

Plan:

- Treat `currentMaterials` as a count, not a last index.
- Use `currentMaterials < materialCount` when adding new material slots.
- Use `i < currentMaterials` in all loops.
- Ensure base material slot 0 has a defined dictionary entry or is intentionally skipped.
- If a material reference is unknown, log once and ignore it instead of writing past arrays.

Acceptance:

- Default 10-slot registration never accesses index 10.
- Boop expressions and material changes do not corrupt memory.
- Add a small compile-time or runtime assert for `currentMaterials <= materialCount`.

### 4.2 Add Allocation Guards Everywhere They Matter

Every allocation in these classes must be checked:

- `Camera`
- `PixelGroup`
- `QuadTree`
- `Node`
- `Scene`
- `JsonNukudeFace`
- BLE JSON/manifest builders
- microphone timer creation
- network file buffers

Rules:

- Never assign `realloc` directly back to the original pointer.
- Check `new` results if using nothrow, or switch to explicit `heap_caps_malloc` where failure is expected and recoverable.
- For setup-only fatal allocations, fail clearly and show an on-device error.
- For runtime allocations, skip the frame or feature gracefully.

Example pattern for `realloc`:

```cpp
Triangle2D *newEntities = (Triangle2D *)realloc(entities, newCapacity * sizeof(Triangle2D));
if (!newEntities) {
    allocationFailed = true;
    return;
}
entities = newEntities;
```

Acceptance:

- A forced allocation failure creates a log and fallback behavior.
- No path writes through a null pointer after a failed allocation.

### 4.3 Fix Microphone Timer Lifecycle

Problem:

- `MicrophoneFourierIT::Update()` can create/delete an `esp_timer` every frame.
- `esp_timer_create` return value is ignored.
- `esp_timer_start_periodic` may be called with an invalid timer handle.
- The early-return logic can process when samples are not ready.

Plan:

- Create the timer once during initialization, not every frame.
- Start/stop the existing timer instead of create/delete loops.
- Check every `esp_timer_*` return value.
- Make timer state explicit: `Idle`, `Sampling`, `Ready`, `Error`.
- If timer creation fails, disable audio-reactive features and keep animation alive.
- Ensure `Update()` never processes FFT data unless `samplesReady == true`.

Acceptance:

- No per-frame timer allocation.
- Audio failure cannot freeze the render loop.
- Heap largest free block no longer drops due to timer churn.

### 4.4 Fix Screen-Space Effect Correctness

Problems:

- `HorizontalBlur` and `VerticalBlur` write `B` from accumulated `G` and `G` from accumulated `B`.
- `RadialBlur` reads `pixelColors[validD]` instead of `pixelColors[indexD]`.
- Blur divisor uses `blurRange * 2` while including the center pixel and not counting invalid neighbors.
- `blurRange` is recomputed inside every pixel loop.

Plan:

- Correct channel assignments.
- Correct radial index use.
- Count actual samples and divide by sample count.
- Compute blur range once before the pixel loop.
- Clamp blur range to at least 1 if the effect expects a nonzero radius.
- For HUB75, prefer direct `x`, `y` indexing over virtual neighbor calls.

Acceptance:

- Blur preserves color channel order.
- Edge pixels do not darken incorrectly from invalid neighbor counts.
- Effect frame time is measured separately.

### 4.5 Fix `SpectrumAnalyzer` Bounds

Problems:

- Range checks use impossible `&&` conditions.
- `if(bins > x && 0 > x)` is impossible for `uint8_t x`.
- `data[x + 1]` and `bounceData[x + 1]` can read past the end.
- Hue shifting per material sample is expensive.

Plan:

- Change coordinate rejection to use `||`.
- Map and clamp `x` to `0..bins - 2` before reading `x + 1`.
- Use `int` or `uint16_t` for intermediate bin math, not `uint8_t` until after clamping.
- Precompute hue-shifted gradient colors once per frame if hue shift is active.

Acceptance:

- Analyzer mode cannot read out of bounds.
- Analyzer cost is visible in material/effect timing.

### 4.6 Decide Scene Capacity and Background Policy

Problem:

- `JsonDrivenProtogenAnimation` inherits from `Animation<1>`.
- `Initialize()` adds both face and background objects.
- The background object is dropped because scene capacity is 1.

Plan:

Choose one policy:

1. S3 face-only policy.
   - Keep `Animation<1>`.
   - Remove background add from S3 path.
   - Disable or hide background/SpectrumAnalyzer expressions on S3.

2. S3 face-plus-background policy.
   - Change to `Animation<2>`.
   - Re-measure render cost with background enabled.
   - Make background materials cheap by default.

3. Hybrid policy.
   - `Animation<2>` but background object disabled unless explicitly selected.
   - Analyzer/background modes marked high-cost.

Acceptance:

- Scene object count matches intended visuals.
- Background modes are either supported and profiled or explicitly disabled.

### 4.7 Replace Misleading `FreeMem()` Usage

Problem:

- `FreeMem()` measures stack/heap address gap and does not report fragmentation.

Plan:

- Keep it only as a legacy debug helper if needed.
- Add a real memory telemetry function using heap capabilities APIs.
- Log internal free, internal largest block, PSRAM free, PSRAM largest block, and stack high-water mark.

Acceptance:

- Review and debug logs no longer rely on stack/heap gap as health signal.

### 4.8 Fix PixelGroup Boundary Corners

Plan:

- Audit all `PixelGroup` count/index checks.
- Use `count >= pixelCount`, not only `count > pixelCount`, for invalid access.
- Ensure `GetColor`, `GetCoordinate`, neighbor accessors, and alternate index helpers handle invalid counts consistently.
- Return a safe default or false on invalid input.

Acceptance:

- No count equal to `pixelCount` can read past the end.

## 5. Phase 2 - Memory Architecture

### 5.1 Memory Placement Policy

Use explicit allocation categories.

| Data | Target | Reason |
|---|---|---|
| HUB75 DMA buffers | Internal DMA-capable | Required by DMA and timing |
| `pixelColors` | Internal | Written by render, read by display every frame |
| `pixelBuffer` | Internal if effects enabled, PSRAM if effects rare | Effect scratch; hot only during effect pass |
| Camera DSP arrays `tmpX/tmpY/rotX/rotY` | Internal, aligned | ESP-DSP hot path |
| `cachedRays` | Remove, recompute compactly, or PSRAM | Not used by DSP after ray build |
| P3HUB75 coordinate table | Flash/internal cached, or compact internal copy | Read every frame |
| Neighbor arrays `up/down/left/right` | Remove for regular grid, or `uint16_t`, or PSRAM | Read-only after init, 32 KB today |
| QuadTree/Node data | Fixed internal arena if QuadTree remains | Hot during render, no heap churn |
| Direct raster z-buffer | Internal | 2048 pixels, hot inner loop |
| Immutable face mesh/morph source data | PSRAM | Large and mostly read-only |
| Active transformed vertices | Internal if affordable | Hot per-frame transform/projection |
| JSON documents | PSRAM allocator | Large, not render-hot |
| BLE manifest cache | PSRAM | Large and infrequent |
| Small BLE state flags | Internal with critical sections | Shared task state |
| Strings/vectors loaded from JSON | Reserve once or PSRAM-backed where possible | Avoid internal fragmentation |

### 5.2 Do Not Blindly Enable PSRAM Fallback

`heap_caps_malloc_extmem_enable(0)` protects hot paths by preventing automatic PSRAM fallback. Changing it to a low threshold may hide crashes while slowing rendering unpredictably.

Safer strategy:

- Keep hot allocations explicit with `MALLOC_CAP_INTERNAL`.
- Move known cold allocations explicitly to `MALLOC_CAP_SPIRAM`.
- If using `heap_caps_malloc_extmem_enable(threshold)`, choose a threshold high enough that small hot allocations still stay internal, and test frame time carefully.
- Confirm third-party libraries do not accidentally move DMA or timing-sensitive buffers into PSRAM.

### 5.3 Remove Per-Frame Heap Allocation

Add a render-frame rule:

- setup may allocate
- JSON load may allocate
- network handlers may allocate in controlled tasks
- render frame must not allocate

Implementation options:

- Add debug wrappers or counters around `malloc`/`free` during `Render()`.
- Use a `renderFrameActive` flag in debug builds and assert if allocation happens inside it.
- Reserve all `std::vector` containers after JSON parse.
- Reserve Arduino `String` buffers for known maximum protocol sizes.

### 5.4 Reduce PixelGroup Memory

Current active P3HUB75 path does not allocate `rectCoords`, but it still allocates four 2048-entry neighbor arrays as `unsigned int`.

Plan options:

1. Convert neighbor arrays to `uint16_t`.
   - 32 KB becomes 16 KB.
   - Low-risk memory win.

2. Move neighbor arrays to PSRAM.
   - Saves internal RAM.
   - Small speed cost only when effects run.

3. Remove neighbor arrays for regular grid effects.
   - Use direct `x`, `y` math for 64x32.
   - Best long-term solution.

Acceptance:

- Internal memory drops by at least 16 KB or 32 KB without breaking effects.

### 5.5 Move Large JSON and Manifest Work to PSRAM

Plan:

- Use a PSRAM allocator for `DynamicJsonDocument` paths that parse face/animation/manifest JSON.
- Avoid `String` copies of entire files when streaming parse or bounded read is possible.
- Keep metadata caches in PSRAM.
- Log JSON allocation failure explicitly.

Acceptance:

- BLE manifest generation no longer needs a 65 KB internal allocation.
- Face/animation JSON loading cannot silently starve internal DRAM.

### 5.6 Static Allocation Budget

Maintain a memory budget table in docs after implementation. Include:

- internal `.bss` and `.data`
- internal heap at setup complete
- largest internal block at setup complete
- largest internal block after 15 minutes
- PSRAM free after setup
- DMA buffer size
- render scratch size
- optional feature buffers

This makes regressions visible.

## 6. Phase 3 - Quick Performance Wins

These should be done before the larger raster rewrite.

### 6.1 Throttle or Disable M5UnitGLASS2 Preview

Problem:

- `TasESP32S3KitV1::Display()` mirrors 2048 pixels to the small display every frame.
- It also draws a rectangle and calls `display.display()` every frame.

Plan:

- Add a compile-time flag: `ENABLE_M5_PIXEL_PREVIEW`.
- Default it off for production.
- If enabled, throttle to 2-5 Hz.
- Draw a tiny status preview or downsampled view instead of all 2048 pixels.
- Avoid `display.drawRect` and full `display.display()` every render frame.

Acceptance:

- Display phase timing drops measurably.
- HUB75 output remains unchanged.

### 6.2 Use Direct Color Pointer in Display

Problem:

- `Display()` repeatedly calls `camPixels1->GetColor(pixelNum)` for R/G/B and for multiple outputs.

Plan:

- Fetch `ProtoRGBColor *colors = camPixels1->GetColors()` once.
- Load one `const ProtoRGBColor &c = colors[pixelNum]` per pixel.
- Cache brightness and call `setBrightness8` only when it changes.
- Keep color buffer internal for display speed.

Acceptance:

- Display loop has fewer virtual/function calls and pointer lookups.

### 6.3 Disable or Reduce Boop Blur on S3

Problem:

- First boop maps to `Surprised`, which enables `blurV`.
- This adds a full vertical blur pass during the boop hold window.

Plan options:

1. S3 config disables `scene_effect` for `Surprised`.
2. Use a small blur radius on S3.
3. Use a cheaper flash or color change instead of blur.
4. Use optimized fixed-grid blur only when effects are enabled.

Acceptance:

- Boop response remains visible.
- Boop no longer causes large frame-time spikes.

### 6.4 Selective IRAM Placement

Candidates:

- `Triangle2D::DidIntersect`
- `Node::Intersect` if QuadTree remains
- direct raster inner loop after rewrite
- optimized fixed-grid blur loops

Rules:

- Add IRAM only after profiling shows a hot function.
- Re-check map file after each IRAM change.
- Avoid bloating IRAM and reducing internal memory available for data.

Acceptance:

- Function placement improves measured frame time without reducing memory safety margin.

### 6.5 Reduce Serial and Debug Overhead

Plan:

- Disable verbose serial logs in performance builds.
- Sample telemetry instead of printing every frame.
- Avoid `String` construction for logs in hot paths.
- Use `Serial.printf` with literals and primitive values.

Acceptance:

- Profiling numbers are not dominated by serial output.

## 7. Phase 4 - Rasterization Architecture

The current QuadTree is flexible but expensive and allocation-heavy. The panel size and mesh size are small enough for fixed raster structures.

### 7.1 Option A: Static/Arena QuadTree

Best if you need a quick stability improvement with minimal algorithm change.

Plan:

- Preallocate projected triangle array for the maximum expected triangle count.
- Preallocate node pool.
- Preallocate node entity reference pool.
- Reset pool counters each frame.
- Replace `new`, `delete`, `malloc`, and `realloc` in QuadTree/Node with arena allocation.
- Add overflow behavior: stop subdividing or fall back to a larger leaf, never crash.

Pros:

- Lowest behavior risk.
- Removes heap fragmentation.
- Keeps current spatial partition logic.

Cons:

- Still does tree traversal per pixel.
- Still has subdivision overhead.
- Still more complex than necessary for 64x32.

Acceptance:

- Zero render-frame heap allocation.
- Same image output as current QuadTree within minor floating-point differences.

### 7.2 Option B: Fixed Tile Bins

Good middle ground.

Plan:

- Divide 64x32 camera pixels into fixed tiles, for example 8x8 pixels.
- That gives 8 columns x 4 rows = 32 bins.
- For each projected triangle, compute pixel-space bbox and append triangle index to overlapped bins.
- For each pixel, only test triangles in its bin.
- Store bin triangle lists in fixed arrays.

Pros:

- No dynamic tree.
- Predictable memory.
- Less triangle testing than full brute force.
- Easier than full scanline raster rewrite.

Cons:

- Bin overflow must be handled.
- Still tests triangles per pixel instead of triangle-driven rasterization.

Overflow policy:

- If a bin fills, set an overflow flag and either use a slower fallback for that bin or clamp extra triangles with a visible debug warning.
- Size bins from measured worst-case triangle overlap.

Acceptance:

- No heap allocation.
- Worst-case bin overflow is measured and handled.
- Faster than current QuadTree in profile build.

### 7.3 Option C: Direct Fixed Triangle Rasterizer

Best long-term path for speed and stability.

Plan:

1. Transform object vertices once per frame.
2. Project each triangle into screen/pixel coordinate space.
3. Reject disabled, degenerate, fully off-screen, or behind-camera triangles.
4. Compute clamped pixel bbox.
5. Use edge functions or barycentric coordinates inside the bbox.
6. Interpolate depth and compare against `zbuffer[2048]`.
7. Interpolate material inputs only for pixels that pass coverage and depth.
8. Write final `ProtoRGBColor` directly to `pixelColors[pixelIndex]`.

Memory:

- `uint16_t` or `float` z-buffer for 2048 pixels.
- projected triangle scratch for 326 triangles.
- optional per-triangle precomputed edge constants.

Why it is likely faster:

- No QuadTree allocation.
- No node traversal.
- No per-pixel leaf lookup.
- Work scales with covered triangle pixels, not all pixels times leaf contents.
- Better depth correctness than average triangle depth.

Important details:

- Define depth convention early: smaller is closer or larger is closer.
- Clip or reject near-plane triangles safely.
- Handle triangles with zero or near-zero area.
- Decide backface culling policy based on current visual behavior.
- Preserve current material UVW behavior or intentionally document changes.
- Match current P3HUB75 coordinate orientation before removing old raster path.
- Keep old QuadTree path behind a compile flag during migration.

Acceptance:

- Same face position/orientation as current renderer.
- No missing triangle cracks in normal expression poses.
- No heap allocation in raster.
- Faster measured `render` time than arena QuadTree or current QuadTree.

### 7.4 Transform and Projection Optimization

Problem:

- `Object3D::UpdateTransform()` rotates each vertex through quaternion helpers and many temporary `Vector3D` objects.

Plan:

- Build one transform matrix per object per frame.
- Apply scale, rotation, and translation using direct multiply-add.
- Store active vertices in arrays that are friendly to batch processing.
- Consider struct-of-arrays for `x[]`, `y[]`, `z[]` if ESP-DSP batching is worth it.
- Keep immutable base vertices and morph deltas in PSRAM if internal memory is tight.
- Keep active transformed vertices internal if projection/raster reads them heavily.

Acceptance:

- Vertex transform phase timing drops.
- Visual transform matches current output.

### 7.5 Material Sampling in the Rasterizer

Problem:

- Material calls are virtual and can use expensive math per shaded pixel.

Plan:

- Keep a generic material path for compatibility.
- Add fast paths for common material types:
  - solid color
  - simple gradient
  - precomputed 64x32 texture/palette
- Skip fully transparent material layers.
- Build an active material layer list each frame so zero-opacity layers are not visited.
- For procedural animated materials, precompute a 64x32 material texture once per frame if cheaper than evaluating inside every triangle sample.

Acceptance:

- Simple expressions do not pay for `SimplexNoise`, `atan2`, `powf`, or unused layers.

## 8. Phase 5 - Animation, Morph, and JSON Runtime

### 8.1 Pre-Resolve Expression Configs

Problem:

- `ApplyExpression()` runs every frame and performs string lookups.
- Morph parameter names are converted to IDs repeatedly.

Plan:

Create runtime expression structs during JSON load:

```cpp
struct ResolvedAnimParameter {
    uint16_t morphId;
    float value;
};

struct ResolvedExpression {
    bool reset;
    bool voiceEnable;
    bool blink;
    bool showMouth;
    bool eyeShape;
    Material *faceMat;
    Material *backgroundMat;
    Effect *sceneEffect;
    bool sceneEffectEnabled;
    std::vector<ResolvedAnimParameter> animParameters;
};
```

Then frame update uses IDs and pointers, not strings.

Acceptance:

- No morph-name scans during steady-state animation.
- Expression lookup is by index or pointer.

### 8.2 Split Apply-On-Change From Apply-Every-Frame

Some expression state only needs to change when the active expression changes:

- selected face material
- selected background material
- selected effect pointer
- blink/voice/show-mouth flags
- reset-state application

Some morph frames may need to be refreshed every frame depending on EasyEase behavior.

Plan:

- Track `activeExpressionId`.
- On expression change, apply persistent state once.
- Every frame, submit only the morph target frames that must stay active.
- For boop hold, store expression ID and timeout, not a copied `String`.

Acceptance:

- Normal frames do not run full expression string logic.
- Boop transition does not allocate.

### 8.3 Resolve Boop, Auto-Link, and Flipped Morph IDs

Plan:

- Convert `boop_morphs` expression names to expression IDs during JSON load.
- Convert `auto_link` morph names to morph IDs during JSON load.
- Convert `flipped_morphs` to morph IDs or bitset during JSON load.
- Keep aliases such as `vrc_v_uh` to `vrc_v_dd` in one resolver.

Acceptance:

- No per-frame string compare loops for boop, auto-link, or flipped morph checks.

### 8.4 Reserve Containers After JSON Load

Plan:

- Reserve `expressions`, `expressionOrder`, `autoLinkSpecs`, `boopMorphSpecs`, `materialRegistry`, `effectRegistry`, and hue-shift registry based on JSON sizes.
- Avoid push-back growth after initialization.
- If runtime updates are required, use bounded capacity and reject oversized config gracefully.

Acceptance:

- No vector reallocation during animation.

### 8.5 Material Stack Optimization

Plan:

- Fix `MaterialAnimator` first.
- Track active material layers where opacity is nonzero.
- If only one material is active at full opacity, return it directly and skip blend math.
- Avoid always-on expensive default layers such as `RainbowNoise` unless explicitly selected.
- Move per-frame `HueShift` work out of per-pixel sampling.
- Precompute material output for procedural effects when possible.

Acceptance:

- Simple expressions sample a cheap material path.
- Expensive procedural materials are opt-in on S3.

## 9. Phase 6 - Effects and Post-Processing

### 9.1 Fixed-Grid Effect Path

The active panel is a regular 64x32 grid. Effects should not need virtual neighbor lookups.

Plan:

- Add S3/HUB75-specific effect helpers:
  - `index = y * 64 + x`
  - clamp `x` and `y`
  - direct horizontal and vertical neighbor reads
- Keep generic `IPixelGroup` effects for unusual layouts.
- Use compile-time dimensions for fast paths.

Acceptance:

- Blur/AA effects no longer require four neighbor arrays for the regular HUB75 path.

### 9.2 Rolling-Sum Blur

Plan:

- Implement horizontal/vertical blur with rolling sums.
- Count actual samples at edges.
- Use integer accumulation.
- Compute radius once per pass.

Acceptance:

- Blur cost becomes O(pixelCount) rather than O(pixelCount * radius).

### 9.3 Effect Policy by Hardware

Plan:

- Define effect quality tiers:
  - S3 normal: no full-screen blur by default
  - S3 high-quality: small-radius optimized blur
  - P4/high-performance: full effect set
- Add JSON compatibility behavior: unsupported effects degrade to a cheaper equivalent instead of failing.

Acceptance:

- User JSON remains portable, but S3 does not accidentally enable heavy effects.

## 10. Phase 7 - Display Output and HUB75

### 10.1 HUB75 Output

Plan:

- Keep HUB75 DMA buffers internal/DMA-capable.
- Keep `pixelColors` internal for display read speed.
- Use direct pointer access to color buffer.
- Investigate whether the HUB75 library allows faster bulk buffer writes than `drawPixelRGB888`.
- If bulk access exists, write directly to the back buffer with the known two-half mirror mapping.
- If not, at least remove repeated color getter calls.

Acceptance:

- `Display()` timing is lower or stable after quick wins.

### 10.2 M5UnitGLASS2 Output

Plan:

- Separate status UI from pixel preview.
- During normal production animation, update only status indicators or nothing.
- During debug, update preview at low rate.
- Avoid blocking display calls inside gesture/menu update unless needed.

Acceptance:

- M5 display cannot be the reason HUB75 appears frozen during normal animation.

### 10.3 Brightness and Minor Corners

Plan:

- Cache brightness and only call `setBrightness8` on change.
- Avoid drawing static preview borders every frame.
- Avoid converting preview colors to binary 0/255 unless that is intentional.
- Keep display operations out of critical timing paths where possible.

## 11. Phase 8 - Networking and BLE

### 11.1 BLE Shared State Synchronization

Problems:

- BLE callbacks and the main loop share flags and strings.
- `bleRxJsonBuffer` is modified in BLE task context.
- Several flags have no barrier or critical section.

Plan:

- Put BLE shared state behind a mutex, critical section, or single-producer/single-consumer queue.
- Avoid `String` mutation in BLE callbacks.
- BLE callback should copy bounded raw bytes into a fixed ring buffer or queue and return quickly.
- Main loop or a BLE worker task parses JSON outside the callback.

Acceptance:

- No unbounded heap allocation in BLE callback context.
- No unsynchronized shared `String` access.

### 11.2 Cap BLE RX JSON

Plan:

- Define maximum RX JSON size.
- Reject messages above the cap with an error response.
- Add timeout for incomplete JSON fragments.
- Clear buffer safely on disconnect.
- Reserve fixed capacity or use static buffer.

Acceptance:

- Malformed BLE input cannot grow memory indefinitely.

### 11.3 Non-Blocking BLE Notifications

Problem:

- `NotifyBleJsonPayload()` sends chunks with `delay(8)` in the main loop.

Plan:

- Convert notification sending to a state machine or worker task.
- Send a small number of chunks per loop iteration.
- Use connection/MTU information to size chunks.
- Yield between chunks if a worker task is used.
- Keep outgoing payload in PSRAM if large.

Acceptance:

- Manifest transfer does not freeze animation for 150 ms.

### 11.4 BLE Manifest Memory

Plan:

- Build manifest with PSRAM-backed JSON document or stream output.
- Cache manifest metadata after startup if memory allows.
- If manifest build fails, send a clear error response.
- Do not silently return empty expression metadata after allocation failure.

Acceptance:

- Manifest request cannot consume a 65 KB internal allocation.

### 11.5 AsyncWebServer and Relay Asset Calls

Problem:

- `RefreshRelayAsset` can perform blocking HTTP/MD5 inside an async request handler.

Plan:

- Serve cached relay assets immediately.
- Refresh remote assets in a background task or scheduled main-loop job.
- Add stale-cache policy.
- If relay is unused in production, compile it out or disable the route.

Acceptance:

- AsyncTCP task is not blocked by slow remote HTTP.

### 11.6 Startup Network Robustness

Plan:

- Keep strict connect/request/idle timeouts.
- Avoid infinite reboot loops when remote face/animation files are unavailable.
- Prefer cached local assets when remote MD5 cannot be fetched.
- Log selected source, latency, and fallback reason.
- Avoid AP+STA mode unless needed; each mode consumes buffers.

Acceptance:

- Bad network does not produce a stuck boot loop.
- Startup network failures degrade to cached assets when safe.

## 12. Phase 9 - Gesture, Sensor, and Audio Path

### 12.1 Boop Must Not Allocate

Plan:

- Store boop target as expression ID or pointer.
- Avoid `activeBoopExpression = spec->name` String copy in the hot path.
- Pre-resolve `boop_morphs` during JSON load.
- Keep boop counters primitive and bounded.

Acceptance:

- A boop event causes no heap allocation.

### 12.2 Gesture Sensor Timing

Plan:

- Add I2C timeout/error handling for PAJ7620 reads.
- Debounce proximity threshold changes.
- Log first failure but do not spam serial.
- If sensor read fails, keep previous expression or fall back to default.

Acceptance:

- Sensor bus issues cannot block or freeze rendering.

### 12.3 Audio Sampling

Plan:

- Use a persistent timer or continuous sampler.
- Avoid allocation in sampler start/stop.
- Protect sample buffer state shared between callback and main loop.
- Handle `ESP_ERR_NO_MEM` and timer errors by disabling audio for that cycle.
- Do not process FFT until a full sample buffer is ready.

Acceptance:

- Audio path does not contribute to heap fragmentation.

## 13. Phase 10 - Build and Compiler Optimization

### 13.1 Profile Build Flags

Plan:

- Add a profile/release S3 environment.
- Test `-O2` and `-Os`; choose based on measured frame time and binary size.
- Consider LTO only after the code is stable.
- Keep exception decoder and debug symbols in debug builds.

Acceptance:

- Every performance claim states the build environment used.

### 13.2 PSRAM Instruction and Rodata Flags

Current flags include PSRAM instruction fetch and rodata placement.

Plan:

- Validate whether hot code or hot read-only data is placed in PSRAM.
- If a hot table or hot code moves to PSRAM and slows the renderer, override placement.
- Keep large cold rodata in PSRAM if it improves internal memory without hurting frame time.

Acceptance:

- PSRAM flags are based on measured placement and timing, not assumptions.

### 13.3 Debug-Only Heap Tools

Plan:

- Use heap poisoning only in debug builds.
- Use heap tracing during targeted lab tests, not production.
- Add optional allocation counters around render frame.
- Capture reset reason and last known phase in RTC memory or persistent debug log if practical.

Acceptance:

- Debug builds catch corruption; production builds keep performance.

## 14. Phase 11 - Testing Matrix

### 14.1 Unit and Compile Tests

- Compile all S3 environments.
- Compile P4 environment if still supported by shared files.
- Add small host-side or firmware-side checks for:
  - `MaterialAnimator` capacity behavior
  - expression name to morph ID resolution
  - SpectrumAnalyzer bin clamp
  - PixelGroup bounds
  - blur sample count at edges

### 14.2 Visual Regression Tests

- Default expression
- Surprised boop expression
- Angry/Sad material changes
- Mouth visemes
- Blink
- Flipped morphs
- Background/analyzer mode if enabled
- Horizontal/vertical/radial blur if enabled

### 14.3 Stress Tests

1. Long-run render test.
   - 8 hours.
   - Log frame time and heap every 60 seconds.

2. Boop stress.
   - Trigger boop repeatedly for 5 minutes.
   - Confirm no allocation and no largest-block drop tied to boop.

3. BLE stress.
   - Connect/disconnect repeatedly.
   - Request manifest repeatedly.
   - Send malformed JSON and oversize JSON.

4. Network stress.
   - Slow remote server.
   - No internet.
   - Bad MD5.
   - Relay route hit repeatedly.

5. Effect stress.
   - Enable each effect individually.
   - Measure effect phase time.
   - Confirm no color-channel swap.

6. Audio stress.
   - Enable microphone processing.
   - Run with silence and loud input.
   - Confirm no timer allocation churn.

### 14.4 Acceptance Log Format

Use a compact log format:

```text
frame=123456 anim_us=1200 render_us=18000 effect_us=0 display_us=5000 int_free=88000 int_largest=52000 psram_free=6200000 mode=Default boop=0 ble=0
```

For failures, include:

- last frame number
- active expression
- active effect
- BLE state
- WiFi state
- internal free/largest block
- PSRAM free/largest block
- reset reason

## 15. Recommended Implementation Order

### Milestone A - Make Measurements Trustworthy

1. Add profile build environment.
2. Add frame timing and real heap telemetry.
3. Replace misleading `FreeMem()` usage in logs.
4. Add map-file placement review.

Deliverable: baseline report with frame timings and heap numbers.

### Milestone B - Remove Known Crash Sources

1. Fix `MaterialAnimator` bounds.
2. Add allocation guards to render and setup allocations.
3. Fix microphone timer lifecycle and return checks.
4. Fix PixelGroup boundary checks.
5. Make boop path allocation-free.

Deliverable: boop no longer correlates with heap drop or freeze.

### Milestone C - Low-Risk Speed Wins

1. Disable/throttle M5 preview.
2. Use direct color pointer in `Display()`.
3. Cache brightness changes.
4. Disable or reduce boop `blurV` on S3.
5. Fix screen effect correctness.

Deliverable: lower display/effect timing with same HUB75 visuals.

### Milestone D - Memory Pressure Reduction

1. Move/cut neighbor arrays.
2. Move large JSON/manifest allocations to PSRAM.
3. Reserve containers after JSON load.
4. Move or remove `cachedRays`.
5. Decide scene capacity/background policy.

Deliverable: larger and stable internal largest free block.

### Milestone E - Render Architecture Upgrade

Choose one:

- static arena QuadTree for fastest stabilization
- fixed tile bins for balanced speed and scope
- direct triangle rasterizer for best long-term performance

Deliverable: no render-frame heap allocations and measured render speed improvement.

### Milestone F - Networking/BLE Non-Blocking Path

1. Synchronize BLE shared state.
2. Cap RX JSON.
3. Make notifications non-blocking.
4. Move manifest JSON work to PSRAM or stream it.
5. Move blocking relay refresh out of async request handler.

Deliverable: BLE/HTTP activity does not visibly freeze the panel.

### Milestone G - Final Tuning

1. Selective IRAM placement.
2. Matrix/SoA vertex transform.
3. Material fast paths and precomputed procedural textures.
4. Release build tuning.

Deliverable: production-ready S3 performance profile.

## 16. Small Corners Checklist

These are easy to miss but should be tracked.

- `MaterialAnimator` loops use `< currentMaterials`, not `<= currentMaterials`.
- `MaterialAnimator::AddMaterial()` uses `< materialCount`, not `<= materialCount`.
- Base material dictionary behavior is defined.
- `QuadTree::Expand()` does not lose the old pointer on failed `realloc`.
- `Node::Expand()` and `Node::Subdivide()` check allocation results.
- `PixelGroup` rejects `count >= pixelCount`.
- `rectCoords` is not counted in S3 P3HUB75 memory budget.
- Neighbor arrays are `uint16_t`, PSRAM, or removed for fixed grid.
- `cachedRays` is removed or moved out of internal RAM if not hot.
- `P3HUB75` coordinate table placement is verified in the map file.
- `display.display()` for M5 preview is not called every frame in production.
- `display.drawRect()` preview border is not redrawn every frame.
- `setBrightness8()` is only called when brightness changes.
- `blurRange` is computed once per effect pass.
- Blur divides by actual sample count.
- Horizontal/vertical blur channels are not swapped.
- Radial blur uses `indexD`, not `validD`, for pixel lookup.
- SpectrumAnalyzer clamps `x` before reading `x + 1`.
- SpectrumAnalyzer coordinate rejection uses `||`, not impossible `&&` checks.
- Boop stores expression ID/pointer, not a copied `String`.
- `activeBoopExpression` or replacement state is bounded and allocation-free.
- `esp_timer_create` return value is checked.
- Microphone timer is persistent or otherwise allocation-free during frames.
- FFT processing only runs when a full sample buffer is ready.
- BLE callback does not mutate `String` or allocate large buffers.
- BLE RX JSON has size cap and timeout.
- BLE TX chunks are sent by state machine or worker task, not one blocking loop.
- Manifest JSON build uses PSRAM or streaming.
- `bleDeviceConnected`, `bleManifestRequested`, and related flags are synchronized.
- Async request handlers do not perform slow remote HTTP refresh inline.
- Startup network failure falls back to cached assets where safe.
- AP+STA mode is enabled only when needed.
- Debug heap poisoning is not enabled in production performance builds.
- Performance logs are sampled, not printed every frame.
- All performance results name the build environment and active effects.

## 17. Things Not To Do

- Do not simply move all rendering data to PSRAM. That may reduce crashes while making frame rate worse.
- Do not benchmark with `-Og` and treat it as production performance.
- Do not add `IRAM_ATTR` to many functions at once. IRAM pressure can create new memory problems.
- Do not keep optimizing QuadTree allocation if the long-term plan is direct triangle rasterization, unless the arena version is needed as a temporary stability bridge.
- Do not hide failed allocations. Log, degrade, or disable the feature.
- Do not let BLE or HTTP work block the render loop for hundreds of milliseconds.

## 18. Suggested First Implementation Batch

If only one batch can be done first, choose this:

1. Add profile build and frame/heap telemetry.
2. Fix `MaterialAnimator` bounds.
3. Make microphone timer allocation-safe.
4. Make boop expression selection allocation-free.
5. Disable/throttle M5 preview.
6. Disable boop `blurV` on S3 or replace it with cheap visual feedback.
7. Add allocation guards to QuadTree/Node until the rasterizer is replaced.

This batch directly addresses the observed freeze trigger while also making later render acceleration measurements trustworthy.
