# ProtoGL Programmable Shader System — Design Document

## 1. Motivation

The current shader system has 3 hardcoded classes (CONVOLUTION, DISPLACEMENT, COLOR_ADJUST)
with fixed parameter structs. Every new effect requires firmware changes of C++ code, recompilation, and
reflashing the RP2350. This document plans a **programmable shader system** that lets
users write screen-space effects in a **GLSL-like language (PGLSL)** that compiles to a
compact bytecode, uploaded to the GPU at runtime.

### Current State

```
Host (ESP32-S3)                              GPU (RP2350)
──────────────────────                       ────────────────────
PglEncoder::SetShader(                       screenspace_effects.cpp
  class = CONVOLUTION,     ──────SPI──────►  switch(shaderClass) {
  params = {radius, angle...}                  case CONVOLUTION: ...
)                                              case DISPLACEMENT: ...
                                               case COLOR_ADJUST: ...
                                             }
```

- 3 fixed shader classes, all behaviour in C++
- 20-byte inline parameter struct per slot
- No user-defined effects without firmware changes

### Target State

```
Host (ESP32-S3)                              GPU (RP2350)
──────────────────────                       ────────────────────
// Upload compiled shader bytecode           PglShaderVM executes
pgl.CreateShaderProgram(                     bytecode per pixel:
  id = 1,                     ──SPI──►        for each pixel:
  bytecode = {...},                              vm.Execute(prog,
  uniformCount = 3                                 fragCoord, color)
)
pgl.SetShaderProgram(                        Uniforms set per-frame
  camera, slot,                              via SetShaderUniform
  programId = 1
)
pgl.SetShaderUniform(
  programId = 1,
  "u_time", elapsedTime
)
```

---

## 2. Design Constraints

| Constraint | Value | Impact |
|---|---|---|
| CPU | Cortex-M33 @ 150 MHz (up to 300 MHz) | ~18,000 cycles/pixel at 150 MHz for 1 ms budget over 8192 px |
| SRAM | 520 KB total, ~356 KB used | ~160 KB headroom; VM + program storage must be small |
| Flash | 4 MB | Ample for interpreter code |
| Framebuffer | 128×64 RGB565 (8192 pixels) | Small pixel count = forgiving for interpreter overhead |
| Shader budget | < 1 ms per shader (4 slots, < 4 ms total) | ~18K cycles/px at 1 ms = bytecode VM is feasible |
| Wire bandwidth | 10 MB/s Octal SPI | Shader programs uploaded once, not per-frame |

**Feasibility estimate:** A 40-instruction shader at ~8 cycles/instruction = 320 cycles/pixel.
8192 pixels × 320 cycles = 2.6M cycles = **~17 µs at 150 MHz**. Well within the 1 ms budget.
Even a 200-instruction shader would be ~0.09 ms. Bytecode interpretation is highly viable.

---

## 3. PGL Shader Language (PGLSL) Specification

### 3.1 Language Overview

PGLSL is a strict subset of GLSL ES 1.00 with restrictions suitable for a register-based
bytecode VM on a microcontroller. It describes **fragment-only** screen-space effects.

```glsl
// Example: Chromatic aberration
#version 100
precision mediump float;

uniform float u_time;
uniform float u_strength;

void main() {
    vec2 uv = gl_FragCoord.xy / u_resolution;
    vec2 dir = uv - vec2(0.5);
    float dist = length(dir);
    float offset = dist * u_strength * sin(u_time * 3.0);

    vec3 color;
    color.r = texture2D(u_framebuffer, uv + dir * offset).r;
    color.g = texture2D(u_framebuffer, uv).g;
    color.b = texture2D(u_framebuffer, uv - dir * offset).b;

    gl_FragColor = vec4(color, 1.0);
}
```

### 3.2 Type System

| Type | Size (VM registers) | Description |
|---|---|---|
| `float` | 1 | 32-bit IEEE 754 |
| `vec2` | 2 | 2× float |
| `vec3` | 3 | 3× float |
| `vec4` | 4 | 4× float |
| `int` | 1 | 32-bit signed (cast to float in VM) |
| `bool` | 1 | 0.0 or 1.0 |
| `sampler2D` | 1 | Framebuffer/texture handle (readonly) |

### 3.3 Built-in Variables

| Variable | Type | Description |
|---|---|---|
| `gl_FragCoord` | `vec4` | `{x, y, 0, 1}` — pixel coordinates (0-based) |
| `gl_FragColor` | `vec4` | Output colour (write-only). `.rgb` mapped to RGB565, `.a` ignored unless blending. |
| `u_resolution` | `vec2` | `{width, height}` — framebuffer dimensions (auto-bound) |
| `u_time` | `float` | Elapsed time in seconds (auto-bound from `elapsedTimeS`) |
| `u_framebuffer` | `sampler2D` | The current framebuffer contents (read-only snapshot from scratch buffer) |

