# Host ↔ GPU Communication Protocol

## Interfaces

1. **Octal SPI (High-Speed Data Bus)**
   * **Purpose:** Transferring ProtoGL command buffers (vertices, transforms, material mappings) to the GPU continuously.
   * **Bandwidth & Clock:** Designed to run at **64 MHz or 80 MHz**. Since Octal SPI transfers 1 byte per clock cycle, the theoretical bandwidth is **64 to 80 Megabytes per second (MB/s)**. 
   * **Transfer Time:** A typical command buffer for a complex frame might be 5-10 KB. At 80 MB/s, a frame's command payload takes under **0.15 milliseconds** to transfer via DMA. This makes the interconnect essentially invisible and instantaneous.
   * **Mode:** Host (ESP32-S3) is the SPI Master. GPU is the SPI Slave (on RP2350: PIO-based slave; on RISC-V/FPGA: native SPI peripheral or custom receiver).
   * **Transport Independence:** The ProtoGL wire format is a flat byte stream. While Octal SPI is the reference transport, the same bytes can be carried over QSPI, standard SPI, shared-memory DMA, or any other byte-oriented bus.

2. **I2C Bus (Configuration & Control Bus)**
   * **Purpose:** Command interface to change rendering settings, toggle matrices on/off, change global brightness, send display configurations (Resolution, Row-Scan config), query GPU status/capability, and reset the GPU.
   * **Bandwidth:** Default 100kHz or Fast 400kHz.
   * **Mode:** Host (ESP32-S3) as Master. GPU as Slave.

## Commands (I2C)

Let `0x3C` be the default GPU I2C address. Register writes follow the format:
`[Register Byte] [Payload...]`

For register reads, the host first writes only the register byte, then performs an I2C read transaction.

### Registry

* **0x01: Set Brightness**
  - Payload: 1 Byte
  - Values: 0-255 global brightness.
* **0x02: Set Panel Config**
  - Payload: 4 Bytes
  - Values: `[Width_L] [Width_H] [Height_L] [Height_H]`
* **0x03: Set Scan Rate**
  - Payload: 1 Byte
  - Values: e.g. `8`, `16`, `32`
* **0x04: Clear Display**
  - Payload: none
* **0x05: Set Color Order**
  - Payload: 1 Byte
  - Values: `0` (RGB), `1` (RBG), `2` (GRB), etc.
* **0x06: Set Gamma Table**
  - Payload: 1 Byte
  - Values: `0` (linear), `1` (CIE), `2` (sRGB)
* **0x07: Set Octal SPI Clock**
  - Payload: 1 Byte
  - Values: MHz (40, 64, 80)
* **0x09: Capability Query**
  - Returns 16 bytes: `PglCapabilityResponse` — GPU architecture, core count, clock speed, SRAM size, resource limits, and hardware feature flags. See `ProtoGL_API_Spec.md` §4.3 for full layout.
  - The host should query this on startup to discover what GPU is connected.
* **0x0A: Status Request**
  - Returns 8 bytes: `currentFPS(u16)`, `droppedFrames(u16)`, `freeMemory(u16, /16)`, `temperature(i8)`, `flags(u8)` (bit0: renderBusy, bit1: bufferOverflow)
* **0x0B: Reset GPU**
  - Payload: none
  - Resets all GPU state, clears resource tables, re-initializes display output

### GPU Memory Access Registers (0x0C – 0x0F)

These registers enable host-side access to GPU device memory across all three tiers
(SRAM, PIO2 external, QSPI MRAM/PSRAM). See `GPU_API_Design.md` §9 and `ProtoGL_API_Spec.md` §4.5.

* **0x0C: Memory Tier Info** (Read)
  - Returns 20 bytes: `PglMemTierInfoResponse`
  - Per-tier capacity (total/free KB) for SRAM, PIO2 external memory, and QSPI CS1 (MRAM or PSRAM)
  - Tier enabled flags, cache entry count, total managed allocations, cache hit rate
  - Host can poll this periodically to monitor GPU memory pressure

