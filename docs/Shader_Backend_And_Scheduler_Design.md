# ProtoGL: Shader Backend Abstraction & Job Scheduler Design

## Overview

This document covers three related improvements to the ProtoGL firmware architecture:

1. **Shader Compilation Portability** — Explicit compile-anywhere policy (PC / host MCU / never GPU)
2. **Shader Backend Abstraction** — Platform-portable math/texture dispatch for the VM
3. **General Job Scheduler** — Abstract multi-core work dispatch, replacing the hardcoded FIFO split

All three share a common goal: **decouple ProtoGL's algorithms from any single hardware
platform**, enabling the same GPU firmware source to build for RP2350, ESP32-P4, RISC-V
custom SoCs, FPGA soft-processors, or desktop simulation — with each platform providing
optimised back-end implementations.

---

## 1. Shader Compilation Portability

### 1.1 Current State

`PglShaderCompiler.h` already compiles on any C++17 platform:

| Dependency | Portable? |
|---|---|
| `<cstdint>`, `<cstring>`, `<cstdio>`, `<cmath>` | Yes — ISO C++ |
| `PglShaderBytecode.h` | Yes — pure constexpr + structs, `<cstdint>` only |

The compiler has **zero** ESP32, Pico SDK, or ARM dependencies.

### 1.2 Compile Location Policy

**Rule:** PGLSL compilation may happen at **build time** or on the **host MCU**. It must
**never** run on the GPU firmware.

| Location | When | Tooling |
|---|---|---|
| **PC (offline)** | Build time. `.pglsl` → `.psb` → C header array | `tools/pglslc` (C++ or Python, links `PglShaderCompiler.h`) |
| **Host MCU (runtime)** | Boot / effect change. `PglShaderCompiler::Compile()` on ESP32-S3 | Already works — compiler is in `lib/ProtoGL/src/` (host-side) |
| **GPU (RP2350)** | **Never.** GPU only executes pre-compiled bytecode. | No compiler code in `firmwares/rp2350/`. VM consumes `ShaderProgram` structs only. |

### 1.3 Enforcement

The current codebase already enforces this separation:
- `PglShaderCompiler.h` is in `lib/ProtoGL/src/` (host library), not in `firmwares/rp2350/`
- `CMakeLists.txt` (GPU build) does not list `PglShaderCompiler.h` as a source/include
- The GPU VM (`pgl_shader_vm.cpp`) has no `#include` of the compiler

**Documentation update:** Add explicit notes to `Shader_System_Design.md` §7.2 and to the
compiler header's doc-comment, stating the compile-location policy.

### 1.4 Standalone PC Compile Target

To support offline compilation on a development PC, the following files are needed:

```
lib/ProtoGL/src/PglShaderBytecode.h   (shared format)
lib/ProtoGL/src/PglShaderCompiler.h   (compiler)
```

A minimal `tools/pglslc.cpp` can `#include` both headers and provide a command-line
interface. No changes to the compiler source are needed — it is already portable.

---

## 2. Shader Backend Abstraction (`PglShaderBackend`)

### 2.1 Motivation

The shader VM (`pgl_shader_vm.cpp`) currently calls standard C math functions (`sinf`,
`cosf`, `sqrtf`, etc.) directly. While this works on any platform with a C library, it
misses optimisation opportunities:

| Platform | Opportunity |
|---|---|
| **Cortex-M33 FPv5** | `fmaf()` is single-cycle; intrinsics for `__VSQRT`, fast rsqrt |
| **Cortex-M33 DSP** | `__UQADD16`/`__UQSUB16` for saturating RGB565 blends (already used in rasterizer) |
| **RISC-V (Hazard3)** | No FPU — soft float is ~10× slower; fast integer approximations preferred |
| **ARM Cortex-M7** | Double-precision FPU, hardware sqrt, larger caches |
| **FPGA soft-CPU** | Custom ALU, parallel pixel pipelines, fixed-point DSP |
| **Desktop simulation** | SSE/AVX SIMD (process 4–8 pixels per instruction) |