### 3.4 Built-in Functions

Matches GLSL ES 1.00 subset — all pure functions, no side effects:

**Math (scalar + component-wise):**
```
float sin(float), cos(float), tan(float)
float asin(float), acos(float), atan(float), atan(float, float)
float pow(float, float), exp(float), log(float), exp2(float), log2(float)
float sqrt(float), inversesqrt(float)
float abs(float), sign(float), floor(float), ceil(float), fract(float)
float mod(float, float)
float min(float, float), max(float, float), clamp(float, float, float)
float mix(float, float, float)      // lerp
float step(float, float)
float smoothstep(float, float, float)
```

**Geometric:**
```
float length(vecN)
float distance(vecN, vecN)
float dot(vecN, vecN)
vec3  cross(vec3, vec3)
vecN  normalize(vecN)
vecN  reflect(vecN, vecN)
```

**Texture:**
```
vec4 texture2D(sampler2D, vec2)     // nearest-neighbour sample from framebuffer
```

**Not supported (GLSL features omitted):**
- No `for`/`while` loops (use unrolled expressions or built-in `mix`/`step` for conditionals)
- No `if`/`else` branching (use `mix(a, b, step(threshold, value))` pattern)
- No user-defined functions (only `void main()`)
- No arrays or matrices beyond vec4
- No `discard`
- No `varying`/`attribute` (no vertex stage)

### 3.5 Swizzle Support

Standard GLSL swizzle on all vector types:
```glsl
vec3 c = color.rgb;   // read
color.rb = vec2(1.0, 0.5);  // write
float r = color.r;
vec2 xy = gl_FragCoord.xy;
```

Sets: `{x,y,z,w}` and `{r,g,b,a}` (interchangeable).

### 3.6 Restrictions Summary

These restrictions keep the compiler simple and the bytecode compact:

1. **No control flow** — no `if`, `for`, `while`, `switch`. Use `mix`/`step`/`smoothstep`.
2. **No user functions** — only `void main()`. Inline all logic.
3. **Max 16 uniforms** per program (128 bytes).
4. **Max 256 bytecode instructions** per program (~1 KB).
5. **Max 32 float registers** in the VM (128 bytes register file).
6. **One texture sampler** (`u_framebuffer`). Future: up to 4 samplers.
7. **Write-once output** — `gl_FragColor` assigned once at end of `main()`.

---

## 4. Bytecode Format (PGL Shader Bytecode — PSB)