* **0x0D: Memory Read Address** (Write)
  - Write 7 bytes: `PglMemReadSetup` — tier(u8), address(u32), length(u16)
  - Configures the read cursor for subsequent `0x0E` reads
  - Alternative to `CMD_MEM_READ_REQUEST` SPI command (for I2C-only setups)

* **0x0E: Memory Read Data** (Read)
  - Returns 32 bytes per read from the staging buffer
  - Auto-increments the read cursor; host issues `ceil(length/32)` reads
  - Staging buffer filled by `CMD_MEM_READ_REQUEST`, `CMD_FRAMEBUFFER_CAPTURE`,
    or direct write to register `0x0D`
  - At 400 kHz I2C, 4096 bytes ≈ 80 ms readback time

* **0x0F: Memory Alloc Result** (Read)
  - Returns 7 bytes: `PglMemAllocResult` — handle(u16), address(u32), status(u8)
  - Contains the result of the most recent `CMD_MEM_ALLOC` command
  - Status codes: 0x00=OK, 0x01=OutOfMemory, 0x02=InvalidTier, 0x03=TierDisabled, 0x04=HandleExhausted

### GPU Diagnostics & Clock Registers (0x10 – 0x11)

* **0x10: Set Clock Frequency** (Write)
  - Write 4 bytes: `PglClockRequest` — `targetMHz(u16)`, `voltageLevel(u8)`, `flags(u8)`
  - `targetMHz`: One of the pre-validated profiles (150, 200, 250, 266, 300 MHz). 0 = query only.
  - `voltageLevel`: VREG level (0 = auto-select based on target frequency).
  - `flags`: bit 0 = recalculate PIO SM clock dividers after change (`PGL_CLOCK_RECONFIGURE_PIO`),
             bit 1 = enable automatic thermal throttling (`PGL_CLOCK_THERMAL_AUTO`)
  - The GPU applies the change asynchronously; query register 0x11 to confirm the actual running frequency.
  - Voltage is raised **before** clock increase, lowered **after** clock decrease to prevent brownout.

* **0x11: Extended Status** (Read)
  - Returns 32 bytes: `PglExtendedStatusResponse`
  - Layout:
    | Offset | Field | Type | Description |
    |--------|-------|------|-------------|
    | 0–1 | `currentFPS` | u16 | Measured render FPS |
    | 2–3 | `droppedFrames` | u16 | Cumulative CRC/overflow drops |
    | 4 | `gpuUsagePercent` | u8 | 0–100, fraction of frame time spent rendering |
    | 5 | `core0UsagePercent` | u8 | 0–100, Core 0 render load |
    | 6 | `core1UsagePercent` | u8 | 0–100, Core 1 render load |
    | 7 | `flags` | u8 | PglStatusFlags (renderBusy, bufferOverflow) |
    | 8–9 | `temperatureQ8` | i16 | Die temperature in Q8.8 fixed-point (°C × 256) |
    | 10–11 | `currentClockMHz` | u16 | Actual running system clock frequency |
    | 12–13 | `sramFreeKB` | u16 | Free internal SRAM in KB |
    | 14–15 | `opiVramTotalKB` | u16 | PIO2 external memory total (0 if not present) |
    | 16–17 | `opiVramFreeKB` | u16 | PIO2 external memory free |
    | 18–19 | `qspiVramTotalKB` | u16 | QSPI CS1 total (MRAM or PSRAM, 0 if not present) |
    | 20–21 | `frameTimeUs` | u16 | Last frame wall-clock time (µs) |
    | 22–23 | `rasterTimeUs` | u16 | Rasterization time (both cores) |
    | 24–25 | `transferTimeUs` | u16 | SPI receive + command parse time |
    | 26–27 | `hub75RefreshHz` | u16 | HUB75 display refresh rate |
    | 28–29 | `qspiVramFreeKB` | u16 | QSPI CS1 free |
    | 30 | `vramTierFlags` | u8 | bit0: PIO2 detected, bit1: QSPI detected, bit2: PIO2 initialised, bit3: QSPI initialised |
    | 31 | `qspiChipType` | u8 | `PglQspiChipType`: 0=none, 1=MRAM MR10Q010, 2=PSRAM APS6408L, 3=ESP-PSRAM, 0xFE=unknown |
  - Temperature is read from the RP2350 on-die ADC sensor. Formula: `temp_C = 27.0 - (V - 0.706) / 0.001721`.
    Accuracy is ±5 °C, sufficient for thermal throttling decisions.
  - VRAM fields report 0 when no external PSRAM is detected.