A **backend abstraction** lets us write the VM interpreter once and swap in
platform-optimised math implementations at compile time.

### 2.2 Design Principles

1. **Zero runtime overhead** — backends are selected at compile time via `#if` / template
   specialisation / `constexpr if`. No virtual dispatch, no function pointers.
2. **Scalar-first** — the default backend operates on individual `float` values, matching
   the current VM. Future backends may process pixels in batches (SIMD).
3. **Opt-in optimisation** — each platform enables its backend via a compile flag. The
   scalar-float backend is always the fallback.
4. **Shared in `lib/ProtoGL/src/`** — the backend header is shared between VM code and
   potentially host-side simulation code (useful for PC-based testing).

### 2.3 API Definition

```cpp
// File: lib/ProtoGL/src/PglShaderBackend.h

namespace PglShaderBackend {

// ─── Configuration flags (set by build system or gpu_config.h) ───
//
// PGL_BACKEND_SCALAR_FLOAT  — Standard C math (default, always available)
// PGL_BACKEND_CM33_FPV5     — Cortex-M33 FPv5 optimisations (fmaf, vsqrt)
// PGL_BACKEND_CM33_DSP      — Cortex-M33 DSP blend intrinsics
// PGL_BACKEND_SOFT_FLOAT    — Integer-only approximate math (RISC-V no FPU)
// PGL_BACKEND_SIMD_SSE      — x86 SSE batch processing (desktop sim)

// ─── Arithmetic ─────────────────────────────────────────────────

float Add(float a, float b);
float Sub(float a, float b);
float Mul(float a, float b);
float Div(float a, float b);         // safe: b=0 → 0
float Fma(float a, float b, float c); // a*b+c (fused if HW supports)
float Neg(float a);

// ─── Math functions ─────────────────────────────────────────────

float Sin(float x);
float Cos(float x);
float Tan(float x);
float Asin(float x);
float Acos(float x);
float Atan(float x);
float Atan2(float y, float x);
float Pow(float base, float exp);
float Exp(float x);
float Log(float x);
float Sqrt(float x);
float Rsqrt(float x);               // 1/sqrt(x), safe: x≤0 → 0

// ─── Rounding / value manipulation ──────────────────────────────

float Abs(float x);
float Sign(float x);
float Floor(float x);
float Ceil(float x);
float Fract(float x);
float Mod(float x, float y);

// ─── Clamping / interpolation ───────────────────────────────────

float Min(float a, float b);
float Max(float a, float b);
float Clamp(float x, float lo, float hi);
float Mix(float a, float b, float t);
float Step(float edge, float x);
float Smoothstep(float e0, float e1, float x);

// ─── Geometric (2D/3D) ─────────────────────────────────────────

float Dot2(float ax, float ay, float bx, float by);
float Dot3(float ax, float ay, float az, float bx, float by, float bz);
float Len2(float x, float y);
float Len3(float x, float y, float z);
// Normalize writes N components starting at outBase.
void  Norm2(float x, float y, float& outX, float& outY);
void  Norm3(float x, float y, float z, float& outX, float& outY, float& outZ);
void  Cross(float ax, float ay, float az,
            float bx, float by, float bz,
            float& outX, float& outY, float& outZ);
float Dist2(float ax, float ay, float bx, float by);

// ─── Texture sampling ───────────────────────────────────────────

// Sample RGB565 framebuffer at pixel coords (nearest-neighbour).
void TexSample(const uint16_t* fb, uint16_t w, uint16_t h,
               float u, float v,
               float& outR, float& outG, float& outB);

// ─── Pixel packing (used by shader dispatch, not the VM itself) ─

uint16_t PackRGB565(float r, float g, float b);  // float [0,1] → RGB565
void     UnpackRGB565(uint16_t pixel, float& r, float& g, float& b);

}  // namespace PglShaderBackend
```

