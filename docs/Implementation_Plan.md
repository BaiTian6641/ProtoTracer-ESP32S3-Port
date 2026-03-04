# Transition Plan & Roadmap: Dual-MCU Refactor

> **⚠ This document is the original Phase 1 transition plan.**
> For the current day-by-day milestone schedule (M0–M8), risk register, and
> audit trail, see **[Project_Schedule.md](Project_Schedule.md)**.
>
> The phases below have been fully incorporated into the M0–M8 milestones:
>
> | Original Phase | Milestone(s) | Status |
> |---|---|---|
> | Phase 1: Planning and Research | M0 (Toolchain & Skeleton) | ✅ Complete |
> | Phase 2: ESP32-S3 Firmware Changes | M0, M2 (Host Integration) | ✅ Complete |
> | Phase 3: RP2350 Base Station Code | M1 (RP2350 Core Bring-up) | ✅ Complete |
> | Phase 4: PIO HUB75 Driver | M1 (HUB75 PIO + BCM) | ✅ Complete |
> | Phase 5: Debug & Profile | M2 (Hardware Validation) | 🔧 In progress (end-to-end test remaining) |
>
> Milestones M3–M8 extend beyond the original plan with: scene math (M3), rasterizer (M4),
> dual-core parallelism (M5), material system (M6), animation pipeline (M7),
> and tiered external memory (M8).

---

## Phase 1: Planning and Research
1. Identify memory requirements for RP2350. *Verdict: 520KB SRAM is plenty for Double Framebuffer + Z-Buffer + Local Scene Nodes.*
2. Shift strictly to **Geometry Sync (Command Buffer API)**. Pushing framebuffers is wasteful when we can treat the RP2350 as a discrete GPU to bypass ESP32 PSRAM latency.

## Phase 2: Firmware Scaffolding (ESP32-S3 Changes)
1. **Remove Local Panel Drivers:** Gradually deprecate `SmartMatrixHUB75.h`, I2S drivers, and `SmartMatrix` dependencies from the ESP32-S3 target.
2. **Implement Vulkan-like Controller:** Create a new class `GPUDriverController.h` that acts as the Command Buffer encoder. Instead of rasterizing on the ESP, it packs `Transform`, `Material`, and `Camera` data into byte arrays and sends them via Octal SPI DMA.
3. **Setup I2C Master:** Introduce a configuration daemon that acts on Boot and state changes to dispatch `Wire` (I2C) commands addressing `0x3C` (The RP2350).

## Phase 3: Create The RP2350 Base Station Code
1. **New Repository:** Create a new branch/repo explicitly for the RP2350 firmware using Pico-SDK (as Arduino Core for RP2350 might be limiting for pure PIO implementations).
2. **I2C Slave Node:** Initialize hardware I2C in slave mode to parse the register map.
3. **Octal SPI Receiver:** Configure a DMA + PIO machine specifically tailored to absorb 8-bit parallel data clocked from the ESP32-S3 continuously into a local array.

## Phase 4: RP2350 PIO HUB75 Driver
1. Adapt [Pico-PIO-HUB75](https://github.com/Wren6991/Pico-PIO-Hub75) or a similar library to push the ingested framebuffer payload to the HUB75 Matrix out hardware pins.
2. The PIO block makes this operation entirely background-driven.
3. Hook up the PIO buffers to the incoming SPI buffers over a simple Double Buffer.

## Phase 5: Debug & Profile
1. **Visual Testing:** Look for frame tearing. Implement a simple hardware-level handshaking pin (e.g. `VSYNC` or `RDY`) so the ESP32-S3 doesn't overwrite an actively scanning frame on the RP2350.
2. **Framerate checking:** Read the `0x0A Status Request` via I2C to see if RP2350 is dropping incoming frames.
3. Tune Octal SPI MHz parameters to safely remain under signal-bounce limits.