### 4.1 Register-Based VM Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  Register File: r0–r31 (32 × float = 128 bytes)            │
├─────────────────────────────────────────────────────────────┤
│  Uniform Table: u0–u15 (16 × float = 64 bytes)             │
├─────────────────────────────────────────────────────────────┤
│  Constants Pool: c0–c31 (32 × float, embedded in program)  │
├─────────────────────────────────────────────────────────────┤
│  Built-ins (auto-loaded per pixel):                         │
│    r0 = gl_FragCoord.x                                      │
│    r1 = gl_FragCoord.y                                      │
│    r2 = 0.0 (z)                                             │
│    r3 = 1.0 (w)                                             │
│    r4 = current pixel R (0.0–1.0)                           │
│    r5 = current pixel G (0.0–1.0)                           │
│    r6 = current pixel B (0.0–1.0)                           │
│    r7 = 1.0 (alpha)                                         │
├─────────────────────────────────────────────────────────────┤
│  Output: r28=R, r29=G, r30=B, r31=A (gl_FragColor)         │
└─────────────────────────────────────────────────────────────┘
```

### 4.2 Instruction Encoding

Each instruction is 4 bytes (fixed-width for fast dispatch):

```
┌──────────┬──────────┬──────────┬──────────┐
│ opcode   │ dst      │ srcA     │ srcB     │
│ (8 bits) │ (8 bits) │ (8 bits) │ (8 bits) │
└──────────┴──────────┴──────────┴──────────┘
```

**Operand encoding (8 bits):**
- `0x00–0x1F` → Register `r0–r31`
- `0x20–0x2F` → Uniform `u0–u15`
- `0x30–0x4F` → Constant pool `c0–c31`
- `0x50–0x5F` → Literal small constant (0.0, 0.5, 1.0, 2.0, -1.0, π, etc.)
- `0xFF` → unused / ignored

### 4.3 Instruction Set

**Arithmetic (dst = srcA op srcB):**

| Opcode | Mnemonic | Operation |
|---|---|---|
| 0x01 | `MOV` | dst = srcA |
| 0x02 | `ADD` | dst = srcA + srcB |
| 0x03 | `SUB` | dst = srcA - srcB |
| 0x04 | `MUL` | dst = srcA × srcB |
| 0x05 | `DIV` | dst = srcA / srcB (safe: srcB=0 → 0) |
| 0x06 | `FMA` | dst = srcA × srcB + dst (fused multiply-add) |
| 0x07 | `NEG` | dst = -srcA |

**Math functions (dst = fn(srcA) or fn(srcA, srcB)):**

| Opcode | Mnemonic | Operation |
|---|---|---|
| 0x10 | `SIN` | dst = sin(srcA) |
| 0x11 | `COS` | dst = cos(srcA) |
| 0x12 | `TAN` | dst = tan(srcA) |
| 0x13 | `ASIN` | dst = asin(srcA) |
| 0x14 | `ACOS` | dst = acos(srcA) |
| 0x15 | `ATAN` | dst = atan(srcA) |
| 0x16 | `ATAN2` | dst = atan2(srcA, srcB) |
| 0x17 | `POW` | dst = pow(srcA, srcB) |
| 0x18 | `EXP` | dst = exp(srcA) |
| 0x19 | `LOG` | dst = log(srcA) |
| 0x1A | `SQRT` | dst = sqrt(srcA) |
| 0x1B | `RSQRT` | dst = 1/sqrt(srcA) |
| 0x1C | `ABS` | dst = abs(srcA) |
| 0x1D | `SIGN` | dst = sign(srcA) |
| 0x1E | `FLOOR` | dst = floor(srcA) |
| 0x1F | `CEIL` | dst = ceil(srcA) |
| 0x20 | `FRACT` | dst = fract(srcA) |
| 0x21 | `MOD` | dst = mod(srcA, srcB) |

**Clamping / interpolation:**

| Opcode | Mnemonic | Operation |
|---|---|---|
| 0x30 | `MIN` | dst = min(srcA, srcB) |
| 0x31 | `MAX` | dst = max(srcA, srcB) |
| 0x32 | `CLAMP` | dst = clamp(srcA, srcB, dst) *(3-operand: lo=srcB, hi=dst)* |
| 0x33 | `MIX` | dst = mix(srcA, srcB, dst) *(3-operand: t=dst)* |
| 0x34 | `STEP` | dst = step(srcA, srcB) → srcB >= srcA ? 1.0 : 0.0 |
| 0x35 | `SSTEP` | dst = smoothstep(srcA, srcB, dst) *(3-operand)* |

**Geometric (vector operations on consecutive registers):**

| Opcode | Mnemonic | Operation |
|---|---|---|
| 0x40 | `DOT2` | dst = srcA·srcA+1 dot srcB·srcB+1 (2D dot product) |
| 0x41 | `DOT3` | dst = 3-component dot product (srcA..+2, srcB..+2) |
| 0x42 | `LEN2` | dst = length(srcA, srcA+1) |
| 0x43 | `LEN3` | dst = length(srcA..+2) |
| 0x44 | `NORM2` | dst,dst+1 = normalize(srcA, srcA+1) |
| 0x45 | `NORM3` | dst..+2 = normalize(srcA..+2) |
| 0x46 | `CROSS` | dst..+2 = cross(srcA..+2, srcB..+2) |
| 0x47 | `DIST2` | dst = distance(srcA,srcA+1, srcB,srcB+1) |

**Texture sampling:**

| Opcode | Mnemonic | Operation |
|---|---|---|
| 0x50 | `TEX2D` | dst..+3 = texture2D(framebuffer, srcA, srcA+1) → {R,G,B,A} |

**Load / store:**

| Opcode | Mnemonic | Operation |
|---|---|---|
| 0x60 | `LCONST` | dst = constants[srcA] *(srcA is pool index, not register)* |
| 0x61 | `LUNI` | dst = uniforms[srcA] |

**Special:**

| Opcode | Mnemonic | Operation |
|---|---|---|
| 0x00 | `NOP` | No operation |
| 0xFF | `END` | Halt execution, output r28–r31 |

### 4.4 Program Binary Layout

```
┌──────────────────────────────────────────────────────────────┐
│ PglShaderProgramHeader (16 bytes)                            │
│   magic:     0x50534231 ("PSB1")                             │
│   version:   uint8_t  (1)                                    │
│   flags:     uint8_t  (bit0: needsScratchCopy)               │
│   constCount:   uint8_t  (0–32)                              │
│   uniformCount: uint8_t  (0–16)                              │
│   instrCount:   uint16_t (0–256)                             │
│   nameHash:     uint32_t (FNV-1a of source text, for debug)  │
│   reserved:     uint16_t                                     │
├──────────────────────────────────────────────────────────────┤
│ Uniform descriptor table (uniformCount × 8 bytes)            │
│   Each entry:                                                │
│     nameHash: uint32_t (FNV-1a of uniform name)              │
│     type:     uint8_t  (0=float, 1=vec2, 2=vec3, 3=vec4)    │
│     slot:     uint8_t  (uniform index 0–15)                  │
│     defaultValueOffset: uint16_t (byte offset into constants)│
├──────────────────────────────────────────────────────────────┤
│ Constants pool (constCount × 4 bytes)                        │
│   float values embedded in program                           │
├──────────────────────────────────────────────────────────────┤
│ Instructions (instrCount × 4 bytes)                          │
│   Fixed-width bytecode (see §4.2)                            │
└──────────────────────────────────────────────────────────────┘
```

**Max program size:** 16 + (16 × 8) + (32 × 4) + (256 × 4) = 16 + 128 + 128 + 1024 = **1296 bytes**.
Typical program: 4 uniforms, 8 constants, 40 instructions = 16 + 32 + 32 + 160 = **240 bytes**.

---

## 5. Wire Protocol Additions (ProtoGL v0.6)

### 5.1 New Opcodes

| Opcode | Name | Payload |
|---|---|---|
| `0x84` | `CMD_CREATE_SHADER_PROGRAM` | `PglCmdCreateShaderProgramHeader` + bytecode blob |
| `0x85` | `CMD_DESTROY_SHADER_PROGRAM` | `uint16_t programId` |
| `0x86` | `CMD_BIND_SHADER_PROGRAM` | `PglCmdBindShaderProgram` |
| `0x87` | `CMD_SET_SHADER_UNIFORM` | `PglCmdSetShaderUniform` + value data |

### 5.2 Payload Structs

```c
// CMD_CREATE_SHADER_PROGRAM (0x84)
// Followed by: PglShaderProgramHeader + uniform descriptors + constants + instructions
struct PglCmdCreateShaderProgramHeader {
    uint16_t programId;     // GPU-side handle (0–63)
    uint16_t bytecodeSize;  // Total size of PSB blob following this header
};

