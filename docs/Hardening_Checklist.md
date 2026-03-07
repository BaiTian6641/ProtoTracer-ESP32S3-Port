# ProtoGL Hardening Checklist — Production-Grade MCU/GPU Graphics Library

> **Purpose:** Actionable checklist of gaps, robustness improvements, and missing
> features identified during the full architectural review (2026-03-07).
> Each item targets making ProtoGL a solid, production-quality graphics library
> suitable for microcontrollers, simple MPUs, and future custom GPU devices on FPGA.
>
> **Scope covers:** Baremetal GPU firmware, host encoder library, wire protocol,
> and portability to non-RP2350 targets (RISC-V, FPGA, Cortex-M7, ESP32-P4).

---

## How to Use This Checklist

- **Priority:** Critical > High > Medium > Low
- **Effort:** Estimated implementation time for one developer
- **Target:** Which milestone or sprint the item should land in
- **Status:** `[ ]` not started, `[~]` in progress, `[x]` complete
- Items are grouped by subsystem. Cross-cutting items appear under "System-Wide."

---

## 1. Fault Tolerance & Error Recovery

### 1.1 GPU Error State Machine

- [ ] **Define `PglGpuState` enum** — `BOOT`, `IDLE`, `RENDERING`, `ERROR`, `RECOVERY`, `SLEEP`
  - Priority: **Critical** · Effort: 1 day · Target: Pre-M11
  - State transitions triggered by: command parse success/failure, CRC error,
    OOM, watchdog timeout, thermal emergency, host reset command

- [ ] **Define `PglErrorCode` enum** — Exhaustive error codes:
  - Priority: **Critical** · Effort: 0.5 day · Target: Pre-M11
  - `PGL_ERR_NONE`, `PGL_ERR_CRC_MISMATCH`, `PGL_ERR_UNKNOWN_OPCODE`,
    `PGL_ERR_PAYLOAD_TRUNCATED`, `PGL_ERR_RESOURCE_TABLE_FULL`,
    `PGL_ERR_OOM_SRAM`, `PGL_ERR_OOM_VRAM`, `PGL_ERR_SHADER_INVALID`,
    `PGL_ERR_DMA_TIMEOUT`, `PGL_ERR_THERMAL_SHUTDOWN`,
    `PGL_ERR_WATCHDOG_RESET`, `PGL_ERR_HARDFAULT`, `PGL_ERR_STACK_OVERFLOW`

- [ ] **Add `PGL_REG_ERROR_STATE` I2C register (read)** — Returns current `PglGpuState` + last `PglErrorCode` + error count
  - Priority: **Critical** · Effort: 0.5 day · Target: Pre-M11
  - Suggested address: `0x12` (verify no conflict with existing register map)

- [ ] **Assert IRQ pin on error** — GPU asserts IRQ (GPIO 13) when entering ERROR state;
      host polls error register via I2C or SPI read to retrieve details
  - Priority: **Critical** · Effort: 0.5 day · Target: Pre-M11

- [ ] **Implement error recovery policy per error type:**
  - Priority: **Critical** · Effort: 2 days · Target: Pre-M11

  | Error | Recovery Action |
  |---|---|
  | CRC mismatch | Drop frame, render last good frame, increment `droppedFrames`, stay in RENDERING |
  | Unknown opcode | Skip command (advance by header-declared length), log warning, continue parse |
  | Payload truncated | Drop rest of frame, render last good, enter RECOVERY for 1 frame |
  | Resource table full | NACK the create command, set error register, continue |
  | OOM (SRAM) | Attempt eviction of lowest-priority cached resource; if still OOM, NACK command |
  | OOM (VRAM) | Attempt tier demotion; if still OOM, NACK command |
  | Shader bytecode invalid | Reject program, set PGL_ERR_SHADER_INVALID, continue |
  | DMA timeout (>10 ms) | Reset DMA channel, re-init PIO SM, enter RECOVERY |
  | Thermal shutdown | Enter SLEEP, assert IRQ, wait for temp < recovery threshold |
  | Watchdog | Hardware reset → BOOT → auto-restore persisted resources |
  | HardFault | Capture fault address in flash, hardware reset → BOOT |

- [ ] **Add `CMD_RESET_GPU` (soft reset) command** — Host can force GPU back to IDLE state,
      reinitialize all subsystems without power cycle
  - Priority: **Critical** · Effort: 1 day · Target: Pre-M11
  - Suggested opcode: `0x0F` (control plane)
  - Clears all resource tables, resets display drivers, flushes DMA, restarts Core 1

- [ ] **Host-side `PglDevice::GetLastError()` / `ResetGpu()` methods**
  - Priority: **Critical** · Effort: 0.5 day · Target: Pre-M11

- [ ] **Document error state machine in Communication_Protocol.md**
  - Priority: **Critical** · Effort: 0.5 day · Target: Pre-M11

---

## 2. Baremetal Firmware Robustness

### 2.1 Watchdog Timer

- [ ] **Enable RP2350 hardware watchdog** — kick interval = 500 ms, reset on timeout
  - Priority: **High** · Effort: 0.5 day · Target: Pre-M11
  - Kick in the main render loop (`GpuCore::Core0Main`) after each `SwapBuffers()`
  - If render loop hangs (e.g., infinite loop in shader VM), watchdog resets the chip
  - On reset: boot detects watchdog reset source → write `PGL_ERR_WATCHDOG_RESET` to
    flash scratch area → auto-restore persisted resources → report to host via I2C

- [ ] **Core 1 watchdog heartbeat** — Core 0 monitors Core 1 liveness via shared atomic counter;
      if Core 1 fails to increment within 100 ms, Core 0 re-launches Core 1
  - Priority: **High** · Effort: 1 day · Target: Pre-M11

### 2.2 ARM Cortex-M33 Fault Handlers