### 2.4 Backend Implementations

#### 2.4.1 Scalar Float (Default)

```
PGL_BACKEND_SCALAR_FLOAT — always available, no platform dependencies.
```

Maps directly to `<cmath>` functions: `sinf`, `cosf`, `sqrtf`, etc.
`Fma()` calls `fmaf()` (which is single-cycle on Cortex-M33 FPv5, and
the compiler generates the optimal instruction for other platforms).

This is the **reference implementation** that all other backends must match
in output (within float32 tolerance).

#### 2.4.2 Cortex-M33 FPv5

```
PGL_BACKEND_CM33_FPV5 — enabled when __ARM_FEATURE_FMA is defined.
```

Overrides:
- `Fma()` → `__builtin_fmaf()` (guaranteed single-cycle FMA)
- `Sqrt()` → `__builtin_sqrtf()` (VSQRT instruction)
- `Rsqrt()` → fast Newton-Raphson using VRSQRTE if available

Everything else falls through to scalar-float.

#### 2.4.3 Cortex-M33 DSP Blend

```
PGL_BACKEND_CM33_DSP — enabled when __ARM_FEATURE_DSP is defined.
```

Overrides:
- `PackRGB565()` / `UnpackRGB565()` — use half-word packing
- `Mix()` — fixed-point 8-bit interpolation for RGB channels (used post-VM)

Note: DSP intrinsics operate on integer-packed values, not floats. This
backend is primarily for the **shader dispatch loop** (intensity blending),
not the inner VM interpreter.

#### 2.4.4 Soft Float (Integer Approximation)

```
PGL_BACKEND_SOFT_FLOAT — explicitly enabled for cores without FPU.
```

For RISC-V Hazard3 (no F extension) or other integer-only cores:
- `Sin()`/`Cos()` → polynomial approximation (Chebyshev or Bhaskara)
- `Sqrt()` → integer fast-inverse-sqrt (Quake algorithm) adapted for float
- `Pow()` → `exp(exp * log(base))` using integer log2 approximation
- `Div()` → Newton-Raphson reciprocal iteration

Trade-off: ~1-2 LSB precision loss, but avoids soft-float library overhead
(which adds ~5-50× cycle penalty for each operation on integer-only cores).

#### 2.4.5 SIMD / Batch (Future - Desktop Simulation)

```
PGL_BACKEND_SIMD_SSE — enabled on x86 with __SSE2__.
```

Processes 4 pixels simultaneously using 128-bit SSE registers. Requires
restructuring the VM loop to batch 4 consecutive pixels through each
instruction (SIMT-style execution). This is a **future extension** for
desktop simulation / testing performance.

### 2.5 Integration with VM

The VM interpreter (`pgl_shader_vm.cpp`) will replace direct `<cmath>` calls
with `PglShaderBackend::` namespace calls:

```cpp
// Before:
case PSB_OP_SIN: result = sinf(a); break;

// After:
case PSB_OP_SIN: result = PglShaderBackend::Sin(a); break;
```

The backend header uses `static inline` functions, so there is **zero** call
overhead — the compiler inlines everything directly into the switch body.

### 2.6 Integration with Screenspace Dispatch

The shader dispatch loop in `screenspace_effects.cpp` will use:
- `PglShaderBackend::UnpackRGB565()` / `PackRGB565()` for pixel I/O
- `PglShaderBackend::Mix()` for intensity blending
- `PglShaderBackend::Clamp()` for output clamping

This centralises all platform-specific math into one header.

---

## 3. General Job Scheduler (`PglJobScheduler`)

### 3.1 Motivation

The current multi-core coordination in `gpu_core.cpp` is:

```
Core 0: PrepareFrame → signal FIFO → RasterizeTop → wait FIFO → Shaders → Swap
Core 1: wait FIFO → RasterizeBottom → signal FIFO
```

