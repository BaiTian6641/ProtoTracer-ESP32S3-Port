# Host ↔ GPU Communication Protocol

## Interfaces

1. **Octal SPI (High-Bandwidth Data Plane)**
   * **Role:** The primary data bus for all bulk Host↔GPU transfers — rendering command
     buffers (Host→GPU), framebuffer readback, memory read/write, and high-speed status
     queries. Analogous to a PCIe data link on a desktop GPU.
   * **Bandwidth & Clock:** 64 MHz or 80 MHz. Octal SPI transfers 1 byte per clock cycle,
     giving **64–80 MB/s** theoretical bandwidth in each direction (half-duplex).
   * **Transfer Time:** A typical 5–10 KB command frame takes **< 0.15 ms** to transfer.
     A 4 KB memory readback completes in **~50 µs** — over 1000× faster than I2C.
   * **Mode:** Half-duplex bidirectional. Host (ESP32-S3) is always the bus master and
     drives CLK/CS. A dedicated **DIR pin** (GPIO 10 on RP2350) controls data-line direction:
     DIR=LOW → Host drives D0–D7 (TX phase); DIR=HIGH → GPU drives D0–D7 (RX phase).
   * **Transport Independence:** The ProtoGL wire format is a flat byte stream. While
     Octal SPI is the reference transport, the same bytes can be carried over QSPI,
     standard SPI, shared-memory DMA, or any other byte-oriented bus.

2. **I2C Bus (Low-Bandwidth Management / Control Plane)**
   * **Role:** A lightweight management bus for device identification, status monitoring,
     configuration, and diagnostics — conceptually similar to **SMBus** on PC platforms.
     Handles small, infrequent register-based transactions: GPU capability queries,
     brightness / panel / gamma configuration, health telemetry (temperature, FPS,
     dropped frames), clock frequency adjustments, and boot-time device probing.
   * **Bandwidth:** Standard 100 kHz or Fast 400 kHz (~50 KB/s effective). This is more
     than sufficient for management traffic, which consists of short register reads/writes
     (typically 1–32 bytes per transaction).
   * **Mode:** Host (ESP32-S3) as Master, GPU as Slave.
   * **When to use:** Always available for management tasks. Also serves as a fallback
     path for GPU readback when the Octal SPI bus is unavailable (e.g., during early
     boot before PIO initialisation, or in ultra-low-power standby mode).

> **Design rationale — two-bus architecture:**
> Separating high-bandwidth rendering data (Octal SPI) from low-bandwidth management
> (I2C) follows the same principle as PCIe + SMBus on desktop GPUs, or USB bulk + HID
> on embedded devices. The I2C management bus can operate independently of the data
> plane — the host can query GPU health, change brightness, or adjust clock speed
> without interrupting an in-flight SPI DMA transfer.

---

## Bidirectional Octal SPI Protocol

### Bus Architecture

The same 8 data lines (D0–D7) carry data in both directions using half-duplex
arbitration. The host is always the clock master — it drives CLK and CS in both
TX (Host→GPU) and RX (GPU→Host) phases.

```
              ┌─────────────────────────────────────────────┐
              │           ESP32-S3 (Host / Bus Master)       │
              │                                               │
              │  LCD_CAM ──────┐   SPI2 (Octal HD) ────┐    │
              │  (burst TX)    │   (read transactions)  │    │
              │                │                        │    │
              │      D0–D7 ←──┴────────────────────────►┘   │
              │      CLK ──────────────────────────────►     │
              │      CS  ──────────────────────────────►     │
              │      DIR ──────────────────────────────►     │
              │      IRQ ◄──────────────────────────────     │
              └──────┬──────────────────────────────┬────────┘
                     │ D0–D7, CLK, CS, DIR          │ IRQ
              ┌──────▼──────────────────────────────▼────────┐
              │           RP2350 (GPU / Bus Slave)            │
              │                                               │
              │  PIO1 SM0 ─── Octal SPI Receiver (RX)       │
              │  PIO1 SM1 ─── Octal SPI Transmitter (TX)    │
              │               DIR pin selects active SM       │
              │                                               │
              │  GPIO 13 (IRQ) ─ Notify host of async events │
              └───────────────────────────────────────────────┘
```

### Data Phases

