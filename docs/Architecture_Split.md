# Architectural Split: Host + GPU Dual-MCU System

## Overview
To improve the performance and maintainability of ProtoTracer, the workload is being split across two separate microcontrollers:
1. **Host (ESP32-S3):** Handles system control, network operations, animations, object physics, and overall scene management.
2. **GPU (External Rendering Core):** Handles localized rasterization, 3D math projection, and final driving of the HUB75 LED matrices.

The GPU is connected to the host via a high-speed parallel data bus (Octal SPI) and a low-speed configuration bus (I2C). The ProtoGL wire protocol is **architecture-agnostic** (Vulkan-like), enabling future GPU implementations on different architectures — but Phase 1 targets the **RP2350** as the concrete, committed GPU:

| GPU Implementation | Phase | Status | Notes |
|---|---|---|---|
| **RP2350 (ARM Cortex-M33)** | **Phase 1** | **M0–M6 code complete, M7–M8 pending** | 520 KB SRAM, dual-core @ 150 MHz, PIO for HUB75 + Octal SPI. Optional external memory (OPI PSRAM + QSPI MRAM tiered). |
| **RP2350 (Hazard3 RISC-V)** | Phase 2 | Planned | Same chip, RISC-V boot mode — recompile only |
| **Custom RISC-V SoC** | Phase 2 | Planned | Must implement ProtoGL command parser + rasterizer |
| **FPGA** | Phase 2 | Planned | Hardware-accelerated rasterizer possible |
| **ARM Cortex-M7 (STM32H7)** | Phase 2 | Planned | More SRAM, faster clock |

### Rationale
Currently, the ESP32 handles both the logic (Wi-Fi, Bluetooth, animation JSON parsing) and the heavy computational load of rendering and pushing high-speed bits to the matrix via SmartMatrix or direct I2S/LCD peripherals. The primary bottleneck is ESP32-S3 PSRAM (~20 FPS due to random-access latency during rasterization).

By offloading rendering and matrix driving to the RP2350 (which features 520 KB of single-cycle tightly-coupled SRAM and PIO-driven display output), the ESP32-S3 is free to parse complex scenes, run animations linearly without jitter, and manage networking workloads seamlessly. The target is **60+ FPS** using the RP2350's dual Cortex-M33 cores with a symmetric screen-space split.

The ProtoGL API is designed to be Vulkan-like and architecture-agnostic, so Phase 2 can retarget other GPU architectures without any host-side code changes.

## System Responsibilities

### ESP32-S3 (Host) Responsibilities:
* Networking & Wi-Fi (ElegantOTA, WebSockets, REST APIs).
* File system management (`mklittlefs`, SD card).
* Animation logic & JSON parsing.
* Scene construction (Node trees, `Transform` updates, Materials configuration).
* Calculating local object positions, physics, and state.
* Encoding configuration commands over **I2C**.
* Building ProtoGL command buffers and transmitting over **Octal SPI**.

### GPU Responsibilities (implementation-dependent):
* Receiving configuration/state requests via **I2C** slave mode.
* Receiving ProtoGL command buffers via **Octal SPI** or other high-speed bus.
* Executing the core Rasterization pipeline (`Camera::Rasterize`).
* Managing local render caches (QuadTrees, Bounding Boxes).
* Driving display output (HUB75 via PIO/HSTX, SPI LCD, or other interface).
* Color depth formatting, gamma correction (e.g., CIE LUTs), and interlacing.
* **Optional:** Managing tiered external memory (OPI PSRAM / QSPI MRAM) with weight-based placement and prefetch pipeline. See `GPU_API_Design.md` §8 for the full architecture.
* **Optional:** Exposing GPU device memory to the host for direct read/write access across all tiers (SRAM, OPI PSRAM, QSPI MRAM) via ProtoGL memory commands (0x30–0x3F) and I2C registers (0x0C–0x0F). See `GPU_API_Design.md` §9.

> **Architecture note:** GPU implementations should use `PglParser.h` for alignment-safe
> deserialization if the core does not support unaligned memory access (e.g., RISC-V
> without Zicclsm). The GPU reports its capabilities via I2C register 0x09.

## Rendering Approach: High-Level Stream vs Framebuffer Stream
Two rendering paradigms can be employed over the Interconnect.

### Option A: GPU Command Buffer (Vulkan-like API) - RECOMMENDED
* ESP32-S3 acts as the game engine CPU. It serializes visual objects (Transforms, Vertex Arrays, Material changes, Camera location) into a ProtoGL Command Buffer.
* The host sends this tight data payload over Octal SPI once per frame.
* **Pros:** Exponentially frees up ESP32-S3 memory and CPU. Moves mathematical processing into the GPU's fast internal SRAM, completely circumventing ESP32 PSRAM latency limitations. GPU implementation is pluggable — any core that can parse the byte stream works.
* **Cons:** Requires porting `Render/Camera.h` and 3D projection logic to the GPU's SDK.

### Option B: Render on ESP32-S3, RP2350 as Framebuffer Sink (Legacy Approach)
* ESP32-S3 processes the `Camera::Rasterize` step inside PSRAM (Bottlenecked at ~20 FPS).
* A RAW RGB565 / RGB888 framebuffer is pumped over Octal SPI using ESP32-S3's LCD peripheral or Octal SPI DMA.
* **Pros:** Easy to code. RP2350 only runs PIO/HUB75 logic.
* **Cons:** Squanders the processing power of the RP2350 and keeps the memory bottleneck residing on the ESP32-S3 side.

*Decision: We are pursuing Option A (GPU API). The reference GPU (RP2350) has 520KB of 1-cycle SRAM which easily fits a 128x64 double-buffered RGB565 framebuffer (~32KB), a rendering Z-buffer (~32KB), leaving ample memory for localized QuadTrees and node data. Other GPU implementations with different SRAM sizes can report their limits via the capability query. This allows true parallel processing.*

## File System Changes Needed
- `src/Controllers/GPUDriverController.h`: New controller replacing `SmartMatrixHUB75.h`. Uses the `ProtoGL` host library to encode command buffers and DMA them out over Octal SPI.
- `lib/ProtoGL/`: Host-side library providing the Vulkan-like API (`pglCreateMesh`, `pglDrawObject`, `pglBeginFrame`/`pglEndFrame`, etc.). Architecture-agnostic wire format. See `ProtoGL_API_Spec.md` for complete API reference.
- **GPU Firmware Project (Phase 1: RP2350)**: Pico-SDK CMake project containing the RP2350-side command parser, rasterizer (ported from `Camera.h`), QuadTree, material system, dual-core Cortex-M33 scheduler, PIO HUB75 driver, and PIO Octal SPI receiver.

## Related Documents
- [ProtoGL_API_Spec.md](ProtoGL_API_Spec.md) — Full wire-format specification and C++ host/GPU API
- [Project_Schedule.md](Project_Schedule.md) — 18-week Phase 1 schedule (RP2350) + Phase 2 roadmap
- [Communication_Protocol.md](Communication_Protocol.md) — I2C register map and Octal SPI transport details
- [GPU_API_Design.md](GPU_API_Design.md) — Dual-core scheduling, memory budget, performance analysis