Problems:
1. **Hardcoded to 2 cores** — won't scale to 4-core or single-core targets.
2. **Shaders are single-core** — Core 1 is idle during the entire shader phase.
3. **No tile-based splitting** — horizontal band split may cause load imbalance
   (top half has more triangles than bottom, or vice versa).
4. **RP2350 FIFO is not portable** — `multicore_fifo_*` is a Pico SDK primitive.
5. **No abstraction** — changing the scheduling strategy requires rewriting `gpu_core.cpp`.

### 3.2 Design: `PglJobScheduler` Interface

```cpp
// File: lib/ProtoGL/src/PglJobScheduler.h (shared header)

/// A unit of parallel work.
struct PglJob {
    void (*func)(void* ctx);   // Job function
    void* ctx;                 // Opaque context pointer
};

/// Abstract job scheduler interface.
/// Platform implementations provide the actual dispatch mechanism.
class PglJobScheduler {
public:
    virtual ~PglJobScheduler() = default;

    /// Return number of worker cores available (excluding the caller).
    virtual uint8_t WorkerCount() const = 0;

    /// Submit a batch of jobs. The scheduler distributes them across workers.
    /// The caller MAY execute some jobs on the calling core (work-stealing).
    /// @param jobs    Array of jobs to execute.
    /// @param count   Number of jobs (1..maxJobs).
    virtual void Submit(const PglJob* jobs, uint8_t count) = 0;

    /// Block until all submitted jobs in the current batch are complete.
    /// The caller may execute pending work while waiting (cooperative wait).
    /// @param idleFunc  Optional function to call while waiting (e.g. HUB75 refresh).
    virtual void WaitAll(void (*idleFunc)() = nullptr) = 0;
};
```

### 3.3 Platform Implementations

#### 3.3.1 RP2350 (Pico SDK Multicore FIFO)

```
File: firmwares/rp2350/src/scheduler/pgl_job_scheduler_rp2350.h/.cpp
```

| Aspect | Implementation |
|---|---|
| Workers | 1 (Core 1) |
| Dispatch | Core 0 writes job pointer to `multicore_fifo_push_blocking()` |
| Core 1 loop | `multicore_fifo_pop_blocking()` → cast to `PglJob*` → call `func(ctx)` → push DONE |
| WaitAll | Poll `multicore_fifo_rvalid()` while calling `idleFunc()` (HUB75 refresh) |
| Submit(N>1) | Core 0 executes jobs[0..N-2] on calling core, sends jobs[N-1] to Core 1 |

This replaces the current `FIFO_CMD_START_RENDER` / `FIFO_CMD_RENDER_DONE` protocol
with a general-purpose job dispatch, while using the **same underlying RP2350 FIFO primitive**.

#### 3.3.2 Single-Core (Fallback)

```
File: lib/ProtoGL/src/PglJobScheduler_SingleCore.h
```

| Aspect | Implementation |
|---|---|
| Workers | 0 |
| Submit | Execute all jobs immediately on the calling core |
| WaitAll | No-op (already complete) |

Used for:
- RISC-V single-core targets
- Desktop simulation (single-threaded)
- Debugging (deterministic execution)

#### 3.3.3 FreeRTOS (Future)

```
For ESP32-P4 or other RTOS-based targets.
```

| Aspect | Implementation |
|---|---|
| Workers | N (configurable task pool) |
| Dispatch | `xQueueSend()` to worker task queues |
| WaitAll | `xSemaphoreTake()` on completion semaphore |

### 3.4 Job Types for GPU Pipeline

> **Note:** The table below describes the M10 job-scheduler approach (Y-band split).
> As of M10a, rasterisation uses `PglTileScheduler` with 32 tiles instead of
> 2 Y-band jobs. See §3.7 for the current tile-based architecture.
> The generic `PglJob`/`DispatchPair()` approach is still valid for shader parallelism.

The GPU pipeline decomposes into these parallelisable jobs:

