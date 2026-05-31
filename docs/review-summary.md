# ProtoTracer ESP32-S3 — Systematic Review Summary

> **Date**: 2026-05-31 | **Review Scope**: Rendering, Networking, BLE, Memory Architecture

## Top Findings by Severity

## Second-Pass Corrections

- Current S3 module is N8R8 with 8 MB PSRAM.
- Current face model is 339 vertices, 326 triangles, and 85 morph targets.
- Current `P3HUB75` path does not allocate `PixelGroup::rectCoords`; that earlier 16 KB estimate only applies to rectangular PixelGroups.
- First boop maps to the `Surprised` expression in `example_animation.json`, and that expression enables `blurV`, so gesture activation also enables a full vertical blur pass.
- `MaterialAnimator<10>` is filled to 10 material slots by default registration, while several loops access `<= currentMaterials`. This can access index 10 of arrays sized 10.
- `JsonDrivenProtogenAnimation` is `Animation<1>` but tries to add both face and background objects; the second object is dropped, so background-based modes are not profiled/rendered as intended.
- Current build config is debug-oriented (`build_type = debug`, `-g -Og`), so render-speed measurements should also be taken in a release/profile environment.

### 🔴 CRITICAL — Likely Root Cause of Freezing

| # | Finding | File(s) | Impact |
|---|---|---|---|
| 1 | **Per-frame QuadTree alloc/free storm** causing heap fragmentation | `Camera.h:248-272`, `QuadTree.h`, `Node.h` | After minutes/hours, internal DRAM fragments → `new` returns nullptr → crash |
| 2 | **`heap_caps_malloc_extmem_enable(0)` forces all incidental allocations to internal DRAM** | `main.cpp:251` | STL containers, `String`, `DynamicJsonDocument`, BLE buffers all compete for 512KB internal DRAM |
| 3 | **No null-pointer checks after heap allocations** in render hot path | `Camera.h`, `PixelGroup.h`, `QuadTree.h` | Silent nullptr dereference → hard fault / freeze |
| 4 | **MaterialAnimator bounds risk when all 10 slots are used** | `MaterialAnimator.h`, `JsonDrivenProtogenAnimation.h` | Index 10 access on arrays sized 10 can corrupt nearby state |

### 🟠 HIGH — Contributing Factors

| # | Finding | File(s) | Impact |
|---|---|---|---|
| 5 | **BLE notify blocks main loop with `delay(8)`** | `ESPMenu.h:337-354` | ~150ms render stall during manifest transfer; watchdog not fed |
| 6 | **65KB DynamicJsonDocument allocated from internal DRAM during BLE manifest build** | `ESPMenu.h:222` | Can fail if DRAM is fragmented; failure is silent |
| 7 | **`PixelGroup::up/down/left/right` (32KB) forced to internal DRAM** via `new[]` | `PixelGroup.h:46-49` | Read-only after init; should move to PSRAM or `uint16_t` |
| 8 | **Boop-triggered `blurV` effect adds a full screen-space pass** | `example_animation.json`, `VerticalBlur.h` | Gesture can sharply increase frame time exactly when booped |
| 9 | **Screen-space effects have correctness bugs** | `HorizontalBlur.h`, `VerticalBlur.h`, `RadialBlur.h` | Color channels swap, radial blur samples bool index, blur divisor is wrong |
| 10 | **`SpectrumAnalyzer` bounds checks are ineffective** | `SpectrumAnalyzer.h` | Potential `data[x + 1]`/`bounceData[x + 1]` read past bin array |
| 11 | **`bleRxJsonBuffer` String accumulation in BLE callback** | `ESPMenu.h:793-805` | Heap allocation inside BLE ISR context; no size limit |
| 12 | **`FreeMem()` is misleading** — measures stack-heap gap, not free internal DRAM or fragmentation | `main.cpp:232-248` | False sense of memory health |

### 🟡 MEDIUM — Design Improvements