- [ ] **Install HardFault handler** — Capture CFSR, HFSR, MMFAR, BFAR, LR, PC → write to
      reserved flash area (256 bytes) → trigger watchdog reset
  - Priority: **High** · Effort: 1 day · Target: Pre-M11

- [ ] **Install MemManage fault handler** — Detect stack overflow (MPU guard region) or
      invalid memory access → log + reset
  - Priority: **High** · Effort: 0.5 day · Target: Pre-M11

- [ ] **Install BusFault handler** — Catch PIO/DMA bus errors (e.g., accessing unmapped
      external memory address)
  - Priority: **High** · Effort: 0.5 day · Target: Pre-M11

- [ ] **Install UsageFault handler** — Catch unaligned access, undefined instruction (protects
      against corrupted shader bytecode jumping to invalid addresses)
  - Priority: **High** · Effort: 0.5 day · Target: Pre-M11

- [ ] **MPU stack guard region** — Configure Cortex-M33 MPU to mark 256 bytes below each core's
      stack as no-access; triggers MemManage fault on stack overflow instead of silent corruption
  - Priority: **High** · Effort: 0.5 day · Target: Pre-M11

- [ ] **Post-reset fault reporting** — After watchdog/fault reset, read crash info from flash
      scratch → report via I2C `PGL_REG_FAULT_INFO` register → host logs for debugging
  - Priority: **Medium** · Effort: 1 day · Target: M11

### 2.3 RISC-V / FPGA Fault Equivalents

- [ ] **Document RISC-V trap handler requirements** — `mcause` / `mepc` / `mtval` capture for
      illegal instruction, load/store access fault, load/store address misaligned
  - Priority: **Medium** · Effort: 0.5 day · Target: Phase 2 planning
  - Needed for: Hazard3 RISC-V mode, custom RISC-V cores, FPGA soft-CPUs

- [ ] **Define FPGA-specific fault signals** — Bus timeout, PIO equivalent error flags, memory
      ECC errors (if using error-correcting SRAM on FPGA)
  - Priority: **Low** · Effort: 1 day · Target: Phase 2

---

## 3. Interrupt Architecture

### 3.1 Interrupt Priority Map

- [ ] **Define and document NVIC priority levels** for all ISRs on RP2350:
  - Priority: **High** · Effort: 1 day · Target: Pre-M11

  | Priority | ISR | Rationale |
  |---|---|---|
  | 0 (highest) | HUB75 DMA completion (PIO0) | Display refresh — visible flicker if delayed >50 µs |
  | 0 | DVI TMDS line IRQ (PIO2) | Scanline-critical — TMDS underrun causes monitor sync loss |
  | 1 | Octal SPI RX DMA (PIO1) | High-bandwidth data ingress — ring buffer overflow if delayed |
  | 2 | QSPI VRAM DMA completion | Background prefetch — can tolerate 100+ µs delay |
  | 2 | SPI LCD DMA completion | Display push — self-buffered display tolerates delay |
  | 3 | I2C slave IRQ | Management plane — low frequency, high latency tolerance |
  | 3 | Watchdog early warning (if available) | Diagnostic only |
  | 4 (lowest) | Timer / profiling | Non-critical telemetry |

- [ ] **Set priorities in firmware initialization** — `NVIC_SetPriority()` calls in
      `GpuCore::Initialize()` before enabling any peripherals
  - Priority: **High** · Effort: 0.5 day · Target: Pre-M11

- [ ] **Document ISR execution time budgets:**
  - Priority: **Medium** · Effort: 0.5 day · Target: M11
  - HUB75 DMA ISR: < 5 µs (just restart next BCM plane)
  - SPI RX DMA ISR: < 2 µs (advance ring buffer pointer)
  - I2C slave ISR: < 10 µs (register read/write, no blocking)
  - No ISR may call `flash_range_program()` or block on FIFO

### 3.2 FPGA / Custom-SoC Interrupt Portability

- [ ] **Define abstract `PglInterruptController` interface** — priority levels, enable/disable,
      pending query — so FPGA and non-ARM targets can map to their native interrupt system
  - Priority: **Low** · Effort: 2 days · Target: Phase 2
  - For RISC-V: maps to PLIC/CLIC priority levels
  - For FPGA: maps to custom IRQ controller register map

---

## 4. Resolution Scalability

### 4.1 Framebuffer Memory Strategy

- [ ] **Document resolution scaling strategy** in GPU_API_Design.md:
  - Priority: **High** · Effort: 1 day · Target: M11

  | Resolution | FB+Z SRAM | Strategy |
  |---|---|---|
  | 128×64 (current) | 64 KB | Fully SRAM-resident (current behavior) |
  | 160×120 | 75 KB | SRAM-resident, tight but feasible |
  | 240×240 | 225 KB | **External-memory framebuffer required** |
  | 320×240 | 300 KB | External-memory FB + scanline render |
  | 640×480 (DVI) | 1.2 MB | Tile-render-to-external + DMA scanout |

- [ ] **Implement 16-bit fixed-point Z-buffer option** — Halves Z-buffer SRAM from 4 bytes/pixel
      to 2 bytes/pixel; accuracy sufficient for scenes with < 100 depth layers
  - Priority: **High** · Effort: 2 days · Target: M12
  - Compile-time switch: `PGL_Z_BUFFER_16BIT`
  - Saves 16 KB at 128×64; saves 150 KB at 320×240

- [ ] **Implement external-memory framebuffer path** — Framebuffer lives in QSPI VRAM (Tier 1);
      tile scheduler renders 16×16 tiles into a SRAM staging tile, then DMA-copies to external FB
  - Priority: **High** · Effort: 1 week · Target: M12-M13
  - Enables resolutions up to 640×480 without SRAM pressure
  - Display driver reads from external FB for scanout (DMA chained)

- [ ] **Implement scanline rendering for self-buffered displays** — Render one scanline at a time
      into a 1-line SRAM buffer → push to GDDRAM via SPI DMA → repeat
  - Priority: **Medium** · Effort: 3 days · Target: M13
  - SRAM cost: ~640 bytes for 320×1 RGB565
  - Throughput: limited by SPI DMA speed, acceptable for 30 FPS on ST7789

