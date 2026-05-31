# Gesture Sensor → Freeze: Crash Path Analysis

> **Date**: 2026-05-31 | **Trigger**: Boop/gesture sensor activation → animation freeze + HUB75 panel freeze

## Executive Summary

The gesture sensor is **not the direct cause** of the crash — it is the **trigger** that pushes an already-fragmented internal DRAM heap over the edge. The actual crash mechanism is an unchecked `nullptr` from a failed heap allocation in the rendering QuadTree or microphone timer, caused by cumulative per-frame internal DRAM fragmentation.

Second-pass note: the first boop also maps to `Surprised` in the animation JSON, and `Surprised` enables the `blurV` scene effect. That means a gesture not only changes expression state; it can add a full-screen vertical blur pass to the render loop.

## Complete Crash Sequence (Most Likely)

```
Frame N (normal):
  animation.UpdateTime()
    → MenuUpdate() → Menu::Update()
      → UpdateBoopSensorState() — reads PAJ7620 (I2C), no boop detected
    → MicrophoneFourierIT::Update()
      → esp_timer_create() [~60 bytes allocated]
      → FFT processing
      → StartSampler() → esp_timer_create() [~60 bytes allocated]
    → ResetFace(), eEA.Update(), UpdateFace(), materialAnimator.Update()
  controller.Render()
    → QuadTree tree(bbox) [allocates entities array + Node::Subdivide allocs]
    → ~30-50 alloc/free cycles for a 326-triangle mesh
  controller.Display()
    → HUB75 DMA push (no allocs)

Frame N+1 (BOOP DETECTED):
  animation.UpdateTime()
    → MenuUpdate() → Menu::Update()
      → UpdateBoopSensorState() — BOOP! proximity > threshold
        → boopPulsePending = true, boopCurrentHigh = true
      → [Menu::Update also writes to M5UnitGLASS2 display — SPI/I2C blocking]
    → ResolveBoopExpressionPtr()
      → Menu::ConsumeBoopPulse() → returns true
      → activeBoopExpression = spec->name  ← ⚠️ String copy allocates ~20-40 bytes
    → ApplyExpression("Surprised")
      → FindExpressionConfig() — string compare loop
      → ApplyResetState() — sets morph targets, material pointers
      → ApplySceneEffect() — set effect pointer on scene
      → AddMorphFrame() loop — calls eEA.AddParameterFrame()
    → MicrophoneFourierIT::Update()
      → esp_timer_create() ← ⚠️ MAY FAIL HERE (heap exhausted)
      → OR: succeeds but heap is now at 0 bytes free
  controller.Render()
    → QuadTree tree(bbox)
    → QuadTree::Expand() → realloc(entities, ...) ← ⚠️ FAILS → returns nullptr
    → entities[count] = triangle ← ⚠️ WRITE TO NULL → CRASH
```

## Three Failure Mechanisms (Any Could Apply)

### Mechanism 1: `activeBoopExpression = spec->name` — The Last Allocation
```cpp
// JsonDrivenProtogenAnimation.h line 1431
activeBoopExpression = spec->name;  // String copy → internal DRAM alloc
```
If internal DRAM is nearly exhausted from cumulative fragmentation, this String copy:
- **Succeeds** but consumes the last free block → next `esp_timer_create` or QuadTree alloc fails
- **Fails** → `activeBoopExpression` becomes corrupted → crash on next use

### Mechanism 2: `esp_timer_create` Failure — Unchecked Return
```cpp
// MicrophoneFourier_MAX9814.h line 90-91
esp_timer_create(&timerParameters, &timer);  // ⚠️ RETURN VALUE IGNORED
esp_timer_start_periodic(timer, 1000000 / sampleRate);  // CRASH if timer==NULL
```
Called EVERY frame. When DRAM is exhausted:
- `esp_timer_create` returns `ESP_ERR_NO_MEM`
- `timer` handle is invalid (NULL or garbage)
- `esp_timer_start_periodic(NULL, ...)` → **instant crash** (likely the freeze symptom)

### Mechanism 3: QuadTree `realloc` Failure — Silent nullptr
```cpp
// QuadTree.h line 28
entities = (Triangle2D*)realloc(entities, newCapacity * sizeof(Triangle2D));
// If realloc returns NULL, `entities` becomes NULL
// Next frame: entities[count] = triangle → CRASH
```
This is the per-frame fragmentation accumulator. If it fails after the boop allocation pushed the heap over the limit, the next triangle insertion crashes.