| Job | Current | With Scheduler |
|---|---|---|
| Rasterize top half | Core 0 (hardcoded) | `PglJob{RasterizeRange, &topCtx}` |
| Rasterize bottom half | Core 1 (hardcoded) | `PglJob{RasterizeRange, &bottomCtx}` |
| Apply shader slot 0 | Core 0 only | `PglJob{ApplyShaderRange, &shader0Ctx}` |
| Apply shader slot 1 | Core 0 only | `PglJob{ApplyShaderRange, &shader1Ctx}` |

### 3.5 Parallel Shader Execution

With the job scheduler, shader execution can be parallelised across cores:

**Option A: Per-slot parallelism** (simple, no dependencies between slots)
```
Core 0: shader slot 0 (all pixels)
Core 1: shader slot 1 (all pixels)
```
Works when slots are independent (don't read each other's output).

**Option B: Per-band parallelism** (same shader, split by Y range)
```
Core 0: shader slot 0, Y=[0, H/2)
Core 1: shader slot 0, Y=[H/2, H)
```
Always safe — each core writes non-overlapping pixel rows.

**Option B is preferred** for ProtoGL because:
- Shaders typically read from a scratch buffer (snapshot) and write to the framebuffer
- Y-band splitting is already proven for rasterisation
- No ordering constraints between pixel rows

### 3.6 Refactored GPU Core Loop

```
// Pseudocode with scheduler:

PrepareFrame(scene);

// Parallel rasterisation
PglJob rasterJobs[2] = {
    {RasterizeRange, &topCtx},
    {RasterizeRange, &bottomCtx}
};
scheduler->Submit(rasterJobs, 2);
scheduler->WaitAll(Hub75PollRefresh);

// Parallel shader execution
for (each active shader slot) {
    CopyFbToScratch();  // serial, fast memcpy
    PglJob shaderJobs[2] = {
        {ApplyShaderRange, &shaderTopCtx},
        {ApplyShaderRange, &shaderBottomCtx}
    };
    scheduler->Submit(shaderJobs, 2);
    scheduler->WaitAll(Hub75PollRefresh);
}

SwapBuffers();
```

### 3.7 Tile-Based Rasterisation (Implemented — M10a)

The 128×64 panel is divided into **32 tiles of 16×16 pixels** (8 columns × 4 rows).
Tiles are dispatched via `PglTileScheduler`, a bare-metal work-stealing scheduler
that replaces the Y-band split for rasterisation.

#### Architecture

```
PglTileScheduler (non-virtual, bare-metal)
├── TileConfig::MORTON_ORDER[32]    ← Z-curve tile traversal order
├── TilePassContext (stack-allocated)
│   ├── rasterizer*, fb*, zBuf*      ← per-frame render state
│   ├── volatile nextTile            ← atomic work counter (0..32)
│   └── volatile coresFinished       ← completion counter
├── DispatchTilePass()               ← dual-core tile rasterisation
│   ├── Core 0: ProcessTiles()       ← pulls tiles via __atomic_fetch_add
│   └── Core 1: ProcessTiles()       ← same loop, same counter
└── DispatchPair()                   ← generic 2-function dispatch (shaders)
```

#### Work-Stealing Protocol

```
// Both cores execute this simultaneously:
while (true) {
    uint32_t idx = __atomic_fetch_add(&ctx->nextTile, 1, __ATOMIC_RELAXED);
    if (idx >= 32) break;       // all tiles claimed
    uint8_t tile = MORTON_ORDER[idx];
    uint16_t col = tile % 8, row = tile / 8;
    rasterizer->RasterizeTile(fb, zBuf, col, row, 16, 16);
}
```

#### Per-Tile QuadTree Caching

`RasterizeTile()` queries the QuadTree **once** for the entire 16×16 AABB,
caching the candidate triangle list.  Then it iterates all 256 pixels testing
only those candidates.  This amortises the QuadTree traversal cost 256× compared
to the per-pixel query in `RasterizeRange()`.