- [ ] **Add `PGL_CAP_MAX_RESOLUTION` to capability query** — GPU reports maximum supported
      resolution based on available memory (SRAM + detected VRAM)
  - Priority: **Medium** · Effort: 0.5 day · Target: M11

### 4.2 QuadTree Scaling

- [ ] **Make QuadTree node pool size configurable** based on resolution — more nodes needed for
      larger framebuffers (currently 1024 entities); formula: `maxEntities = (W × H) / 8`
  - Priority: **Medium** · Effort: 1 day · Target: M12

- [ ] **Evaluate hierarchical tile sizes for large resolutions** — 16×16 tiles at 640×480 = 1200
      tiles (too many); consider 32×32 tiles (300 tiles) or adaptive tile size
  - Priority: **Medium** · Effort: 2 days · Target: M12-M13

---

## 5. Concurrency & Thread Safety

### 5.1 Concurrency Model Documentation

- [ ] **Add §"Concurrency Model" to GPU_API_Design.md** defining:
  - Priority: **Medium** · Effort: 1 day · Target: M11

  | Data Structure | Ownership | Access Pattern |
  |---|---|---|
  | SceneState (resource tables) | Core 0 exclusive | Written during parse, read-only during raster |
  | QuadTree | Core 0 builds | Read-only during raster (both cores) |
  | Triangle pool / vertex buffer | Core 0 builds | Read-only during raster |
  | Framebuffer (back) | Per-tile exclusive | Each tile written by at most one core |
  | Z-buffer | Per-tile exclusive | Same tile exclusion as framebuffer |
  | Tile counter (`nextTile`) | Shared | `__atomic_fetch_add` (RELAXED) |
  | `coresFinished` counter | Shared | `__atomic_fetch_add` (RELEASE) + load (ACQUIRE) |
  | HUB75 front buffer | Display driver | Swapped atomically after both cores finish |
  | FIFO mailbox | Kernel primitive | Pico-SDK multicore FIFO (hardware synchronized) |
  | I2C register file | ISR + Core 0 | Volatile reads; ISR writes atomically within register |
  | MemTierManager cache | Core 0 exclusive | Accessed only between frames (never during raster) |

### 5.2 Multi-Core Scaling (>2 Cores)

- [ ] **Document scaling strategy for 4+ core targets** (ESP32-P4, FPGA multi-hart):
  - Priority: **Medium** · Effort: 1 day · Target: Phase 2 planning
  - Tile scheduler already supports N workers (atomic counter is core-count-agnostic)
  - `PglJobScheduler_FreeRTOS` implementation sketch for ESP32-P4
  - FPGA: one render core + one display core + N shader cores (pipeline parallelism)

- [ ] **Define memory ordering requirements** — Current code uses `__ATOMIC_RELAXED` for tile
      counter (safe because tiles are independent); document when `ACQUIRE`/`RELEASE` are needed
  - Priority: **Medium** · Effort: 0.5 day · Target: M11

### 5.3 Reentrant Safety

- [ ] **Audit all `static` mutable state in GPU firmware** — Static variables in ISRs or shared
      namespaces are potential reentrancy hazards when called from multiple cores
  - Priority: **Medium** · Effort: 1 day · Target: M11
  - Known: `PglTileScheduler::passCtx_` (safe — set by Core 0 before dispatch)
  - Known: `Hub75Driver` namespace globals (safe — only called from Core 0 or ISR, never Core 1)

---

## 6. Shader System Hardening

### 6.1 Bytecode Validation

- [ ] **Validate all PSB bytecode on upload** (`HandleCreateShaderProgram`):
  - Priority: **High** · Effort: 1 day · Target: Pre-M11

  | Check | Description |
  |---|---|
  | Magic | `header.magic == PSB_MAGIC (0x50534231)` |
  | Version | `header.version <= PSB_VERSION_CURRENT` |
  | Instruction count | `header.instrCount <= PGL_MAX_SHADER_INSTRUCTIONS (256)` |
  | Uniform count | `header.uniformCount <= 16` |
  | Constant count | `header.constCount <= 32` |
  | Operand bounds | Every instruction's src/dst register index ∈ [0, 31] |
  | Opcode validity | Every instruction's opcode ∈ known opcode set (no undefined) |
  | Termination | Last instruction must be `PSB_OP_END` |
  | Size check | Total blob size == sizeof(header) + uniforms + constants + instructions |

  Reject program and set `PGL_ERR_SHADER_INVALID` if any check fails.

- [ ] **Add VM execution cycle limit** — If a shader exceeds 10,000 cycles per pixel (pathological
      case), abort execution and output fallback color (black or last pixel)
  - Priority: **Medium** · Effort: 0.5 day · Target: M11
  - Prevents shader bugs from hanging the GPU (each pixel gets a cycle budget)

### 6.2 Shader Debugging Support

- [ ] **Add `PGL_REG_SHADER_DEBUG` I2C register** — Reports: last executed program ID,
      instruction pointer at abort (if cycle limit hit), register dump on error
  - Priority: **Low** · Effort: 1 day · Target: M12

- [ ] **Add `CMD_DEBUG_SHADER` command** — Execute shader on a single pixel, return register
      file contents via SPI read (for host-side shader debugger)
  - Priority: **Low** · Effort: 2 days · Target: M13

### 6.3 Vertex Shader Support (Future)

- [ ] **Design vertex shader stage** — PSB programs that run per-vertex before projection:
  - Priority: **Low** · Effort: 2 weeks · Target: Phase 2
  - Input: object-space position, normal, UV, custom attributes
  - Output: clip-space position, transformed normal, modified UV
  - Wire command: `CMD_BIND_VERTEX_SHADER (0x88)` per draw call
  - Enables: skeletal animation on GPU, wave/wind deformation, billboarding without host reupload
  - SRAM impact: one set of output registers per vertex (8 floats × max vertices)