## Why "After Gesture" Specifically?

| Factor | Why It Matters |
|---|---|
| **Timing coincidence** | Heap fragments gradually. By the time the user triggers a boop, minutes/hours of fragmentation have accumulated |
| **Extra String alloc** | `activeBoopExpression = spec->name` allocates ~20-40 bytes MORE than normal frames |
| **Expression change** | ApplyExpression runs more code paths (FindExpressionConfig loops, AddMorphFrame calls) — all use stack but no extra heap |
| **Vertical blur enabled** | `Surprised` enables `blurV`, adding a 2048-pixel screen-space pass during the boop hold window |
| **Menu::Update display write** | Writes to M5UnitGLASS2 (SPI) with `display.display()` — blocks for ~1-5ms, delaying the loop |

## Additional Stability Finding: MaterialAnimator Bounds

`MaterialAnimator<10>` is filled to 10 slots by default registration: base plus 9 replace layers. Several loops use `<= currentMaterials`, which can access index 10 of arrays sized 10 when `currentMaterials == 10`. This can corrupt nearby state and make a boop-triggered effect look like a sensor problem.

Recommended fix:

```cpp
// Use < currentMaterials, not <= currentMaterials
for (uint8_t i = 1; i < currentMaterials; i++) {
  combineMaterial.SetOpacity(i, materialRatios[i]);
}
```

## Microphone Timer — Memory Leak Risk ⚠️

```cpp
// MicrophoneFourier_MAX9814.h
static void StartSampler(){
    esp_timer_create(&timerParameters, &timer);  // allocate handle
    esp_timer_start_periodic(timer, 1000000 / sampleRate);
}

static void SamplerCallback(){
    // ... collects 256 samples ...
    if(samplesStorage >= FFTSize){
        esp_timer_stop(timer);
        esp_timer_delete(timer);  // free handle
        samplesReady = true;
    }
}

static void Update(){
    // ... processes FFT ...
    StartSampler();  // ⚠️ creates new timer EVERY Update() call
}
```

**Per-frame cycle**: `esp_timer_create` (~60B alloc) → timer fires → `esp_timer_delete` (~60B free) → next frame: repeat.

This is a **30-60 alloc/free cycles per second** on top of the QuadTree alloc/free. Both contribute to fragmentation. If `esp_timer_create` fails (unchecked return), `esp_timer_start_periodic(NULL)` crashes immediately.

## Heap Fragmentation Timeline (Estimated)

| Time | Internal DRAM State | Risk |
|---|---|---|
| Boot | ~200 KB free, clean | None |
| 1 min | ~150 KB free, minor fragmentation | Low |
| 5 min | ~100 KB free, moderate fragmentation | Medium |
| 15 min | ~50 KB free, significant fragmentation | High |
| 30+ min | ~20-30 KB free, largest block ~2-8 KB | **Critical** — any alloc > largest block → crash |

The boop String alloc (~30 bytes) succeeds even at "Critical" because it's small. But the QuadTree expand needs a contiguous block of `newCapacity * 8` bytes. When `capacity` goes from 16→32→48→72..., each expansion needs a larger contiguous block. If the largest free block is only 2 KB, a 72×8=576 byte expansion might still succeed, but a 108×8=864 byte expansion could fail.

## Immediate Verification Steps

### 1. Add Crash Telemetry (before any fix)
Add to `loop()` just before `controller.Render()`:
```cpp
size_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
if (largestBlock < 4096) {
    Serial.printf("⚠️ CRITICAL: free=%u largest_block=%u\n", freeInternal, largestBlock);
}
```

### 2. Guard the Microphone Timer
```cpp
static void StartSampler(){
    samplesReady = false;
    samples = 0;
    samplesStorage = 0;
    
    esp_err_t err = esp_timer_create(&timerParameters, &timer);
    if (err != ESP_OK) {
        Serial.printf("❌ esp_timer_create failed: %d\n", err);
        samplesReady = true;  // skip this cycle gracefully
        return;
    }
    esp_timer_start_periodic(timer, 1000000 / sampleRate);
}
```

### 3. Reproduce in Lab
- Run with `PRINTINFO` enabled to see frame times
- Add `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)` logging
- Trigger the gesture sensor repeatedly and watch the free block size
- Note if the crash happens on the FIRST boop or after N boops