| Phase | DIR Pin | D0–D7 Driver | Description |
|-------|---------|-------------|-------------|
| **TX (Host→GPU)** | LOW | ESP32-S3 | Command buffer streaming, memory writes, mailbox writes |
| **Turnaround** | LOW→HIGH | _Tri-state_ | 2 dummy clock cycles; both sides release bus |
| **RX (GPU→Host)** | HIGH | RP2350 | Status reads, memory readback, mailbox reads, bulk data |
| **Idle** | LOW | _Tri-state_ | CS deasserted; bus idle |

### Write Transaction (Host→GPU)

Standard command-buffer streaming. Identical to previous unidirectional protocol:

```
CS ▔▔▔▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▔▔▔
CLK     ╱╲╱╲╱╲╱╲╱╲╱╲╱╲╱╲╱╲╱╲╱╲╱╲╱╲╱╲╱╲╱╲╱╲╱╲╱╲╱╲
DIR ▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁
D0-7    [Frame Header      ][Command 0     ][Cmd 1   ]...
         Host drives                Host drives
```

The host uses the ESP32-S3 **LCD_CAM** peripheral in 8-bit parallel mode with DMA
for maximum throughput (80 MB/s at 80 MHz).

### Read Transaction (GPU→Host)

For status queries, memory readback, shared memory window reads, and bulk data:

```
CS ▔▔▔▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▔▔▔
CLK     ╱╲╱╲╱╲╱╲╱╲╱╲  ╱╲╱╲  ╱╲╱╲╱╲╱╲╱╲╱╲╱╲╱╲╱╲╱╲╱╲╱╲╱╲╱╲
DIR ▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔
D0-7    [Read Cmd ][Addr ]  [T ][Response Data (N bytes)         ]
         Host TX             GPU TX
                        ↑
                   Turnaround
                   (2 dummy clocks)
```

1. **TX portion**: Host sends a 1-byte read command + 3-byte address (4 bytes total)
2. **Turnaround**: Host asserts DIR=HIGH, 2 dummy clock cycles for line settling
3. **RX portion**: Host continues clocking; GPU drives D0–D7 with response data
4. **Completion**: Host deasserts CS, sets DIR=LOW

The host uses the ESP32-S3 **SPI2** peripheral in Octal SPI half-duplex mode for
read transactions, which natively supports command→dummy→data phasing.

### SPI Read Commands

These replace the I2C register read path with Octal SPI read transactions:

| Read Cmd | Name | Address Bytes | Response Size | Description |
|----------|------|---------------|---------------|-------------|
| `0xE0` | `SPI_READ_STATUS` | 0 | 8 | Basic GPU status (same as I2C 0x0A) |
| `0xE1` | `SPI_READ_EXTENDED` | 0 | 32 | Extended status (same as I2C 0x11) |
| `0xE2` | `SPI_READ_CAPABILITY` | 0 | 16 | GPU capability query (same as I2C 0x09) |
| `0xE3` | `SPI_READ_MEM_TIER` | 0 | 20 | Memory tier info (same as I2C 0x0C) |
| `0xE4` | `SPI_READ_MEM_DATA` | 3 (tier+addr) | N | Read GPU memory at address (replaces I2C 0x0D+0x0E) |
| `0xE5` | `SPI_READ_ALLOC_RESULT` | 0 | 7 | Last alloc result (same as I2C 0x0F) |
| `0xE6` | `SPI_READ_POOL_STATUS` | 0 | 16 | Pool diagnostic info (same as I2C 0x18) |
| `0xE7` | `SPI_READ_DEFRAG_STATUS` | 0 | 8 | Defrag progress (same as I2C 0x19) |
| `0xE8` | `SPI_READ_DISPLAY_INFO` | 1 (index) | 20 | Display info (same as I2C 0x15) |
| `0xE9` | `SPI_READ_DIRTY_STATS` | 0 | 8 | DMA/dirty stats (same as I2C 0x1D) |
| `0xEA` | `SPI_READ_SMW` | 2 (offset) | N | Read Shared Memory Window at offset |
| `0xEB` | `SPI_READ_PERSIST_STATUS` | 0 | 12 | Persistence status (same as I2C 0x1C) *(v0.7.1)* |

> **Note:** For `SPI_READ_MEM_DATA` and `SPI_READ_SMW`, the response length is
> specified in the host's SPI transaction setup (how many dummy clocks to generate
> in the RX phase). Maximum single read is 4096 bytes.

### SPI Read Performance vs I2C