// CMD_DESTROY_SHADER_PROGRAM (0x85)
struct PglCmdDestroyShaderProgram {
    uint16_t programId;
};

// CMD_BIND_SHADER_PROGRAM (0x86)
// Assigns a compiled program to a camera's shader slot.
// Replaces the old CMD_SET_SHADER for programmable shaders.
// The existing CMD_SET_SHADER (0x83) continues to work for built-in classes.
struct PglCmdBindShaderProgram {
    uint8_t  cameraId;
    uint8_t  shaderSlot;
    uint16_t programId;      // 0xFFFF = unbind (clear slot)
    float    intensity;      // global mix factor (0.0 = bypass, 1.0 = full)
};

// CMD_SET_SHADER_UNIFORM (0x87)
// Sets one or more uniform values for a loaded program.
// Values are applied before the next frame's shader execution.
struct PglCmdSetShaderUniformHeader {
    uint16_t programId;
    uint8_t  uniformSlot;    // 0–15 — index into the program's uniform table
    uint8_t  componentCount; // 1=float, 2=vec2, 3=vec3, 4=vec4
    // Followed by: componentCount × float (4–16 bytes)
};
```

### 5.3 Backward Compatibility

- `CMD_SET_SHADER (0x83)` remains for built-in shaders (CONVOLUTION, DISPLACEMENT, COLOR_ADJUST)
- New `CMD_BIND_SHADER_PROGRAM (0x86)` uses `PGL_SHADER_PROGRAM = 0x04` enum value
- Both paths coexist: slots can contain either built-in or programmable shaders
- GPU firmware without VM support silently ignores 0x84–0x87 (version check via capability flag)

---

## 6. GPU Firmware Changes

### 6.1 New Files

| File | Size Est. | Description |
|---|---|---|
| `render/pgl_shader_vm.h` | ~100 lines | VM class declaration, register file, execute API |
| `render/pgl_shader_vm.cpp` | ~400 lines | Bytecode interpreter loop, built-in function dispatch |
| `render/pgl_shader_program.h` | ~80 lines | Program storage struct, program table |

### 6.2 PglShaderVM Class

```cpp
class PglShaderVM {
public:
    /// Execute a shader program for one pixel.
    /// @param prog    Compiled program (instructions + constants + uniforms)
    /// @param fragX   Pixel X coordinate
    /// @param fragY   Pixel Y coordinate
    /// @param inR     Input red   (0.0–1.0, from framebuffer)
    /// @param inG     Input green (0.0–1.0)
    /// @param inB     Input blue  (0.0–1.0)
    /// @param fb      Framebuffer pointer (for texture2D sampling)
    /// @param w, h    Framebuffer dimensions
    /// @param outR, outG, outB  Output colour (0.0–1.0)
    void Execute(const ShaderProgram& prog,
                 float fragX, float fragY,
                 float inR, float inG, float inB,
                 const uint16_t* fb, uint16_t w, uint16_t h,
                 float& outR, float& outG, float& outB);

private:
    float regs_[32];  // Register file (128 bytes)
};
```

### 6.3 Integration with screenspace_effects.cpp

A new shader class `PGL_SHADER_PROGRAM` is added to the dispatcher:

```cpp
case PGL_SHADER_PROGRAM: {
    // Copy framebuffer to scratch (shader reads from scratch, writes to fb)
    std::memcpy(scratch, fb, pixelCount * 2);
    PglShaderVM vm;
    const ShaderProgram& prog = scene->shaderPrograms[slot.programId];
    for (uint16_t y = 0; y < h; ++y) {
        for (uint16_t x = 0; x < w; ++x) {
            uint32_t idx = y * w + x;
            float inR = R5(scratch[idx]) / 31.0f;
            float inG = G6(scratch[idx]) / 63.0f;
            float inB = B5(scratch[idx]) / 31.0f;
            float outR, outG, outB;
            vm.Execute(prog, (float)x, (float)y, inR, inG, inB,
                       scratch, w, h, outR, outG, outB);
            fb[idx] = PackRGB565(Clamp5((int)(outR * 31.0f)),
                                  Clamp6((int)(outG * 63.0f)),
                                  Clamp5((int)(outB * 31.0f)));
        }
    }
    break;
}
```

### 6.4 Scene State Additions

```cpp
// In scene_state.h:
static constexpr uint8_t PGL_MAX_SHADER_PROGRAMS = 16;