### 6.4 Control Flow Extension (Future)

- [ ] **Design limited control flow for PGLSL** — `if`/`else` (no loops) to enable conditional
      effects (e.g., discard pixel, branch on uniform threshold)
  - Priority: **Low** · Effort: 1 week · Target: Phase 2
  - New opcodes: `PSB_OP_JZ` (jump if zero), `PSB_OP_JNZ`, `PSB_OP_CMP`
  - No backward jumps (guarantees termination without loop analysis)
  - Flat `if/else` only — no nesting beyond depth 2

---

## 7. Power Management

### 7.1 GPU Power States

- [ ] **Define power state machine:**
  - Priority: **Medium** · Effort: 2 days · Target: M12

  ```
  BOOT ──► ACTIVE ──► IDLE ──► STANDBY ──► SLEEP
                ▲        │         │          │
                └────────┴─────────┴──────────┘  (wake on SPI CS / I2C / IRQ)
  ```

  | State | Clock | PIO | Display | SRAM | Wake Latency |
  |---|---|---|---|---|---|
  | ACTIVE | Full (150-300 MHz) | All running | Refreshing | Retained | — |
  | IDLE | Reduced (75 MHz) | Display only | Refreshing | Retained | < 1 µs |
  | STANDBY | Stopped | All stopped | Off (GDDRAM retained) | Retained | < 100 µs |
  | SLEEP | Off (dormant) | Off | Off | Retained (if SRAM domain powered) | < 5 ms |

- [ ] **Add `CMD_SET_POWER_MODE` command** (opcode `0x0E`) — Host requests power state transition
  - Priority: **Medium** · Effort: 1 day · Target: M12

- [ ] **Add `PGL_REG_POWER_STATE` I2C register** (address `0x13`) — Read current power state
  - Priority: **Medium** · Effort: 0.5 day · Target: M12

- [ ] **Auto-idle timer** — GPU transitions ACTIVE→IDLE after N ms with no new command buffer
      (configurable, default 200 ms). Transition IDLE→ACTIVE on next SPI CS assertion.
  - Priority: **Medium** · Effort: 1 day · Target: M12

- [ ] **Display-specific power commands** — `CMD_DISPLAY_SLEEP` / `CMD_DISPLAY_WAKE` for
      OLED display-off mode, LCD backlight PWM control, HUB75 OE blanking
  - Priority: **Low** · Effort: 1 day · Target: M13

### 7.2 FPGA Power Considerations

- [ ] **Document FPGA clock gating strategy** — Gate render pipeline clock when idle, keep only
      SPI slave and I2C slave clock active
  - Priority: **Low** · Effort: 0.5 day · Target: Phase 2
  - For custom RISC-V on FPGA: WFI instruction gates core clock via hardware signal

---

## 8. Testing & Validation

### 8.1 Test Framework

- [ ] **Select test framework** — Recommend Unity (C) or Catch2 (C++) for both host and GPU
  - Priority: **Medium** · Effort: 0.5 day · Target: M11

- [ ] **Set up desktop build target** — Compile ProtoGL host library + GPU renderer (using
      `PglShaderBackend_ScalarFloat` + `PglJobScheduler_SingleCore`) on x86/ARM64 desktop
  - Priority: **Medium** · Effort: 2 days · Target: M11
  - Enables: unit tests, golden-image tests, fuzz tests — all without hardware

### 8.2 Host-Side Tests

- [ ] **PglEncoder unit tests** — Encode every command type → verify wire bytes match spec
  - Priority: **Medium** · Effort: 2 days · Target: M11

- [ ] **PglParser unit tests** — Feed known byte buffers → verify parsed structs match
  - Priority: **Medium** · Effort: 2 days · Target: M11

- [ ] **PglShaderCompiler unit tests** — Compile every library shader → verify bytecode
      instruction count, operand bounds, round-trip encode/decode
  - Priority: **Medium** · Effort: 1 day · Target: M11

- [ ] **CRC-16 edge cases** — Empty buffer, single byte, max frame size (32 KB), bit-flip injection
  - Priority: **Medium** · Effort: 0.5 day · Target: M11

### 8.3 GPU-Side Tests

- [ ] **Golden-image test harness** — Render known scene → compare framebuffer hash (FNV-1a) against
      expected value; run on desktop build and on hardware
  - Priority: **Medium** · Effort: 3 days · Target: M11
  - Test scenes: single triangle, cube, textured quad, 5 material types, 2D overlay,
    compositor blend modes, shader effects

- [ ] **Memory pool stress test** — 10,000× alloc/free cycle with random sizes; verify zero
      fragmentation, zero leaks, all free blocks coalesced
  - Priority: **Medium** · Effort: 1 day · Target: M11

- [ ] **Tier migration stress test** — Rapidly promote/demote 50 resources; verify data integrity
      (CRC-32 before/after migration), no stale pointers
  - Priority: **Medium** · Effort: 1 day · Target: M11-M12

### 8.4 Wire-Format Fuzz Testing

- [ ] **Command parser fuzz harness** — Feed random/mutated byte streams into `command_parser.cpp`;
      must never crash, must never access out-of-bounds memory
  - Priority: **High** · Effort: 2 days · Target: Pre-M11
  - Use desktop build + AddressSanitizer (ASan) + UndefinedBehaviorSanitizer (UBSan)
  - Critical for security if the GPU ever receives command buffers from untrusted sources

- [ ] **PSB bytecode fuzzer** — Feed random bytecode blobs into `HandleCreateShaderProgram` +
      `PglShaderVM::Execute`; must reject invalid programs, must not execute invalid opcodes
  - Priority: **High** · Effort: 1 day · Target: Pre-M11

### 8.5 Hardware Integration Tests