| # | Finding | File(s) | Impact |
|---|---|---|---|
| 9 | Only ONE function (`Triangle3D::DidIntersect`) uses `IRAM_ATTR` | `Triangle3D.h:47` | Critical hot paths (QuadTree intersect, barycentric test) run from flash |
| 10 | `Camera::cachedRays` (16KB) in internal DRAM unnecessarily | `Camera.h:49` | Used after DSP, could be PSRAM |
| 11 | `Scene::objectsShadow` adds complexity for negligible gain | `Scene.h:13-15` | Extra allocation + sync logic |
| 12 | BLE callback modifies `bleRxJsonBuffer` without mutex | `ESPMenu.h:689-805` | Race condition with `Menu::Update()` |

## Remediation Priority

For the full execution plan, see `optimization-plan.md`. It expands the findings below into ordered workstreams, implementation details, acceptance criteria, and small-corner cleanup items.

### Phase 1 — Immediate (stop the freezing)
1. **Persist or pool the QuadTree** across frames instead of creating/destroying each frame
2. **Add telemetry**: `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` + `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)` every frame
3. **Add null-pointer guards** after every `new`/`heap_caps_malloc` call in render path
4. **Fix `MaterialAnimator` loop bounds** so the default 10-slot material setup cannot corrupt memory
5. **Cap `bleRxJsonBuffer`** to prevent unbounded growth

### Phase 2 — Short-term (reduce DRAM pressure)
5. **Move `PixelGroup::up/down/left/right` to PSRAM** (read-only after init)
6. **Move `Camera::cachedRays` to PSRAM** (not used during DSP acceleration)
7. **Add `IRAM_ATTR` to hot render functions**: `Triangle2D::DidIntersect`, `Node::Intersect`
8. **Add mutex protection** for `bleRxJsonBuffer` and `bleDeviceConnected`

### Phase 3 — Medium-term (architectural)
9. **Consider a frame allocator/arena** for per-frame QuadTree allocations (single block alloc/free per frame instead of dozens)
10. **Move BLE JSON manifest building to PSRAM** (use `AnimJsonPsramAllocator` pattern)
11. **Replace `delay(8)` in BLE notify with non-blocking chunking** (state machine)
12. **Add FreeRTOS task for BLE payload flushing** to decouple from render loop

## Render Acceleration Priority

See `render-acceleration-review.md` for the detailed speed roadmap. Highest-impact acceleration paths:

1. Replace QuadTree with fixed triangle rasterization or fixed tile bins for 64x32.
2. Throttle or disable M5UnitGLASS2 pixel preview in production.
3. Optimize or disable boop-triggered `blurV` on ESP32-S3.
4. Pre-resolve expression/morph IDs during JSON load instead of scanning strings every frame.
5. Verify hot data/code placement (`P3HUB75`, triangle tests, raster inner loop) before enabling PSRAM rodata/instruction placement for production.
6. Convert per-vertex transform from quaternion helper calls to a per-frame matrix and optional SoA/DSP batches.

## Architecture Diagram

```
┌────────────────────── INTERNAL DRAM (512 KB) ──────────────────────────┐
│ Stack │ Heap (fragmented) │ .data/.bss │ WiFi/BLE │ DMA buffers        │
│       │ ← QuadTree        │             │ stack    │ (HUB75 I2S)        │
│       │ ← PixelGroup      │             │          │                    │
│       │ ← Camera SIMD     │             │          │                    │
│       │ ← String/STL      │             │          │                    │
│       │ ← JSON docs       │             │          │                    │
└────────────────────────────────────────────────────────────────────────┘

┌────────────────────── PSRAM (8 MB) ────────────────────────────────────┐
│ Scene::objects │ Face mesh data │ JSON file cache │ .bss (via flags)   │
│ (MOSTLY IDLE — extmem_enable(0) prevents automatic use)               │
└────────────────────────────────────────────────────────────────────────┘
```

The core problem: internal DRAM is over-subscribed while PSRAM is under-utilized.
The `extmem_enable(0)` knob is too coarse — it should be `extmem_enable(threshold)`
or individual allocations should be explicitly placed.