struct ShaderProgram {
    bool     active          = false;
    uint16_t programId       = 0;
    uint8_t  uniformCount    = 0;
    uint8_t  constCount      = 0;
    uint16_t instrCount      = 0;
    uint8_t  flags           = 0;
    float    uniforms[16]    = {};    // Current uniform values (64 bytes)
    float    constants[32]   = {};    // Embedded constants (128 bytes)
    uint32_t instructions[256] = {};  // Bytecode (1024 bytes)
    uint32_t uniformNameHashes[16] = {};  // For name-based uniform lookup
    uint8_t  uniformTypes[16] = {};       // Component counts per uniform
};
// Per-program SRAM: ~1.3 KB.  16 programs = ~21 KB.
```

### 6.5 SRAM Budget Impact

| Component | Size | Notes |
|---|---|---|
| VM register file | 128 bytes | Stack-allocated per shader execution |
| ShaderProgram × 16 | ~21 KB | Max 16 loaded programs |
| **Total new** | **~21 KB** | Budget: 356 → 377 KB / 520 KB (27% headroom) |

If SRAM is tight, reduce `PGL_MAX_SHADER_PROGRAMS` to 8 (~10.5 KB) or 4 (~5.3 KB).

### 6.6 Performance Model

| Shader complexity | Instructions | Cycles/pixel (est.) | Time for 8192 px @ 150 MHz |
|---|---|---|---|
| Simple (brightness) | 5–10 | 40–80 | 0.003–0.005 ms |
| Medium (chromatic aberration) | 30–50 | 240–400 | 0.013–0.022 ms |
| Complex (multi-sample blur) | 100–150 | 800–1200 | 0.044–0.065 ms |
| Maximum (256 instructions) | 256 | ~2000 | ~0.11 ms |

Even the most complex shader is **<0.2 ms** — well within the 1 ms per-slot budget.
Per-instruction cost estimate: ~8 cycles (fetch + decode + FPU op + writeback on CM33).

---

## 7. Host-Side Library Additions

### 7.1 PglEncoder Extensions

New encoder methods in `PglEncoder.h`:

```cpp
void CreateShaderProgram(uint16_t programId,
                         const void* bytecodeBlob, uint16_t bytecodeSize);
void DestroyShaderProgram(uint16_t programId);
void BindShaderProgram(PglCamera cameraId, uint8_t shaderSlot,
                       uint16_t programId, float intensity);
void SetShaderUniform(uint16_t programId, uint8_t uniformSlot,
                      float value);
void SetShaderUniform(uint16_t programId, uint8_t uniformSlot,
                      float x, float y);
void SetShaderUniform(uint16_t programId, uint8_t uniformSlot,
                      float x, float y, float z);
void SetShaderUniform(uint16_t programId, uint8_t uniformSlot,
                      float x, float y, float z, float w);