| Operation | I2C (400 kHz) | SPI Read (80 MHz) | Speedup |
|-----------|---------------|-------------------|---------|
| Read 8-byte status | ~200 µs | ~1.5 µs | **130×** |
| Read 32-byte extended status | ~800 µs | ~2 µs | **400×** |
| Read 4 KB memory block | ~80 ms | ~55 µs | **1450×** |
| Read 16 KB framebuffer | ~320 ms | ~210 µs | **1500×** |

### PIO Implementation (RP2350)

PIO1 hosts two state machines that share the 8 data-line GPIOs:

- **SM0 (Receiver)**: Active when DIR=LOW. Samples D0–D7 on CLK rising edge,
  pushes bytes to RX FIFO. DMA drains FIFO into SRAM ring buffer. This is the
  existing Octal SPI receiver program.
- **SM1 (Transmitter)**: Active when DIR=HIGH. Reads bytes from TX FIFO,
  drives D0–D7 before CLK rising edge. DMA feeds TX FIFO from a response buffer.

Direction switching is coordinated by the PIO programs monitoring the DIR pin
(GPIO 10 as input). When DIR transitions:
1. The active SM completes its current byte and stalls
2. Pin direction registers flip (input↔output) for D0–D7
3. The other SM begins on the next CLK edge

The 2-cycle turnaround gap prevents bus contention.

## I2C Register Map (Management / Control Plane)

> The I2C bus serves as the **management and control plane** — handling device
> identification, configuration, status monitoring, and diagnostics (similar to
> SMBus on PC platforms). Transactions are small register reads/writes (1–32 bytes).
> All registers listed below are also queryable via Octal SPI read commands
> (0xE0–0xEA range) for higher speed, but I2C is the natural home for these
> low-frequency management operations and operates independently of in-flight
> SPI data transfers.

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
(SRAM, QSPI-A external, QSPI-B external). See `GPU_API_Design.md` §9 and `ProtoGL_API_Spec.md` §4.5.

* **0x0C: Memory Tier Info** (Read)
  - Returns 20 bytes: `PglMemTierInfoResponse`
  - Per-tier capacity (total/free KB) for SRAM and each QSPI VRAM channel (A and B)
  - Per-channel chip count, chip type, and enabled flags; cache entry count, cache hit rate
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
    | 14–15 | `qspiATotalKB` | u16 | QSPI Channel A total VRAM (sum of both CS, 0 if absent) |
    | 16–17 | `qspiAFreeKB` | u16 | QSPI Channel A free VRAM |
    | 18–19 | `qspiBTotalKB` | u16 | QSPI Channel B total VRAM (0 if absent or single-channel) |
    | 20–21 | `frameTimeUs` | u16 | Last frame wall-clock time (µs) |
    | 22–23 | `rasterTimeUs` | u16 | Rasterization time (both cores) |
    | 24–25 | `transferTimeUs` | u16 | SPI receive + command parse time |
    | 26–27 | `hub75RefreshHz` | u16 | HUB75 display refresh rate |
    | 28–29 | `qspiBFreeKB` | u16 | QSPI Channel B free VRAM |
    | 30 | `vramTierFlags` | u8 | bit0: QSPI-A detected, bit1: QSPI-B detected, bit2: QSPI-A initialised, bit3: QSPI-B initialised |
    | 31 | `qspiChipType` | u8 | `PglQspiChipType` (Channel A CS0): 0=none, 1=MRAM MR10Q010, 2=PSRAM APS6408L, 3=ESP-PSRAM, 0xFE=unknown |
  - Temperature is read from the RP2350 on-die ADC sensor. Formula: `temp_C = 27.0 - (V - 0.706) / 0.001721`.
    Accuracy is ±5 °C, sufficient for thermal throttling decisions.
  - VRAM fields report 0 when no external PSRAM is detected.

### Resource Persistence Register (0x1C) *(v0.7.1)*

* **0x1C: Persistence Status** (Read)
  - Returns 12 bytes: `PglPersistStatusResponse`
  - Layout:
    | Offset | Field | Type | Description |
    |--------|-------|------|-------------|
    | 0 | `status` | u8 | 0x00=not persisted, 0x01=writeback in progress, 0x02=persisted, 0x03=error |
    | 1 | `resourceClass` | u8 | Queried `PglMemResourceClass` |
    | 2–3 | `resourceId` | u16 | Queried resource ID |
    | 4–7 | `flashAddr` | u32 | Flash address of persisted data (0 if none) |
    | 8–9 | `sizeBytes` | u16 | Size of persisted data in bytes |
    | 10–11 | `reserved` | u16 | — |
  - Populated after a `CMD_QUERY_PERSISTENCE` (0x48) command is processed.
  - When queried in manifest summary mode (`resourceClass=0xFF`), fields are reinterpreted:
    `resourceClass` → `totalEntries (max 64)`, `resourceId` → `usedEntries`,
    `flashAddr` → `freeBytes`, `sizeBytes` → reserved.
  - Also available via SPI read `SPI_READ_PERSIST_STATUS` (0xEB) at full SPI bandwidth.