## Data Frames (Octal SPI) — ProtoGL Command Buffers

The Octal SPI bus exclusively carries **ProtoGL Command Buffers** (see `ProtoGL_API_Spec.md` for the full wire format).

Each frame is a single DMA burst:
```
[Frame Header: 0x55AA + frameNum(4) + totalLen(4) + cmdCount(2)]  (12 bytes)
[Command 0: opcode(1) + length(2) + payload...]                   (variable)
[Command 1: ...]                                                   (variable)
...
[Command N: CMD_END_FRAME]                                         (7 bytes)
[CRC-16]                                                           (2 bytes)
```

Typical frame payload is **5-10 KB** (20 objects, some with morph vertices).
At 80 MHz Octal SPI = 80 MB/s, transfer time is under **0.15 ms** per frame — effectively zero latency.

The GPU receiver detects the sync word (`0x55AA`), DMA-drains the full payload into a SRAM ring buffer, and the command parser begins processing immediately.

> **Implementation note:** On RP2350, PIO is used as the Octal SPI receiver. On RISC-V or FPGA
> GPUs, use the native SPI peripheral or a custom parallel interface. The ProtoGL wire format
> is the same regardless of the physical receiver implementation.

## Signal Timings
The host uses an Octal SPI / LCD Parallel mode driver to output the datastream synchronously with a Chip Select (`CS`) signal to trigger the GPU's DMA IRQ.

### Core Pin Table (GPIO 0–15 — all RP2350 packages)

| Pin Name | GPU (RP2350 GPIO) | Host (ESP32-S3) | Description |
|---|---|---|---|
| SPI D0  | GPIO 0 | LCD_D0 | Data line 0 |
| SPI D1  | GPIO 1 | LCD_D1 | Data line 1 |
| SPI D2  | GPIO 2 | LCD_D2 | Data line 2 |
| SPI D3  | GPIO 3 | LCD_D3 | Data line 3 |
| SPI D4  | GPIO 4 | LCD_D4 | Data line 4 |
| SPI D5  | GPIO 5 | LCD_D5 | Data line 5 |
| SPI D6  | GPIO 6 | LCD_D6 | Data line 6 |
| SPI D7  | GPIO 7 | LCD_D7 | Data line 7 |
| SPI CLK | GPIO 8 | LCD_PCLK | Fast clock for octal data |
| SPI CS  | GPIO 9 | LCD_CS | Slave select / Frame sync |
| RDY     | GPIO 10 (output) | Configurable (input) | GPU pulls high when ready to receive |
| PIO2 CS0 | GPIO 11 | — | PIO2 memory chip select 0 (active-low) |
| PIO2 CLK | GPIO 12 | — | PIO2 memory clock (75–104 MHz) |
| I2C SDA | GPIO 14 | Configurable | Low-speed command data |
| I2C SCL | GPIO 15 | Configurable | Low-speed clock |

### HUB75 Display Pins (GPIO 16–29 — all RP2350 packages)

| Pin Name | GPU (RP2350 GPIO) | Description |
|---|---|---|
| R1  | GPIO 16 | Red data, top half |
| G1  | GPIO 17 | Green data, top half |
| B1  | GPIO 18 | Blue data, top half |
| R2  | GPIO 19 | Red data, bottom half |
| G2  | GPIO 20 | Green data, bottom half |
| B2  | GPIO 21 | Blue data, bottom half |
| A   | GPIO 22 | Row address bit 0 |
| B   | GPIO 23 | Row address bit 1 |
| C   | GPIO 24 | Row address bit 2 |
| D   | GPIO 25 | Row address bit 3 |
| E   | GPIO 26 | Row address bit 4 (for 1/32 scan) |
| CLK | GPIO 27 | Pixel shift clock |
| LAT | GPIO 28 | Row latch |
| OE  | GPIO 29 | Output enable (active-low) |