```

### 7.2 PglShaderCompiler (Host-Side)

A lightweight PGLSL→PSB compiler that runs on ESP32-S3 at build-time or runtime.

**File:** `lib/ProtoGL/src/PglShaderCompiler.h` (~600–800 lines estimated)

**Phases:**
1. **Lexer** — tokenize PGLSL source into tokens (identifiers, numbers, operators, keywords)
2. **Parser** — build a simple AST (expression tree). No control flow = flat sequence of assignments.
3. **Type checker** — validate types, resolve swizzles, check built-in functions
4. **Register allocator** — linear scan allocation over 32 registers (r8–r27 for user temporaries, r0–r7 and r28–r31 reserved)
5. **Code generator** — emit 4-byte instructions from AST

**Compiler API:**
```cpp
class PglShaderCompiler {
public:
    struct CompileResult {
        bool        success;
        uint8_t     bytecode[1296];  // Max program size
        uint16_t    bytecodeSize;
        char        errorMsg[128];
        uint16_t    errorLine;
    };

    static CompileResult Compile(const char* pglslSource, size_t sourceLength);
};
```

**Compile-time cost:** ~1–5 ms for a typical shader (tokenize + parse + emit). Acceptable
for one-time compilation at boot or when effects change.

### 7.3 Offline Compiler (Build Tool)

**File:** `tools/pglslc.py` or `tools/pglslc.cpp`

Compiles `.pglsl` source files to `.psb` binary blobs at build time. The ESP32-S3 can
`#include` the blob as a C array and upload it via `CreateShaderProgram()`.

```bash
# Compile shader at build time
python tools/pglslc.py shaders/chromatic.pglsl -o shaders/chromatic.psb

# In ESP32 code:
#include "shaders/chromatic_psb.h"  // generated: static const uint8_t chromatic_psb[] = {...};
pglDevice.GetEncoder().CreateShaderProgram(1, chromatic_psb, sizeof(chromatic_psb));
```

---

## 8. Shader Library (Built-In Effects)

Ship a library of pre-written PGLSL shaders that replicate (and extend) the current
built-in effects, demonstrating the language:

| Shader | Description | Instructions (est.) |
|---|---|---|
| `brightness.pglsl` | Additive brightness adjustment | ~8 |
| `contrast.pglsl` | Contrast around midpoint | ~12 |
| `gamma.pglsl` | Power-curve gamma correction | ~10 |
| `invert.pglsl` | Colour inversion | ~6 |
| `threshold.pglsl` | Binary B&W threshold | ~10 |
| `edge_feather.pglsl` | Dim edge pixels (4-sample neighbour test) | ~30 |
| `edge_detect.pglsl` | Sobel edge detection (3×3 kernel) | ~60 |
| `chromatic_ab.pglsl` | Chromatic aberration (radial RGB split) | ~40 |
| `vignette.pglsl` | Radial darkening toward edges | ~15 |
| `scanlines.pglsl` | CRT-style horizontal scanlines | ~12 |
| `pixelate.pglsl` | Downsample to larger virtual pixels | ~15 |
| `hue_shift.pglsl` | Rotate hue by uniform amount | ~25 |
| `saturation.pglsl` | Adjust colour saturation | ~15 |
| `film_grain.pglsl` | Pseudo-random noise overlay (hash-based) | ~20 |
| `barrel_distort.pglsl` | Barrel/pincushion lens distortion | ~35 |
| `color_palette.pglsl` | Snap to N-colour palette (dithered) | ~30 |

---

## 9. Implementation Milestones

### M9: Programmable Shader System (3 weeks)