- [ ] **Automated SPI loopback test** — ESP32-S3 sends known pattern → RP2350 echoes back via
      SPI read → host verifies byte-for-byte match at 40/60/80 MHz
  - Priority: **High** · Effort: 1 day · Target: M11 (during HW bring-up)

- [ ] **24-hour stability test** — `ProtogenHUB75Animation` running continuously; log: FPS
      (min/max/avg), dropped frames, GPU temperature, SRAM free, error count
  - Priority: **High** · Effort: 1 day · Target: M11 exit criteria

- [ ] **Power-cycle persistence test** — Persist 10 resources → power cycle → verify auto-restore
      (resource data matches, no corruption, restore time < 100 ms)
  - Priority: **Medium** · Effort: 1 day · Target: M12

---

## 9. Wire Protocol Hardening

### 9.1 Command Validation

- [ ] **Bounds-check all resource IDs in every command handler** — Mesh ID < `PGL_MAX_MESHES`,
      material ID < `PGL_MAX_MATERIALS`, etc. Currently assumed but not systematically verified.
  - Priority: **High** · Effort: 1 day · Target: Pre-M11

- [ ] **Add `payloadSize` validation** — Compare declared payload size in command header against
      expected size for each opcode; reject if mismatch
  - Priority: **High** · Effort: 0.5 day · Target: Pre-M11

- [ ] **Add frame sequence number validation** — Detect dropped or reordered frames; log warning
      if `frameNum` is not monotonically increasing
  - Priority: **Medium** · Effort: 0.5 day · Target: M11

### 9.2 Protocol Versioning

- [ ] **Add wire protocol version to frame header** — 1-byte version field so GPU can detect
      host/GPU version mismatch and report via I2C error register
  - Priority: **Medium** · Effort: 0.5 day · Target: M11
  - GPU rejects frames with `protocolVersion > PGL_PROTOCOL_VERSION_MAX` (forward compat)
  - GPU accepts frames with `protocolVersion >= PGL_PROTOCOL_VERSION_MIN` (backward compat)

- [ ] **Add `PGL_CAP_PROTOCOL_VERSION` to capability response** — Host reads supported protocol
      version range at boot to verify compatibility
  - Priority: **Medium** · Effort: 0.5 day · Target: M11

### 9.3 Secure Command Buffer (Future — If Receiving External Commands)

- [ ] **Optional HMAC-SHA256 frame authentication** — Prevent malicious command injection if
      GPU is connected to untrusted host or receiving commands over a network
  - Priority: **Low** · Effort: 1 week · Target: Phase 2 / FPGA
  - Shared secret provisioned at flash time
  - Only needed if ProtoGL is used in multi-tenant or networked scenarios

---

## 10. Display & Rendering Quality

### 10.1 Texture Filtering

- [ ] **Implement bilinear texture filtering** — 4 texel reads + 2D lerp per pixel
  - Priority: **Low** · Effort: 2 days · Target: M12-M13
  - Per-material flag: `PGL_MAT_FLAG_BILINEAR` (bit in material params)
  - Adds ~20 cycles/pixel; worthwhile for scaled sprites and UI elements

- [ ] **Implement color-dithered output** — Ordered dithering (Bayer 4×4) when converting
      from internal precision to RGB565; reduces visible banding on gradients
  - Priority: **Low** · Effort: 1 day · Target: M12
  - Particularly visible on SPI LCDs with gradient backgrounds

### 10.2 Perspective-Correct UV Interpolation