## Data Frames (Octal SPI) — ProtoGL Command Buffers

The Octal SPI TX phase carries **ProtoGL Command Buffers** (see `ProtoGL_API_Spec.md` for the full wire format).
The RX phase carries read responses (§ Bidirectional Octal SPI Protocol above).

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

> **Implementation note:** On RP2350, PIO is used as the bidirectional Octal SPI transceiver
> (SM0 for RX, SM1 for TX). On RISC-V or FPGA
> GPUs, use the native SPI peripheral or a custom parallel interface. The ProtoGL wire format
> is the same regardless of the physical receiver implementation.

## Signal Timings
The host uses an Octal SPI / LCD Parallel mode driver to output the datastream synchronously with a Chip Select (`CS`) signal to trigger the GPU's DMA IRQ.

### Core Pin Table (GPIO 0–15 — all RP2350 packages)

| Pin Name | GPU (RP2350 GPIO) | Host (ESP32-S3) | Description |
|---|---|---|---|
| SPI D0  | GPIO 0 | LCD_D0 / SPI2_D0 | Bidirectional data line 0 |
| SPI D1  | GPIO 1 | LCD_D1 / SPI2_D1 | Bidirectional data line 1 |
| SPI D2  | GPIO 2 | LCD_D2 / SPI2_D2 | Bidirectional data line 2 |
| SPI D3  | GPIO 3 | LCD_D3 / SPI2_D3 | Bidirectional data line 3 |
| SPI D4  | GPIO 4 | LCD_D4 / SPI2_D4 | Bidirectional data line 4 |
| SPI D5  | GPIO 5 | LCD_D5 / SPI2_D5 | Bidirectional data line 5 |
| SPI D6  | GPIO 6 | LCD_D6 / SPI2_D6 | Bidirectional data line 6 |
| SPI D7  | GPIO 7 | LCD_D7 / SPI2_D7 | Bidirectional data line 7 |
| SPI CLK | GPIO 8 | LCD_PCLK / SPI2_CLK | Fast clock for octal data (host drives always) |
| SPI CS  | GPIO 9 | LCD_CS / SPI2_CS | Slave select / Frame sync |
| DIR     | GPIO 10 (input) | Configurable (output) | Bus direction: LOW=Host TX, HIGH=GPU TX |
| QSPI-A CS0 | GPIO 11 | — | QSPI VRAM Channel A chip-select 0 (active-low) |
| QSPI-A CLK | GPIO 12 | — | QSPI VRAM Channel A clock (75–104 MHz) |
| IRQ     | GPIO 13 (output) | Configurable (input) | GPU→Host async notification (active-low pulse) |
| I2C SDA | GPIO 14 | Configurable | Management bus data (device ID, config, status) |
| I2C SCL | GPIO 15 | Configurable | Management bus clock (100/400 kHz) |

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

### QSPI VRAM Pins (RP2350B QFN-80 Only — GPIO 30+)

External VRAM uses **two independent PIO2-driven QSPI channels**, each supporting up to
2 chip selects (CS0 + CS1) for a maximum of **4 external RAM chips**. Only available on
RP2350B (QFN-80, 48 GPIO). RP2350A (QFN-60, 30 GPIO) has no external VRAM support.

Pin usage depends on `QSPI_VRAM_MODE`:

- **NONE**: No external VRAM. SRAM-only operation.
- **SINGLE_CHANNEL**: QSPI Channel A only (7 GPIO).
- **DUAL_CHANNEL**: Both QSPI Channel A + B (14 GPIO).

#### QSPI Channel A (Tier 1) — PIO2 SM0 + SM1

| Pin Name | GPIO | Description |
|---|---|---|
| QSPI-A DQ0 | 34 | Data line 0 |
| QSPI-A DQ1 | 35 | Data line 1 |
| QSPI-A DQ2 | 36 | Data line 2 |
| QSPI-A DQ3 | 37 | Data line 3 |
| QSPI-A CS1 | 38 | Chip select 1 (active-low, 2nd chip; CS0 on GPIO 11) |
| QSPI-A CLK | — | Shared: GPIO 12 (from Core Pin Table) |
| QSPI-A CS0 | — | Shared: GPIO 11 (from Core Pin Table) |