| Week | Day | Task | Deliverable |
|---|---|---|---|
| **W1** | Mon | Define PSB binary format + instruction set (this document) | `PglShaderBytecode.h` — opcodes, program header struct |
| | Tue | Implement `PglShaderVM::Execute()` — core interpreter loop with all arithmetic + math opcodes | VM executes ADD/MUL/SIN/COS etc. on test data |
| | Wed | Add texture sampling (`TEX2D`), geometric ops (`DOT`, `LEN`, `NORM`), interpolation (`MIX`, `CLAMP`, `STEP`) | Full instruction set functional |
| | Thu | Add `ShaderProgram` to `scene_state.h`. Wire `CMD_CREATE/DESTROY_SHADER_PROGRAM` into `command_parser.cpp` | Programs loaded via SPI commands |
| | Fri | Integrate VM into `screenspace_effects.cpp` as `PGL_SHADER_PROGRAM` class. Test with hand-assembled bytecode. | Hand-crafted brightness shader renders correctly |
| **W2** | Mon | Implement PGLSL lexer (tokenizer) in `PglShaderCompiler.h` | Tokenizes sample shaders correctly |
| | Tue | Implement PGLSL parser (AST) — expressions, declarations, assignments, swizzles | AST built for sample shaders |
| | Wed | Implement type checker + register allocator (linear scan) | Types resolved, registers assigned |
| | Thu | Implement code generator (AST → PSB bytecode) | End-to-end: PGLSL source → bytecode blob |
| | Fri | Add `PglEncoder` methods for new opcodes. Add `PglShaderCompiler::Compile()` static API. | Host can compile + upload shaders |
| **W3** | Mon | Write 4 core shader library effects: brightness, contrast, chromatic_ab, vignette | `.pglsl` sources + pre-compiled `.psb` blobs |
| | Tue | Write 4 more: edge_detect, scanlines, hue_shift, film_grain | 8 total library shaders |
| | Wed | Write offline compiler tool (`tools/pglslc.py`) | Build-time shader compilation |
| | Thu | Benchmark all library shaders on RP2350 (DWT timing). Optimize VM hot path. | Performance table matches §6.6 estimates |
| | Fri | Add `PGL_CAP_SHADER_VM` capability flag. Update `ProtoGL_API_Spec.md` §4 + §8. Wire `CMD_SET_SHADER_UNIFORM` into command parser. | v0.6 spec draft complete |

### Exit Criteria (M9)
- User can write a PGLSL shader, compile it (offline or runtime), upload bytecode, bind to a camera slot, set uniforms per-frame, and see the effect on the HUB75 panel.
- All 8+ library shaders render correctly and within performance budget.
- Built-in shaders (CONVOLUTION, DISPLACEMENT, COLOR_ADJUST) still work unchanged.
- VM execution of a 40-instruction shader takes < 0.05 ms on 128×64 @ 150 MHz.

---

## 10. Future Extensions (Post-M9)

| Extension | Description | Effort |
|---|---|---|
| **Multi-pass programs** | Single `.pglsl` with multiple `pass { }` blocks. Each pass reads previous pass output. | 1 week |
| **Compute shaders** | Non-pixel shaders that run once per frame (update uniforms, generate LUTs). | 1 week |
| **Shader hot-reload** | I2C command to re-upload a program without frame disruption. | 2 days |
| **LUT textures** | Additional `sampler2D` slots for lookup textures (color grading, noise). | 3 days |
| **Vertex shaders** | Per-vertex position/UV modification before rasterization. | 2 weeks |
| **Compile-time optimization** | Constant folding, dead code elimination, instruction scheduling. | 1 week |
| **SPIR-V subset** | Accept SPIR-V binary instead of PGLSL text (for Vulkan toolchain compat). | 2 weeks |

---

## 11. Quick Reference: Writing Your First Shader

```glsl
// File: my_vignette.pglsl
// Darkens edges in a radial falloff from center

uniform float u_strength;   // 0.0 = no effect, 1.0 = heavy vignette

void main() {
    vec2 uv = gl_FragCoord.xy / u_resolution;
    vec2 center = vec2(0.5, 0.5);
    float dist = length(uv - center) * 1.414;   // 0..1 from center to corner
    float vignette = mix(1.0, 1.0 - dist * dist, u_strength);

    vec4 color = texture2D(u_framebuffer, uv);
    gl_FragColor = vec4(color.rgb * vignette, 1.0);
}
```

**Usage on ESP32-S3:**
```cpp
// At boot / initialization:
auto result = PglShaderCompiler::Compile(vignetteSource, strlen(vignetteSource));
if (result.success) {
    encoder.CreateShaderProgram(0, result.bytecode, result.bytecodeSize);
    encoder.BindShaderProgram(PGL_CAMERA_0, 0, /*programId=*/0, /*intensity=*/1.0f);
}

// Every frame:
encoder.SetShaderUniform(0, /*slot=*/0, /*u_strength=*/0.7f);
```

This compiles to approximately 15 bytecode instructions (~60 bytes), executes in ~0.01 ms
for a 128×64 panel, and produces a smooth radial vignette effect.

---

## 12. Compilation Portability Policy

The PGLSL → PSB bytecode compiler (`PglShaderCompiler.h`) runs exclusively on the
**host MCU (ESP32-S3)** or on a **PC** during offline build. It **never runs on the GPU**.

| Location | Mechanism | When |
|---|---|---|
| **PC (offline)** | `tools/pglslc.py` invokes `PglShaderCompiler` | Build time — .pglsl → .psb binary blob |
| **Host MCU (runtime)** | `PglShaderCompiler::Compile()` on ESP32-S3 | One-time at boot or when uploading new effects |
| **GPU (RP2350)** | ❌ Never | GPU only runs pre-compiled bytecode via `PglShaderVM::Execute()` |