| Metric | RasterizeRange (Y-band) | RasterizeTile (tile) |
|---|---|---|
| QuadTree queries | 128 × 64 = 8,192 | 32 |
| Query AABB size | 1×1 pixel | 16×16 pixels |
| Candidate re-use | none | 256× per tile |
| Load balance | fixed 50/50 | dynamic work-stealing |
| Cache fit | 128×32 band → 8 KB FB | 16×16 tile → 512 B FB |

#### Morton Z-Order

Tiles are processed in Morton Z-curve order instead of row-major.  This
ensures that consecutively-claimed tiles are spatially adjacent, improving
cache locality when both cores work on nearby regions.

```
Morton order for 8×4 grid:
 0  1  4  5 16 17 20 21
 2  3  6  7 18 19 22 23
 8  9 12 13 24 25 28 29
10 11 14 15 26 27 30 31
```

#### Memory Footprint per Tile

| Component | Size | Notes |
|---|---|---|
| Framebuffer | 512 B | 16×16 × 2 bytes (RGB565) |
| Z-buffer | 1,024 B | 16×16 × 4 bytes (float) |
| Candidate list | 256 B | 128 handles × 2 bytes |
| **Total** | **1,792 B** | Fits in Cortex-M33 L1/TCM |

#### Inter-Core Protocol (FIFO)

| Direction | Command | Payload |
|---|---|---|
| Core 0 → Core 1 | `FIFO_CMD_TILE_PASS` (0x54494C45) | `&passCtx_` |
| Core 0 → Core 1 | `FIFO_CMD_PAIR` (0x50414952) | `func`, `ctx` |
| Core 1 → Core 0 | `FIFO_DONE` (0x444F4E45) | — |

#### Files

| File | Location | Lines | Description |
|---|---|---|---|
| `pgl_tile_scheduler.h` | `firmwares/rp2350/src/scheduler/` | ~150 | Tile scheduler header + TileConfig + TilePassContext |
| `pgl_tile_scheduler.cpp` | `firmwares/rp2350/src/scheduler/` | ~200 | Lock-free tile dispatch implementation |

---

## 4. File Plan

### 4.1 New Files

| File | Location | Lines (est.) | Description |
|---|---|---|---|
| `PglShaderBackend.h` | `lib/ProtoGL/src/` | ~300 | Backend abstraction header + scalar-float default |
| `PglJobScheduler.h` | `lib/ProtoGL/src/` | ~60 | Abstract scheduler interface |
| `PglJobScheduler_SingleCore.h` | `lib/ProtoGL/src/` | ~40 | Inline single-core fallback |
| `pgl_job_scheduler_rp2350.h` | `firmwares/rp2350/src/scheduler/` | ~40 | RP2350 generic scheduler declaration |
| `pgl_job_scheduler_rp2350.cpp` | `firmwares/rp2350/src/scheduler/` | ~100 | RP2350 FIFO-based generic implementation |
| `pgl_tile_scheduler.h` | `firmwares/rp2350/src/scheduler/` | ~150 | Tile-based work-stealing scheduler header |
| `pgl_tile_scheduler.cpp` | `firmwares/rp2350/src/scheduler/` | ~200 | Lock-free tile dispatch implementation |

### 4.2 Modified Files

| File | Changes |
|---|---|
| `pgl_shader_vm.cpp` | Replace `<cmath>` calls with `PglShaderBackend::` namespace calls |
| `screenspace_effects.cpp` | Use backend for pixel pack/unpack/blend; add `ApplyShaderRange()` for parallel dispatch |
| `gpu_core.h` | Remove FIFO constants (moved to scheduler) |
| `gpu_core.cpp` | Use `PglTileScheduler` for tile rasterisation + `DispatchPair()` for shader dispatch |
| `rasterizer.h` | Add `RasterizeTile()` method for tile-based rasterisation |
| `rasterizer.cpp` | Implement `RasterizeTile()` with per-tile QuadTree query + cached candidate iteration |
| `CMakeLists.txt` | Add `scheduler/pgl_tile_scheduler.cpp` and `scheduler/pgl_job_scheduler_rp2350.cpp` |
| `Shader_System_Design.md` | Add §12 compile-location policy, update §6 with backend references |