- [ ] **Implement 1/w hyperbolic UV interpolation** — Currently affine (acknowledged M4 audit);
      causes texture wobble on foreshortened surfaces
  - Priority: **Low** · Effort: 2 days · Target: M12
  - Cost: 1 division + 2 multiplies per pixel; offset by removing UV re-normalization
  - Per-mesh opt-in flag (low-poly meshes don't benefit)

### 10.3 Anti-Aliasing

- [ ] **Implement edge-aware FXAA post-process shader** — Ships as a PGLSL library shader;
      no firmware changes needed, just a well-tuned shader program
  - Priority: **Low** · Effort: 2 days · Target: M12 (shader library addition)

---

## 11. FPGA / Custom GPU Portability

### 11.1 Hardware Abstraction Layer

- [ ] **Define `PglHal` abstraction** — Platform-specific primitives behind a compile-time interface
      (not virtual — `#if` / template specialization, like PglShaderBackend):
  - Priority: **Medium** · Effort: 1 week · Target: Phase 2 planning

  | HAL Function | RP2350 Impl | FPGA Impl | STM32H7 Impl |
  |---|---|---|---|
  | `Hal::ReadCycles()` | DWT->CYCCNT | custom CSR | DWT->CYCCNT |
  | `Hal::DmaMemcpy()` | RP2350 DMA | AXI DMA | STM32 DMA2 |
  | `Hal::DmaFill()` | RP2350 DMA const-fill | AXI DMA fill | STM32 DMA2 |
  | `Hal::AtomicFetchAdd()` | `__atomic_fetch_add` | RISC-V AMO or mutex | `__atomic_fetch_add` |
  | `Hal::FlashWrite()` | `flash_range_program` | SPI flash IP | STM32 Flash API |
  | `Hal::EnterCritical()` | `__disable_irq()` | `csrci mstatus, 8` | `__disable_irq()` |
  | `Hal::Wfi()` | `__WFI()` | `wfi` | `__WFI()` |

- [ ] **Document PIO → FPGA mapping** — PIO state machines have no FPGA equivalent; provide
      design guidance for replacing PIO with HDL modules:
  - Priority: **Medium** · Effort: 1 day · Target: Phase 2

  | PIO Function | FPGA Replacement |
  |---|---|
  | HUB75 data shift + BCM | Shift register + timing FSM in Verilog |
  | Octal SPI slave | SPI slave IP with 8-bit data bus |
  | TMDS serializer | SERDES primitive or shift register |
  | QSPI VRAM driver | QSPI controller IP or PIO equivalent |

### 11.2 FPGA-Specific Enhancements

- [ ] **Hardware rasterizer pipeline design** — Document which GPU pipeline stages could be
      accelerated in hardware on FPGA:
  - Priority: **Low** · Effort: 2 days · Target: Phase 2

  | Stage | Software (CPU) | Hardware (FPGA) | Speedup |
  |---|---|---|---|
  | Vertex transform | Matrix multiply | Parallel 3×3 MACs | ~10× |
  | Perspective divide | FP division | Pipelined divider IP | ~5× |
  | Triangle setup | Edge function calc | 3× parallel multiply | ~3× |
  | Z-buffer test | Compare + branch | Single-cycle compare | ~2× |
  | Texture sampling | Memory read + lerp | Dual-port BRAM read | ~3× |
  | Pixel output | FB write | Direct BRAM port | ~1× |

- [ ] **Define FPGA memory map** — Framebuffer in block RAM, texture cache in block RAM,
      command FIFO in block RAM, external DRAM interface for large textures
  - Priority: **Low** · Effort: 1 day · Target: Phase 2

- [ ] **Define FPGA register-based command interface** — Alternative to SPI for on-chip GPU:
      memory-mapped registers for command submission (AXI-Lite or Wishbone slave)
  - Priority: **Low** · Effort: 2 days · Target: Phase 2

---

## 12. API Completeness

### 12.1 Missing Quality-of-Life Features

- [ ] **Implement `CMD_DRAW_ARC` (0xAD)** — Arc primitive (referenced in M12 schedule but
      missing from 2D_Graphics_And_Compositing.md wire format section)
  - Priority: **Low** · Effort: 1 day · Target: M12

- [ ] **Add scissor test (global clip rect)** — In addition to per-layer clip; applies to all
      rendering including 3D
  - Priority: **Low** · Effort: 1 day · Target: M12

- [ ] **Add render statistics query** — Per-frame: triangle count, pixel fill rate, shader
      execution time, compositor overhead, display push time
  - Priority: **Medium** · Effort: 1 day · Target: M11
  - Available via SPI read command or I2C extended status

- [ ] **Add `CMD_SET_GLOBAL_SHADER_UNIFORM`** — Set a uniform across ALL active shader programs
      in one command (e.g., time, frame count — avoids per-program uniform updates)
  - Priority: **Low** · Effort: 0.5 day · Target: M12

### 12.2 Resource Management Completeness

- [ ] **Implement resource reference counting** — Track how many draw calls reference each
      mesh/material/texture; warn (or error) on destroy while referenced
  - Priority: **Medium** · Effort: 2 days · Target: M11
  - Prevents use-after-free bugs from host-side resource lifecycle errors

- [ ] **Add `CMD_QUERY_RESOURCE_INFO`** — Host can query: resource type, size, current tier,
      access count, persistence state for any resource ID
  - Priority: **Low** · Effort: 1 day · Target: M12

### 12.3 Offline Tooling

- [ ] **Complete `pglslc.py` offline shader compiler** — Listed in M9 W24 Wed as ⬜
  - Priority: **Medium** · Effort: 2 days · Target: M11

- [ ] **Mesh / texture import tool** — Convert common formats (OBJ, PNG) into ProtoGL resource
      blobs with correct wire-format headers
  - Priority: **Low** · Effort: 1 week · Target: Post-M13

- [ ] **Scene exporter** — Export a full ProtoGL command buffer from a desktop tool (render
      preview + binary dump) for regression testing
  - Priority: **Low** · Effort: 1 week · Target: Post-M13

---

## 13. Documentation Gaps

- [ ] **Error Recovery Protocol** — New section in Communication_Protocol.md covering error state
      machine, IRQ assertion, error register, host recovery procedure
  - Priority: **Critical** · Effort: 0.5 day · Target: Pre-M11

- [ ] **Interrupt Priority Map** — New section in Communication_Protocol.md or GPU_API_Design.md
  - Priority: **High** · Effort: 0.5 day · Target: Pre-M11

- [ ] **Concurrency Model** — New section in GPU_API_Design.md
  - Priority: **Medium** · Effort: 0.5 day · Target: M11

- [ ] **Resolution Scaling Strategy** — New section in GPU_API_Design.md
  - Priority: **High** · Effort: 0.5 day · Target: M11

- [ ] **Power Management** — New section in GPU_API_Design.md
  - Priority: **Medium** · Effort: 0.5 day · Target: M12

- [ ] **FPGA Porting Guide** — New document (`docs/FPGA_Porting_Guide.md`) covering: HAL mapping,
      PIO→HDL replacement, memory map, interrupt mapping, build system
  - Priority: **Low** · Effort: 2 days · Target: Phase 2

- [ ] **Testing Strategy** — New document (`docs/Testing_Strategy.md`) covering: framework,
      test matrix, CI pipeline, golden images, fuzz testing
  - Priority: **Medium** · Effort: 1 day · Target: M11

---

## 14. HUD Display (GPU-Attached + Host Boot Fallback)

> **Context:** The diagnostic / status HUD should live on the **RP2350 GPU**, not the
> ESP32-S3 host. The GPU already has everything needed: `DisplayManager` supports up
> to 4 simultaneous displays, and an I2C OLED (SSD1306/SSD1309 128×64 mono) can be
> driven via the RP2350's **I2C1 peripheral** (2 GPIO pins, no PIO SMs, no DMA channel
> conflict with HUB75 on PIO0), and the
> 2D layer system (`Layer 2: 2D HUD Overlay`) can draw text, icons, and bars natively.
> GPU telemetry (temperature, FPS, VRAM) is **local** — no bus round-trip required.
>
> For host information (BLE state, WiFi RSSI, heap), the host pushes a compact status
> struct to the GPU via a new SPI command. The GPU renders both its own telemetry and
> the forwarded host data onto the HUD display.
>
> The host-side M5UnitGLASS2 (128×64 I2C OLED) remains available as a **boot-time
> display** (progress bars before the GPU is initialised) and as a **fallback** when no
> GPU is present. This matches the existing ESPMenu.h pattern but cleanly separates
> the concerns.
>
> **Why GPU-attached?**
> - GPU telemetry is local — zero bus overhead for FPS, temperature, VRAM
> - Mini framebuffer preview is trivial — GPU downscales its own render target
> - Leverages the existing `DisplayManager` + `I2cDisplayDriver` infrastructure
> - Leverages the existing 2D layer system for text / icons / bars
> - Frees up host I2C bus bandwidth (no contention with GPU management commands)
> - On future FPGA GPU, the HUD I2C port maps directly to an HDL I2C IP block