### PIO2 External Memory Data Pins (GPIO 34–41)

These pins are used by the optional Tier 1 PIO2 external memory interface. Pin usage depends on the configured `PIO2_MEM_MODE`:

- **OPI PSRAM** (`OPI_PSRAM`): All 8 data lines (GPIO 34–41), QFN-80 package only.
- **Dual QSPI MRAM** (`DUAL_QSPI_MRAM`): 4 data lines (GPIO 34–37) + CS1 on GPIO 38. Any RP2350 package.
- **Single QSPI MRAM** (`SINGLE_QSPI_MRAM`): 4 data lines (GPIO 34–37) only. Any RP2350 package.

| Pin Name | GPIO | OPI PSRAM | Dual QSPI MRAM | Single QSPI MRAM |
|---|---|---|---|---|
| DQ0 | 34 | Data line 0 | Data line 0 | Data line 0 |
| DQ1 | 35 | Data line 1 | Data line 1 | Data line 1 |
| DQ2 | 36 | Data line 2 | Data line 2 | Data line 2 |
| DQ3 | 37 | Data line 3 | Data line 3 | Data line 3 |
| DQ4 / CS1 | 38 | Data line 4 | **MRAM CS1** | _Unused_ |
| DQ5 | 39 | Data line 5 | _Unused_ | _Unused_ |
| DQ6 | 40 | Data line 6 | _Unused_ | _Unused_ |
| DQ7 | 41 | Data line 7 | _Unused_ | _Unused_ |

> **Note:** GPIO 13 is reserved. GPIO 30–33 are not accessible on QFN-60; on QFN-80 they are available but currently unassigned.

### QSPI CS1 Memory (Optional — Auto-Detected)

An optional Tier 2 QSPI memory chip on QMI CS1 is **auto-detected at boot**. The firmware probes with chip-specific RDID commands and supports:

- **Everspin MR10Q010 MRAM** (128 KB, non-volatile, no random-access penalty, ~52 MB/s). RDID: 0x4B → 0x076B111111. Needs WREN before writes. Dual supply: VDD 3.3V, VDDQ 1.8V.
- **AP Memory APS6408L PSRAM** (8 MB, volatile, row-buffer miss penalty, ~66 MB/s @ 133 MHz). RDID: 0x9F → MFR=0x0D. Needs refresh.

Both are mapped into XIP at `0x11000000`. The detected chip type determines driver init sequence and memory tier placement weights (MRAM's uniform latency allows aggressive demotion of random-access resources from SRAM to QSPI). The detected type is reported in extended status byte 31 (`qspiChipType`).

### PIO Block Allocation

The RP2350 has three PIO blocks (PIO0, PIO1, PIO2), each with four state machines:

| PIO Block | State Machine(s) | Function | Status |
|---|---|---|---|
| PIO0 | SM0: Data shift, SM1: Row select | HUB75 display driver | ✅ Active (M1) |
| PIO1 | SM0: 8-bit parallel RX | Octal SPI receiver (host→GPU) | ✅ Active (M1) |
| PIO2 | SM0: Cmd/write, SM1: Read (OPI 8-bit or QSPI 4-bit) | PIO2 external memory (OPI PSRAM or QSPI MRAM) | ⬜ Planned (M8) |

> **Note:** RP2350 GPIO assignments are locked in `gpu_config.h`. ESP32-S3 pin assignments are configured at runtime via `PglDeviceConfig`. PIO2 OPI PSRAM mode requires the QFN-80 package (GPIO 34–41), while PIO2 QSPI MRAM modes work on any RP2350 package (GPIO 34–37 + CS). When `PIO2_MEM_MODE` is `NONE`, the build gracefully degrades to SRAM-only operation. The QSPI CS1 chip (MRAM or PSRAM) is auto-detected at boot and works on any RP2350 package (needs external wiring; MRAM needs dual supply). See `GPU_API_Design.md` §8 for the full tiered memory architecture.