---

## 5. Implementation Milestones

### M10: Backend Abstraction + Job Scheduler (2 weeks)

| Week | Day | Task | Deliverable |
|---|---|---|---|
| **W1** | Mon | Create `PglShaderBackend.h` with scalar-float default | All math/texture/pack functions implemented |
| | Tue | Wire backend into `pgl_shader_vm.cpp` — replace all `<cmath>` calls | VM produces identical output through backend |
| | Wed | Wire backend into `screenspace_effects.cpp` — pack/unpack/blend | Shader dispatch uses backend |
| | Thu | Add `PGL_BACKEND_CM33_FPV5` conditional path (fmaf, vsqrt) | Compiles on Cortex-M33 with FPv5 optimisations |
| | Fri | Add `PGL_BACKEND_SOFT_FLOAT` stub (document API, placeholder impls) | Compiles on integer-only targets |
| **W2** | Mon | Create `PglJobScheduler.h` interface + `PglJobScheduler_SingleCore.h` | Interface defined, single-core fallback works |
| | Tue | Implement `pgl_job_scheduler_rp2350.cpp` using multicore FIFO | Core 1 executes jobs dispatched via scheduler |
| | Wed | Refactor `gpu_core.cpp` to use scheduler for rasterisation | Rasterisation uses job dispatch (same 2-core split) |
| | Thu | Add `ApplyShaderRange()` to screenspace_effects. Parallel shader dispatch via scheduler. | Shaders run on both cores |
| | Fri | Update docs, benchmark parallel shaders, verify zero regression | Design doc + schedule updated |

### Exit Criteria (M10)
- `PglShaderBackend` header provides all shader math operations with scalar-float default
- VM and shader dispatch use backend exclusively (no direct `<cmath>` in render code)
- `PglJobScheduler` interface defined with RP2350 and single-core implementations
- Rasterisation and shader execution use the scheduler for multi-core dispatch
- Documentation updated (this document + Shader_System_Design.md + Project_Schedule.md)

---

## 6. SRAM / Performance Impact

### Backend Overhead
- **Zero runtime overhead.** All backend functions are `static inline` in a header.
  The compiler inlines them identically to the current direct `<cmath>` calls.
- **Code size:** ~0 bytes additional (inlined functions replace equivalent calls).

### Scheduler Overhead
- `PglJobScheduler_RP2350` instance: ~20 bytes (vtable ptr + FIFO state).
- Job arrays: stack-allocated, 16 bytes per job (function pointer + context pointer).
- One additional FIFO round-trip per shader slot (~2 µs per slot on RP2350).
- **Net gain:** Parallel shader execution on 2 cores should roughly halve shader time
  for complex effects (~0.05 ms → ~0.03 ms for a 40-instruction shader).

---

## 7. Platform Compatibility Matrix

| Platform | Backend | Scheduler | Notes |
|---|---|---|---|
| RP2350 Cortex-M33 | `SCALAR_FLOAT` + `CM33_FPV5` | `RP2350` (FIFO) | Current primary target |
| RP2350 Hazard3 RISC-V | `SOFT_FLOAT` | `RP2350` (FIFO) | Same chip, RISC-V mode |
| ESP32-P4 | `SCALAR_FLOAT` | `FreeRTOS` (future) | Dual-core, RTOS-based |
| STM32H7 Cortex-M7 | `SCALAR_FLOAT` + `CM7_FPV5` | `SingleCore` or `FreeRTOS` | Double-precision FPU |
| Desktop (x86/ARM64) | `SCALAR_FLOAT` (+ `SIMD_SSE` future) | `SingleCore` | Simulation / testing |
| FPGA soft-CPU | `SOFT_FLOAT` | `SingleCore` | Custom ALU |