#### QSPI Channel B (Tier 2) — PIO2 SM2 + SM3

| Pin Name | GPIO | Description |
|---|---|---|
| QSPI-B DQ0 | 39 | Data line 0 |
| QSPI-B DQ1 | 40 | Data line 1 |
| QSPI-B DQ2 | 41 | Data line 2 |
| QSPI-B DQ3 | 42 | Data line 3 |
| QSPI-B CLK | 43 | Channel B clock (75–104 MHz) |
| QSPI-B CS0 | 44 | Chip select 0 (active-low) |
| QSPI-B CS1 | 45 | Chip select 1 (active-low, 2nd chip) |

> **Note:** GPIO 30–33 are not accessible on QFN-60 (RP2350A). GPIO 34–45 require
> QFN-80 (RP2350B). Each chip select is independently auto-detected at boot — the
> firmware probes with RDID commands and supports mixed chip types per channel.

### Supported External Memory Chips (Auto-Detected per Chip Select)

Each QSPI VRAM chip select is individually probed at boot. Supported devices:

- **Everspin MR10Q010 MRAM** (128 KB, non-volatile, uniform latency, ~52 MB/s). RDID: 0x4B → 0x076B111111. Needs WREN before writes. Dual supply: VDD 3.3V, VDDQ 1.8V.
- **AP Memory APS6408L PSRAM** (8 MB, volatile, row-buffer miss penalty, ~66 MB/s @ 133 MHz). RDID: 0x9F → MFR=0x0D. Needs refresh.

All access is **indirect via PIO2 DMA** — no XIP mapping. The detected chip type per
chip-select determines driver init sequence and memory tier placement weights (MRAM's
uniform latency favours demotion of random-access resources from SRAM). Channel A CS0
chip type is reported in extended status byte 31 (`qspiChipType`); full per-chip info
available via `MEM_TIER_INFO` register (0x0C).

### PIO Block Allocation

The RP2350 has three PIO blocks (PIO0, PIO1, PIO2), each with four state machines:

| PIO Block | State Machine(s) | Function | Status |
|---|---|---|---|
| PIO0 | SM0: Data shift, SM1: Row select | HUB75 display driver | ✅ Active (M1) |
| PIO1 | SM0: 8-bit parallel RX, SM1: 8-bit parallel TX | Bidirectional Octal SPI (host↔GPU) | ✅ Active (M1) |
| PIO2 | SM0+SM1: QSPI Ch-A write/read, SM2+SM3: QSPI Ch-B write/read | Dual-channel QSPI VRAM (RP2350B only) | ⬜ Planned (M8) |

> **Note:** GPIO 10 is the bus direction (DIR) pin for bidirectional Octal SPI (host
> drives LOW for TX, HIGH for GPU RX). GPIO 13 is the GPU→Host IRQ output
> (active-low pulse for SMW updates, alloc completions, persist-writeback completions [Slot 9], and error notifications).
> GPIO 14–15 are reserved for I2C management bus. GPIO 30–33 are not accessible
> on QFN-60 (RP2350A); on QFN-80 (RP2350B) they are available but currently unassigned.
> PIO1 uses two state machines: SM0 for RX (existing) and SM1 for TX (bidirectional
> read responses). The IRQ pin is shared across both I2C and Octal SPI protocols — it
> signals any GPU-initiated async event regardless of which bus the host reads the result from.
>
> **RP2350A (QFN-60):** `QSPI_VRAM_MODE` is always `NONE`. SRAM-only operation.
> PIO2 is unused (available for future expansion).
>
> **RP2350B (QFN-80):** `QSPI_VRAM_MODE` may be `SINGLE_CHANNEL` (QSPI-A only,
> 7 pins) or `DUAL_CHANNEL` (QSPI-A + QSPI-B, 14 pins). All 4 PIO2 state machines
> are allocated for dual-channel mode. GPU auto-detects chip type and count per channel.
>
> **HUD Display:** An optional I2C OLED (SSD1306/SSD1309, 128×64 mono) may be attached
> to I2C1 on the RP2350 for GPU-local status display (FPS, temperature, VRAM usage).
> Uses 2 GPIO pins (I2C1 SDA + SCL), independent of the host I2C0 management bus.
>
> See `GPU_API_Design.md` §8 for the full tiered memory architecture.