### 14.1 GPU-Attached HUD Display Driver

- [ ] **Register I2C HUD as secondary display** — At GPU boot, instantiate an
      `I2cDisplayDriver` for the HUD OLED (SSD1306 128×64 or SSD1309 128×64) on
      hardware I2C1 and register it with `DisplayManager` as display index 1.
      Assign it a dedicated render target (`renderTargetId = 1`, 128×64 MONO1)
      via `CMD_SET_DISPLAY_ROUTE` (0x91). The primary HUB75 panel
      remains display index 0 on PIO0 — no resource conflict.
  - Priority: **High** · Effort: 1 day · Target: M12
  - Leverages: `DisplayManager::RegisterDisplay()`, `I2cDisplayDriver`,
    I2C1 peripheral (I2C0 reserved for host management bus)

- [ ] **HUD pin allocation on RP2350** — Reserve I2C1 pins for the HUD display
      (SDA, SCL). Document pin assignment in the board configuration.
      I2C0 is used by the host management bus; I2C1 is dedicated to the HUD OLED.
      Add pin config to `GpuConfig`.
  - Priority: **High** · Effort: 0.5 day · Target: M12

- [ ] **HUD render target + 2D layer** — Create a small auxiliary render target
      (128×64 or 96×64) and bind a 2D layer to it. The GPU's 2D draw commands
      (`CMD_DRAW_RECT` 0xA1, `CMD_DRAW_TEXT` 0xA5, `CMD_DRAW_SPRITE` 0xA4) render
      HUD content into this target. On `SwapBuffers()`, `I2cDisplayDriver` pushes
      only dirty rectangles to the OLED's GDDRAM — self-refresh handles display.
  - Priority: **Medium** · Effort: 1 day · Target: M12

### 14.2 Host → GPU Status Forwarding

- [ ] **`CMD_SET_HOST_STATUS` (0x93)** — New SPI command for the host to push a
      compact status struct to the GPU once per frame (or at a lower cadence).
      The GPU stores the latest copy and draws it onto the HUD layer.
      ```
      struct PglCmdSetHostStatus {      // ~32 bytes
          uint32_t heapFreeBytes;       // esp_get_free_heap_size()
          uint32_t encodeTimeUs;        // Host encode duration this frame
          uint16_t droppedFramesHost;   // Host-side frame drops
          int8_t   wifiRssi;            // WiFi RSSI (0 = not connected)
          int8_t   bleRssi;             // BLE RSSI (0 = not connected)
          uint8_t  bleConnected : 1;    // BLE link active
          uint8_t  wifiConnected : 1;   // WiFi link active
          uint8_t  micEnabled : 1;      // Microphone enabled
          uint8_t  reserved : 5;
          uint8_t  faceExpression;      // Current face expression index
          uint8_t  brightness;          // Current brightness setting
          uint8_t  displayMode;         // Current display mode
          uint16_t stackHighWaterMark;  // Lowest free stack (words)
          uint8_t  padding[14];         // Pad to 32 bytes for alignment
      };
      ```
      The host calls `PglEncoder::SetHostStatus(...)` each frame (or every N frames).
      GPU parser stores the struct in a static `hostStatus_` in the command handler.
  - Priority: **High** · Effort: 1 day · Target: M12
  - Wire-format: Add to Communication_Protocol.md and ProtoGL_API_Spec.md

- [ ] **Host-side encoder method** — Add `PglEncoder::SetHostStatus(const PglHostStatus&)`
      that emits `CMD_SET_HOST_STATUS` into the command buffer. Call from
      `GPUDriverController::Display()` at the same cadence as existing diagnostics.
  - Priority: **High** · Effort: 0.5 day · Target: M12

### 14.3 HUD Content & Layout

- [ ] **Structured HUD layout zones** — Define a compile-time HUD layout for the
      GPU-attached display:
      - **Status bar** (top 12 px): BLE / WiFi / mic icons drawn via `CMD_DRAW_SPRITE`
      - **GPU info zone** (rows 12–38): FPS, GPU %, temperature, SRAM/VRAM usage
      - **Host info zone** (rows 38–52): heap free, encode time, frame drops
      - **Preview zone** (rows 52–64): mini framebuffer thumbnail (optional)
      Layout defined as a `HudLayout` struct in GPU firmware. Controllers can override
      zone positions via `GpuConfig`.
  - Priority: **Medium** · Effort: 1 day · Target: M12

- [ ] **GPU telemetry renderer** — A small GPU-side function that runs after
      `EndFrame()` compositing: reads local `diagnostics_` struct (already tracked:
      FPS, temperature, GPU%, core utilisation, VRAM) and emits 2D draw calls into
      the HUD render target. No bus overhead — all data is local.
  - Priority: **High** · Effort: 1 day · Target: M12

- [ ] **Host telemetry renderer** — Reads the cached `hostStatus_` struct (received
      via `CMD_SET_HOST_STATUS`) and renders host metrics into the HUD host-info zone.
      Displays: heap free, encode time, BLE/WiFi RSSI, dropped frames. Greyed out if
      no `CMD_SET_HOST_STATUS` received within last 2 seconds (stale data guard).
  - Priority: **Medium** · Effort: 0.5 day · Target: M12