The compiler has **zero platform dependencies** — it requires only `<cstdint>`, `<cstring>`,
`<cstdio>`, `<cmath>`, and the platform-agnostic `PglShaderBytecode.h`. It can be compiled
as a standalone PC tool or linked into any Cortex-M or RISC-V host firmware.

---

## 13. PglShaderBackend — Platform-Portable Math Abstraction (M10)

All GPU shader and rendering code routes math operations through the
`PglShaderBackend` namespace (`lib/ProtoGL/src/PglShaderBackend.h`), which provides
compile-time dispatched implementations:

| Backend Variant | Selection Criteria | Math Strategy |
|---|---|---|
| `PGL_BACKEND_CM33_FPV5` | `__ARM_FEATURE_FMA` defined | `__builtin_fmaf`, `__builtin_sqrtf`, hardware FPU |
| `PGL_BACKEND_CM33_DSP` | `__ARM_FEATURE_DSP` defined | ARM DSP intrinsics for saturating integer ops |
| `PGL_BACKEND_SCALAR_FLOAT` | Default (any platform with `<cmath>`) | Standard C `sinf`, `cosf`, `sqrtf`, etc. |
| `PGL_BACKEND_SOFT_FLOAT` | No FPU, no `<cmath>` | Bhaskara sin, Schraudolph exp, Quake rsqrt |
| `PGL_BACKEND_SIMD_SSE` | Future — x86 SSE / ARM NEON / RISC-V V | Vectorised 4-wide float operations |

**API categories:**
- Arithmetic: `Add`, `Sub`, `Mul`, `Div`, `Fma`, `Neg`
- Math functions: `Sin`, `Cos`, `Tan`, `Asin`, `Acos`, `Atan`, `Atan2`, `Pow`, `Exp`, `Log`, `Sqrt`, `Rsqrt`
- Rounding/value: `Abs`, `Sign`, `Floor`, `Ceil`, `Fract`, `Mod`
- Clamping/interpolation: `Min`, `Max`, `Clamp`, `Mix`, `Step`, `Smoothstep`
- Geometric: `Dot2`, `Dot3`, `Len2`, `Len3`, `Norm2`, `Norm3`, `Cross`, `Dist2`
- Texture/pixel: `UnpackRGB565`, `PackRGB565`, `PackRGB565i`, `TexSample`, `R5`, `G6`, `B5`, `Clamp5`, `Clamp6`

All functions are `static inline` in a namespace — zero virtual-call overhead, resolved entirely at compile time.

**Files using PglShaderBackend:**
- `pgl_shader_vm.cpp` — all VM interpreter opcodes dispatch through `BE::`
- `screenspace_effects.cpp` — pixel pack/unpack, trig, blur kernels
- Rasterizer — ARM DSP intrinsics behind backend guard (future)

---

## 14. PglJobScheduler — General Job Scheduler (M10)

A platform-abstract job scheduler for parallel work dispatch.

**Interface** (`lib/ProtoGL/src/PglJobScheduler.h`):
```cpp
struct PglJob {
    void (*func)(void* ctx);
    void* ctx;
};

class PglJobScheduler {
public:
    virtual ~PglJobScheduler() = default;
    virtual uint8_t WorkerCount() const = 0;
    virtual void Submit(const PglJob* jobs, uint8_t count) = 0;
    virtual void WaitAll(void (*idleFunc)() = nullptr) = 0;
};
```

**Platform implementations:**

| Implementation | File | Strategy |
|---|---|---|
| `PglJobScheduler_RP2350` | `firmwares/rp2350/src/scheduler/` | Multicore FIFO — sends last job to Core 1, runs rest on Core 0 |
| `PglJobScheduler_SingleCore` | `lib/ProtoGL/src/PglJobScheduler_SingleCore.h` | Serial fallback — executes all jobs inline |
| `PglJobScheduler_FreeRTOS` | Future | FreeRTOS task + semaphore dispatch |

**Usage in gpu_core.cpp:**
```cpp
RasterJobCtx top  = {&rasterizer, backBuffer, 0, halfH, PerfStage::RasterTop};
RasterJobCtx bot  = {&rasterizer, backBuffer, halfH, H, PerfStage::RasterBottom};
PglJob jobs[2] = {{RasterJobFunc, &top}, {RasterJobFunc, &bot}};
scheduler.Submit(jobs, 2);
scheduler.WaitAll(Hub75Driver::PollRefresh);
```

This replaces the previous hard-coded `FIFO_CMD_START_RENDER` / `FIFO_CMD_RENDER_DONE`
protocol, making the rasterization dispatch platform-portable.