- [ ] **Mini framebuffer preview** — GPU downscales its own primary render target
      (e.g., 128×64 → 64×12 thumbnail via box filter) and blits the result into the
      HUD preview zone. This is trivial on the GPU side — no SPI read path needed,
      no bus contention. Runs at ≤5 Hz to keep overhead low.
  - Priority: **Low** · Effort: 1 day · Target: M13

### 14.4 Icon & Asset System

- [ ] **HUD icon atlas on GPU** — Upload a small icon atlas texture (e.g., 128×16,
      1-bit packed or RGB565) to GPU SRAM at init. Contains status icons: BLE,
      WiFi, microphone, battery, error, thermometer — 16×12 or 12×12 each.
      Drawn to HUD via `CMD_DRAW_SPRITE` referencing atlas sub-regions.
      Build-time script converts PNGs → C array → uploaded via `CMD_UPLOAD_TEXTURE`.
  - Priority: **Low** · Effort: 1 day · Target: M13
  - Replaces the host-side `Flash/Icons/Icons.h` pattern for GPU-attached HUD

### 14.5 Separation of Concerns (Host Side)

- [ ] **Decouple ESPMenu into independent subsystems** — Refactor the current
      monolithic `ESPMenu.h` (727 lines, mixes HUD + BLE + gesture + NeoPixel) into:
      (a) `HostBootDisplay` — M5UnitGLASS2 boot progress bars only (pre-GPU init),
      (b) `BleControlHandler` — BLE UART parsing and command dispatch,
      (c) `SensorInputHandler` — gesture/proximity/boop detection.
      After GPU init, the host no longer drives the HUD OLED — it sends
      `CMD_SET_HOST_STATUS` and the GPU renders the HUD content.
  - Priority: **High** · Effort: 3 days · Target: M12

### 14.6 Power-Aware HUD

- [ ] **HUD power state tracking** — When GPU enters `PGL_POWER_SLEEP`, the
      `I2cDisplayDriver` for the HUD calls `displayOff()` / `setBrightness(0)`.
      On wake, restore last brightness. Integrate with §7 Power Management states.
      On host side, `HostBootDisplay` also respects power state for the fallback OLED.
  - Priority: **Low** · Effort: 0.5 day · Target: M13

- [ ] **HUD update throttling** — Cap HUD render target refresh to 5–10 Hz (the
      128×64 mono OLED only needs ~6 FPS for readable text). The GPU's
      `DisplayManager` can skip `SwapBuffers()` on the HUD display if the dirty-rect
      tracker reports no changes. I2C1 bus bandwidth is not shared with the host
      Octal-SPI bus, so contention is not an issue — but CPU time for 2D draw
      should be minimised.
  - Priority: **Medium** · Effort: 0.5 day · Target: M12

### 14.7 Host-Attached Fallback (Optional)

- [ ] **`PglHostHud` lightweight interface** — For boards that keep the HUD on the
      host (e.g., M5UnitGLASS2 on ESP32-S3 I2C), provide a thin `PglHostHud`
      interface: `init()`, `showBootProgress()`, `drawDiagnostics()`, `flush()`.
      Reads `QueryGpuHealth()` from GPU and `esp_get_free_heap_size()` locally.
      This is the **fallback path** for when no GPU-attached I2C HUD is available,
      or during early boot before the GPU is initialised.
  - Priority: **Low** · Effort: 1 day · Target: M13

---

## Summary — Priority Distribution

| Priority | Item Count | Estimated Total Effort |
|---|---|---|
| **Critical** | 9 items | ~7 days |
| **High** | 22 items | ~24 days |
| **Medium** | 33 items | ~33 days |
| **Low** | 27 items | ~48.5 days (many are Phase 2) |
| **Total** | **91 items** | **~112.5 days** |

### Recommended Execution Order

1. **Pre-M11 Sprint (2-3 weeks):** All Critical + High items from §1 (Error Recovery),
   §2.1–2.2 (Watchdog + Fault Handlers), §3.1 (Interrupt Priorities), §6.1 (Bytecode
   Validation), §8.4 (Fuzz Testing), §9.1 (Command Validation). These are **prerequisites
   for safe hardware bring-up**.

2. **M11 Integration (3 weeks):** Medium items from §5 (Concurrency), §8.1–8.3 (Test
   Harness), §4.1 (Resolution Strategy doc), §9.2 (Protocol Versioning), §12.1 (Render
   Stats). Coincides with M11 display + pool work.

3. **M12–M13 (6 weeks):** §4.1 (16-bit Z, External FB), §7 (Power Management),
   §10 (Rendering Quality), §6.2 (Shader Debug), **§14 (GPU-Attached HUD)** — register
   I2C HUD driver on RP2350, `CMD_SET_HOST_STATUS`, GPU/host telemetry renderers,
   ESPMenu refactor, HUD layout. Coincides with DVI + 2D + compositing — the HUD
   directly uses the 2D layer system being built in this phase.

4. **Phase 2 (TBD):** §11 (FPGA Portability), §6.3–6.4 (Vertex Shaders, Control Flow),
   §3.2 (FPGA Interrupts), §2.3 (RISC-V Faults), §14.3 mini-FB preview, §14.7
   host-attached fallback. Triggered by hardware availability.

---

## Version History

| Version | Date | Changes |
|---|---|---|
| v1.0 | 2026-03-07 | Initial checklist from architectural review |
| v1.1 | 2026-03-07 | Added §14 Host-Side HUD Display (10 items) |
| v1.2 | 2026-03-07 | Rewrote §14 as GPU-Attached HUD (17 items): primary HUD on RP2350 I2C1 (SSD1306/SSD1309 128×64 mono), host pushes status via `CMD_SET_HOST_STATUS` (0x93), leverages existing DisplayManager + I2cDisplayDriver + 2D layer system. Host M5UnitGLASS2 demoted to boot-only fallback. |
