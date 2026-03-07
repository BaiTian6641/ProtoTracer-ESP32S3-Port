# Unified Display Frontend Design

> **ProtoGL v0.7 — Display Abstraction Layer**
> 
> This document defines the unified display driver interface for ProtoGL GPU firmware,
> enabling the same rendering pipeline to output to multiple display types across
> different platforms without modifying the core rasterizer or host-side API.

---

## §1 Motivation

The current RP2350 GPU firmware is tightly coupled to a single display output: a 128×64 HUB75 LED panel driven by PIO. While this works well for the Protogen use case, ProtoGL aspires to be a general-purpose graphics API comparable to Vulkan — supporting diverse displays (OLED, LCD, DVI monitors, LED panels) across different GPU platforms (RP2350, RP2040, ESP32-P4, FPGA, custom RISC-V).

### Current Limitations

| Area | Current State | Desired State |
|------|--------------|---------------|
| Display type | HUB75 only (hardcoded) | Any display via unified interface |
| Resolution | 128×64 fixed at compile time | Runtime-configurable |
| Color depth | RGB565 framebuffer only | RGB565, RGB888, ARGB8888, indexed |
| Output count | Single display | Multiple simultaneous outputs |
| Platform | RP2350 PIO only | Platform-agnostic driver model |
| Framebuffer ownership | Always MCU-hosted | Aware of display-side GDDRAM |
| Write efficiency | CPU-loop per pixel | DMA fill + chunk-skip optimisation |

### Design Goals

1. **Interface Uniformity** — One abstract `DisplayDriver` interface for all display types
2. **Framebuffer Ownership Awareness** — Distinguish between self-buffered displays (on-chip GDDRAM) and host-buffered displays (MCU-maintained scanout)
3. **Zero-Copy Where Possible** — DMA from framebuffer directly, no intermediate copy
4. **DMA Chunk-Fill & Skip** — Detect uniform-colour regions and offload fills to DMA, freeing the CPU for rendering
5. **Hot-Pluggable** — Register/unregister display drivers at runtime
6. **Multi-Output** — Route different render targets to different physical displays
7. **Platform Portable** — Same interface works on RP2350, ESP32-P4, FPGA, etc.
8. **Backward Compatible** — Existing HUB75 behavior preserved as the default driver

---

## §2 Architecture Overview

```
┌───────────────────────────────────────────────────────────┐
│                    GPU Core Pipeline                       │
│ ┌──────────┐   ┌──────────┐   ┌────────────┐             │
│ │ Rasterize│──▶│ Shaders  │──▶│ Composit.  │             │
│ └──────────┘   └──────────┘   └─────┬──────┘             │
│                                      │                     │
│                              ┌───────▼───────┐            │
│                              │ DisplayManager│            │
│                              │  (Routing)    │            │
│                              └───┬───┬───┬───┘            │
│                                  │   │   │                 │
│                          ┌───────┘   │   └───────┐        │
│                          ▼           ▼           ▼        │
│                    ┌──────────┐ ┌──────────┐ ┌──────────┐ │
│                    │HUB75Drv  │ │DviDriver │ │SpiLcdDrv │ │
│                    │(PIO0)    │ │(PIO1/DVI)│ │(SPI+DMA) │ │
│                    └──────────┘ └──────────┘ └──────────┘ │
│                          │           │           │        │
└──────────────────────────┼───────────┼───────────┼────────┘
                           ▼           ▼           ▼
                     HUB75 Panel   DVI-D Monitor  SPI LCD
```

### Component Responsibilities

| Component | Responsibility |
|-----------|---------------|
| `DisplayDriver` (abstract) | Interface for all display backends |
| `DisplayManager` | Registry, routing, framebuffer format negotiation, dirty tracking |
| `Hub75DisplayDriver` | PIO-driven HUB75 with BCM — **host-buffered** |
| `DviDisplayDriver` | PIO-driven DVI-D/HDMI via bit-banged TMDS — **host-buffered** |
| `SpiDisplayDriver` | SPI+DMA for SSD1331, SSD1306, SSD1351, ST7789, etc. — **mixed** |
| `QspiDisplayDriver` | PIO-driven QSPI for RM67162, etc. — **self-buffered** |
| `ParallelDisplayDriver` | 8/16-bit parallel MCU interface (8080/6800) — **self-buffered** |
| `DmaFillEngine` | Chunk-skip/constant-fill DMA helper for framebuffer writes |

### Framebuffer Ownership Model

Display ICs fall into two fundamental categories that affect how the GPU firmware
interacts with them:

| Category | MCU Framebuffer? | Scanout By | Examples |
|----------|-----------------|------------|----------|
| **Host-buffered** | Yes — MCU maintains continuous scanout buffer | MCU hardware (PIO/DMA) drives display pins at refresh rate | HUB75 (PIO BCM), DVI-D (PIO TMDS) |
| **Self-buffered** | Optional — display IC has internal GDDRAM | Display IC scans GDDRAM autonomously | SSD1306 (1 KB), SSD1331 (48 KB), SSD1351 (128 KB), ST7789, ILI9341, RM67162 |

**Implications for Self-Buffered Displays:**

- MCU only needs to push **changed pixels** (dirty rectangles), not the whole frame
- No continuous refresh loop required — once data is in GDDRAM the display shows it
- SRAM savings: the MCU can use a **shadow buffer** (for dirty tracking) or no local
  framebuffer at all if the application can reconstruct regions on demand
- `SwapBuffers()` becomes a batch SPI/I2C push of dirty regions, not a buffer pointer swap

**Implications for Host-Buffered Displays:**

- MCU must maintain a full framebuffer in SRAM and continuously drive the display
- `PollRefresh()` or DMA IRQ must keep running even between frames
- Double-buffering is essential to avoid tearing

#### GDDRAM Map of Common Self-Buffered Display ICs

| Display IC | Resolution | Color | GDDRAM Size | Interface | Refresh |
|-----------|------------|-------|-------------|-----------|----------|
| SSD1306 | 128×64 | Mono 1-bit | 1 KB | SPI / I2C | Self-refresh |
| SSD1309 | 128×64 | Mono 1-bit | 1 KB | SPI / I2C | Self-refresh |
| SSD1331 | 96×64 | RGB565 | 12 KB | SPI | Self-refresh |
| SSD1351 | 128×128 | RGB565/RGB666 | 32–48 KB | SPI | Self-refresh |
| ST7789 | 240×240 / 240×320 | RGB565/RGB666 | 115–172 KB | SPI | Self-refresh |
| ILI9341 | 240×320 | RGB565/RGB666 | 172 KB | SPI / Parallel | Self-refresh |
| GC9A01 | 240×240 | RGB565 | 115 KB | SPI | Self-refresh |
| RM67162 | 536×240 | RGB565/RGB888 | 257 KB | QSPI | Self-refresh + TE |
| SH1107 | 128×128 | Mono 1-bit | 2 KB | SPI / I2C | Self-refresh |

> **HUD Displays:** SSD1306, SSD1309, and SSD1331 are planned as secondary HUD
> (heads-up display) outputs driven by a separate device. These small self-buffered
> displays complement the primary HUB75 matrix by showing status, debug info, or
> overlays independently of the main render pipeline.

---

## §3 Display Driver Interface

### §3.1 Core Abstract Interface

```cpp
/// Display pixel format
enum PglPixelFormat : uint8_t {
    PGL_PIXEL_RGB565     = 0x00,  // 16-bit, current default
    PGL_PIXEL_RGB888     = 0x01,  // 24-bit packed
    PGL_PIXEL_ARGB8888   = 0x02,  // 32-bit with alpha
    PGL_PIXEL_RGB332     = 0x03,  // 8-bit for tiny displays
    PGL_PIXEL_INDEXED8   = 0x04,  // 8-bit indexed (256-color palette)
    PGL_PIXEL_MONO1      = 0x05,  // 1-bit monochrome
};

/// Display refresh mode
enum PglRefreshMode : uint8_t {
    PGL_REFRESH_CONTINUOUS = 0x00,  // Driver handles refresh (HUB75 BCM polling)
    PGL_REFRESH_VSYNC      = 0x01,  // Wait for vertical blank before swap
    PGL_REFRESH_ON_DEMAND  = 0x02,  // Push framebuffer explicitly (SPI displays)
    PGL_REFRESH_DMA_AUTO   = 0x03,  // DMA auto-transfer (fire and forget)
};

/// Display capability flags
enum PglDisplayCaps : uint16_t {
    PGL_DISP_CAP_DOUBLE_BUFFER = 0x0001,  // Hardware supports double buffering
    PGL_DISP_CAP_VSYNC         = 0x0002,  // Vertical sync signal available
    PGL_DISP_CAP_DMA           = 0x0004,  // DMA transfer supported
    PGL_DISP_CAP_ROTATION      = 0x0008,  // Hardware rotation support
    PGL_DISP_CAP_BRIGHTNESS    = 0x0010,  // Brightness control
    PGL_DISP_CAP_GAMMA         = 0x0020,  // Gamma table support
    PGL_DISP_CAP_PARTIAL       = 0x0040,  // Partial refresh (dirty rectangles)
    PGL_DISP_CAP_TEARFREE      = 0x0080,  // Tear-free display (LTDC, DVI)
    PGL_DISP_CAP_MULTI_LAYER   = 0x0100,  // Hardware layer compositing
    PGL_DISP_CAP_SELF_BUFFERED = 0x0200,  // Display has internal GDDRAM
    PGL_DISP_CAP_DIRTY_RECT    = 0x0400,  // Driver supports dirty-rectangle push
    PGL_DISP_CAP_HW_SCROLL     = 0x0800,  // Hardware scroll (SSD1306, SSD1351)
    PGL_DISP_CAP_HW_INVERT     = 0x1000,  // Hardware display invert
};

/// Framebuffer ownership model
enum PglFramebufferOwner : uint8_t {
    PGL_FB_HOST_OWNED   = 0x00,  // MCU maintains framebuffer, continuous scanout
    PGL_FB_DISPLAY_OWNED = 0x01,  // Display IC has GDDRAM, MCU pushes changes
    PGL_FB_SHARED        = 0x02,  // Both — MCU shadow + display GDDRAM (dirty tracking)
};

/// Display configuration returned by driver
struct PglDisplayInfo {
    uint16_t           width;            // Native width in pixels
    uint16_t           height;           // Native height in pixels
    PglPixelFormat     nativeFormat;     // Preferred pixel format
    PglRefreshMode     refreshMode;      // How the display updates
    uint16_t           refreshRateHz;    // Target refresh rate
    uint16_t           capabilities;     // PglDisplayCaps bitmask
    uint32_t           framebufferSize;  // Bytes needed per framebuffer
    PglFramebufferOwner fbOwner;         // Who owns the framebuffer
    uint32_t           gddramSize;       // Display-side GDDRAM in bytes (0 = none)
    const char*        driverName;       // Human-readable name (e.g. "HUB75-PIO")
};

/// Abstract display driver interface.
/// All display backends implement this interface.
class DisplayDriver {
public:
    virtual ~DisplayDriver() = default;

    /// Initialize the display hardware.
    /// @param config  Platform-specific display configuration.
    /// @return true on success.
    virtual bool Initialize(const void* config) = 0;

    /// Shutdown the display and release hardware resources.
    virtual void Shutdown() = 0;

    /// Query display capabilities and native configuration.
    virtual PglDisplayInfo GetInfo() const = 0;

    /// Set the framebuffer pointer for display output.
    /// Depending on refresh mode, this may trigger an immediate push
    /// or just register the buffer for the next refresh cycle.
    /// @param framebuffer  Pointer to pixel data in native format.
    virtual void SetFramebuffer(const void* framebuffer) = 0;

    /// Trigger a framebuffer swap / push to the display.
    /// For CONTINUOUS mode: no-op (driver handles refresh internally).
    /// For ON_DEMAND mode: initiates DMA transfer.
    /// For VSYNC mode: waits for blank then swaps.
    virtual void SwapBuffers() = 0;

    /// Poll-based refresh (must be called from main loop for CONTINUOUS mode).
    /// Drives one sub-frame of output (e.g., one BCM plane of HUB75).
    /// For DMA/VSYNC displays, this is a no-op.
    virtual void PollRefresh() = 0;

    /// Check if a previously initiated transfer is still in progress.
    virtual bool IsBusy() const = 0;

    // ── Optional Control ────────────────────────────────────────────

    /// Set display brightness (0–255). Requires PGL_DISP_CAP_BRIGHTNESS.
    virtual void SetBrightness(uint8_t brightness) { (void)brightness; }

    /// Set hardware rotation (0, 90, 180, 270). Requires PGL_DISP_CAP_ROTATION.
    virtual void SetRotation(uint16_t degrees) { (void)degrees; }

    /// Load gamma correction table. Requires PGL_DISP_CAP_GAMMA.
    virtual void SetGammaTable(const uint8_t* table) { (void)table; }

    /// Push a partial region (dirty rectangle). Requires PGL_DISP_CAP_PARTIAL.
    virtual void PushRegion(uint16_t x, uint16_t y,
                            uint16_t w, uint16_t h, const void* data) {
        (void)x; (void)y; (void)w; (void)h; (void)data;
    }

    /// Fill display with a solid color (fast clear).
    virtual void Clear(uint32_t color = 0) { (void)color; }

    // ── Self-Buffered (GDDRAM) Display Helpers ──────────────────────

    /// Mark a rectangular region as dirty (needs re-push to GDDRAM).
    /// For self-buffered displays only (PGL_FB_DISPLAY_OWNED / PGL_FB_SHARED).
    virtual void MarkDirty(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
        (void)x; (void)y; (void)w; (void)h;
    }

    /// Flush all dirty regions to GDDRAM via SPI/I2C DMA.
    /// For host-buffered displays this is a no-op (they flush via SwapBuffers).
    virtual void FlushDirty() {}

    /// Query whether the display owns its framebuffer (has GDDRAM).
    bool IsSelfBuffered() const {
        return (GetInfo().capabilities & PGL_DISP_CAP_SELF_BUFFERED) != 0;
    }

    /// Activate hardware-accelerated scroll (SSD1306, SSD1351).
    /// Requires PGL_DISP_CAP_HW_SCROLL.
    virtual void SetHardwareScroll(int16_t dx, int16_t dy) {
        (void)dx; (void)dy;
    }
};
```

### §3.2 Display Manager

```cpp
/// Maximum number of simultaneously registered display drivers.
static constexpr uint8_t PGL_MAX_DISPLAYS = 4;

/// Display output routing — which render target goes to which display.
struct PglDisplayRoute {
    uint8_t  renderTargetId;  // Render target / camera output (0 = primary)
    uint8_t  displayIndex;    // Registered display driver index
    uint8_t  layerId;         // For multi-layer displays, which layer
    bool     enabled;
};

/// Display Manager — owns the registry and routes render targets to outputs.
class DisplayManager {
public:
    /// Register a display driver. Returns display index (0–3) or -1 on failure.
    int8_t RegisterDisplay(DisplayDriver* driver, const void* config);

    /// Unregister a display driver.
    void UnregisterDisplay(int8_t displayIndex);

    /// Get a registered display driver.
    DisplayDriver* GetDisplay(int8_t index);

    /// Configure output routing.
    void SetRoute(uint8_t routeIndex, const PglDisplayRoute& route);

    /// Called after rasterization: routes framebuffers to displays.
    void PresentFrame(const void** framebuffers, uint8_t count);

    /// Poll all continuous-refresh displays (call from main loop).
    void PollAll();

    /// Wait for all displays to finish any pending transfers.
    void WaitAll();

    /// Query all display capabilities for host reporting.
    uint8_t GetDisplayCount() const;
    PglDisplayInfo GetDisplayInfo(uint8_t index) const;

private:
    DisplayDriver*    displays_[PGL_MAX_DISPLAYS] = {};
    PglDisplayRoute   routes_[PGL_MAX_DISPLAYS]   = {};
    uint8_t           displayCount_ = 0;
};
```

---

## §4 RP2350 Display Driver Implementations

### §4.1 HUB75 Display Driver (PIO0)

Refactored from the existing `Hub75Driver` namespace into a class implementing `DisplayDriver`.

```cpp
struct Hub75Config {
    // Data pins (active-low accent, directly mapped to PIO)
    int8_t  rgbPins[6];      // R1, G1, B1, R2, G2, B2
    int8_t  addrPins[5];     // A, B, C, D, E (up to 64 rows)
    int8_t  clkPin;
    int8_t  latPin;
    int8_t  oePin;

    uint16_t panelWidth;     // Pixels per row (default: 128)
    uint16_t panelHeight;    // Total rows (default: 64)
    uint8_t  scanRate;       // 1/N scan (default: 32 for 1/32 scan)
    uint8_t  bcmBitDepth;    // BCM bit planes (default: 8)
    uint8_t  brightness;     // Global brightness 0–255
    uint8_t  colorOrder;     // RGB, GRB, BGR, etc.

    // PIO allocation
    uint8_t  pioInstance;    // 0 = PIO0 (default)
    uint8_t  dataSmIndex;    // State machine for data shift (default: 0)
    uint8_t  rowSmIndex;     // State machine for row control (default: 1)
};

class Hub75DisplayDriver : public DisplayDriver {
public:
    bool Initialize(const void* config) override;
    void Shutdown() override;
    PglDisplayInfo GetInfo() const override;
    // GetInfo() returns: fbOwner = PGL_FB_HOST_OWNED, gddramSize = 0
    void SetFramebuffer(const void* framebuffer) override;
    void SwapBuffers() override;
    void PollRefresh() override;   // REQUIRED — drives BCM plane scanout
    bool IsBusy() const override;
    void SetBrightness(uint8_t brightness) override;
    void Clear(uint32_t color) override;

    /// HUB75-specific: get current refresh rate.
    uint32_t GetRefreshRate() const;

    /// HUB75-specific: fill with test pattern.
    void FillTestPattern(uint16_t* buffer, uint8_t pattern);
};
```

**PIO Resource Usage:** PIO0 SM0 (data shift + CLK side-set) + PIO0 SM1 (LAT/OE/row address)

**Framebuffer Format:** RGB565 (16-bit) — converted to 6-bit per pixel (R1G1B1R2G2B2) during PIO output, with BCM modulation for brightness depth.

### §4.2 DVI-D Display Driver (PIO)

DVI-D output via PIO-driven TMDS encoding, following the PicoDVI approach. Supports standard monitor resolutions.

```cpp
struct DviConfig {
    // TMDS differential pairs (active-high/low, directly to PIO)
    int8_t  tmdsDataPins[6];   // D0+, D0-, D1+, D1-, D2+, D2-
    int8_t  tmdsClkPins[2];    // CLK+, CLK-

    // Resolution / timing (predefined modes)
    enum DviMode : uint8_t {
        DVI_640x480_60Hz   = 0,   // 25.175 MHz pixel clock
        DVI_320x240_60Hz   = 1,   // Pixel-doubled 640x480
        DVI_800x600_60Hz   = 2,   // 40 MHz pixel clock
        DVI_160x120_60Hz   = 3,   // Pixel-quadrupled 640x480
        DVI_CUSTOM         = 0xFF,
    } mode;

    // PIO allocation
    uint8_t pioInstance;     // Which PIO block (0, 1, or 2)
    uint8_t smStartIndex;   // Starting SM index (uses 3 SMs for TMDS)

    // Optional
    uint16_t hActive;        // For CUSTOM mode
    uint16_t vActive;        // For CUSTOM mode
    uint32_t pixelClockHz;   // For CUSTOM mode
};

class DviDisplayDriver : public DisplayDriver {
public:
    bool Initialize(const void* config) override;
    void Shutdown() override;
    PglDisplayInfo GetInfo() const override;
    // GetInfo() returns: fbOwner = PGL_FB_HOST_OWNED, gddramSize = 0
    void SetFramebuffer(const void* framebuffer) override;
    void SwapBuffers() override;
    void PollRefresh() override;  // No-op — DMA-driven scanout
    bool IsBusy() const override;

    /// DVI-specific: set output resolution mode.
    bool SetMode(DviConfig::DviMode mode);
};
```

**PIO Resource Usage:** 3 state machines (one per TMDS data channel), each performing 10-bit serialization at pixel clock × 10. DMA feeds pre-encoded TMDS symbols.

**Framebuffer Format:** RGB565 or RGB888 internal → TMDS-encoded scanline buffers (generated per-line via DMA IRQ).

### §4.3 SPI Display Driver

For SPI-connected displays (SSD1331, ST7789, ILI9341, GC9A01, SH1107, etc.) using the MCU's internal SPI peripheral with DMA.

```cpp
struct SpiDisplayConfig {
    int8_t   mosiPin;
    int8_t   sclkPin;
    int8_t   csPin;
    int8_t   dcPin;           // Data/Command select
    int8_t   rstPin;          // Hardware reset (-1 if not used)
    int8_t   blPin;           // Backlight PWM pin (-1 if not used)

    uint32_t spiClockHz;      // SPI bus speed (e.g., 40000000 for 40 MHz)
    uint8_t  spiInstance;     // SPI0 or SPI1

    // Display controller IC
    enum SpiController : uint8_t {
        SPI_CTRL_SSD1331  = 0,   // 96x64 RGB OLED
        SPI_CTRL_ST7789   = 1,   // 240x240 / 240x320 IPS LCD
        SPI_CTRL_ILI9341  = 2,   // 240x320 TFT LCD
        SPI_CTRL_GC9A01   = 3,   // 240x240 round IPS
        SPI_CTRL_SH1107   = 4,   // 128x128 / 64x128 mono OLED
        SPI_CTRL_SSD1306  = 5,   // 128x64 / 128x32 mono OLED
        SPI_CTRL_CUSTOM   = 0xFF,
    } controller;

    uint16_t width;
    uint16_t height;
    uint8_t  rotation;         // 0, 90, 180, 270
    PglPixelFormat format;     // Native pixel format
};

class SpiDisplayDriver : public DisplayDriver {
public:
    bool Initialize(const void* config) override;
    void Shutdown() override;
    PglDisplayInfo GetInfo() const override;
    // GetInfo() returns: fbOwner = PGL_FB_DISPLAY_OWNED, gddramSize = per-IC
    //   SSD1306: 1 KB, SSD1331: 12 KB, SSD1351: 48 KB, ST7789: 172 KB
    void SetFramebuffer(const void* framebuffer) override;
    void SwapBuffers() override;   // Pushes dirty regions to GDDRAM
    void PollRefresh() override;   // No-op — display self-refreshes from GDDRAM
    bool IsBusy() const override;
    void SetBrightness(uint8_t brightness) override;
    void SetRotation(uint16_t degrees) override;
    void PushRegion(uint16_t x, uint16_t y,
                    uint16_t w, uint16_t h, const void* data) override;
    void Clear(uint32_t color) override;
    void MarkDirty(uint16_t x, uint16_t y, uint16_t w, uint16_t h) override;
    void FlushDirty() override;
    void SetHardwareScroll(int16_t dx, int16_t dy) override;  // SSD1306/SSD1351

    /// SPI-specific: send raw command to display controller.
    void SendCommand(uint8_t cmd, const uint8_t* params = nullptr, uint8_t len = 0);

private:
    /// Dirty rectangle tracking for GDDRAM displays.
    /// Maintains a bounding box of all MarkDirty() calls since last FlushDirty().
    struct DirtyRect {
        uint16_t x0, y0, x1, y1;
        bool     any;
    } dirty_ = {0, 0, 0, 0, false};
};
```

**DMA Strategy:** Full-framebuffer DMA push using the SPI peripheral's DMA channel. For partial updates, only the dirty region is transferred (requires `PGL_DISP_CAP_PARTIAL`).

**Framebuffer Ownership:** SPI displays with GDDRAM (`SSD1306`, `SSD1331`, `SSD1351`, `ST7789`, `ILI9341`, `GC9A01`) report `PGL_FB_DISPLAY_OWNED`. The driver tracks dirty rectangles via `MarkDirty()` and batches them into a single SPI DMA push during `FlushDirty()` / `SwapBuffers()`. If no pixels changed, `SwapBuffers()` skips the SPI transfer entirely — saving bus bandwidth and power. For displays without GDDRAM (theoretical SPI pass-through), `PGL_FB_HOST_OWNED` is used.

### §4.4 QSPI Display Driver (PIO)

For high-bandwidth QSPI displays (RM67162 AMOLED, etc.) using PIO for 4-lane SPI.

```cpp
struct QspiDisplayConfig {
    int8_t   dataPins[4];     // QSPI D0–D3
    int8_t   sclkPin;
    int8_t   csPin;
    int8_t   dcPin;           // Data/Command (some QSPI displays use inline command)
    int8_t   rstPin;
    int8_t   tePin;           // Tearing effect pin (-1 if not used)

    uint32_t spiClockHz;      // QSPI clock speed (e.g., 80 MHz)
    uint8_t  pioInstance;     // PIO block for QSPI

    enum QspiController : uint8_t {
        QSPI_CTRL_RM67162 = 0,  // 536x240 AMOLED
        QSPI_CTRL_CUSTOM  = 0xFF,
    } controller;

    uint16_t width;
    uint16_t height;
    PglPixelFormat format;
};

class QspiDisplayDriver : public DisplayDriver {
public:
    bool Initialize(const void* config) override;
    void Shutdown() override;
    PglDisplayInfo GetInfo() const override;
    void SetFramebuffer(const void* framebuffer) override;
    void SwapBuffers() override;
    void PollRefresh() override;  // No-op if TE pin driven
    bool IsBusy() const override;
    void SetBrightness(uint8_t brightness) override;
    void PushRegion(uint16_t x, uint16_t y,
                    uint16_t w, uint16_t h, const void* data) override;
};
```

**PIO Resource Usage:** 1 state machine for 4-bit output + CLK side-set, DMA-driven. Uses PIO QSPI program similar to `octal_spi_rx` but in TX mode.

**Framebuffer Ownership:** QSPI displays (RM67162, etc.) have large GDDRAM and report `PGL_FB_DISPLAY_OWNED`. With TE-pin synchronization and dirty-rectangle push, the MCU framebuffer can be reduced to a shadow buffer or eliminated entirely for static content.

### §4.5 Parallel Interface Display Driver

For classic 8080/6800-style parallel MCU interface displays (many TFT modules).

```cpp
struct ParallelDisplayConfig {
    int8_t   dataPins[16];    // D0–D7 (8-bit) or D0–D15 (16-bit)
    int8_t   wrPin;           // Write strobe
    int8_t   rdPin;           // Read strobe (-1 if not used)
    int8_t   csPin;
    int8_t   dcPin;           // Data/Command
    int8_t   rstPin;

    uint8_t  busWidth;        // 8 or 16
    uint8_t  interfaceType;   // 0 = 8080 (Intel), 1 = 6800 (Motorola)

    enum ParallelController : uint8_t {
        PAR_CTRL_ILI9341  = 0,
        PAR_CTRL_ILI9488  = 1,
        PAR_CTRL_NT35510  = 2,
        PAR_CTRL_CUSTOM   = 0xFF,
    } controller;

    uint16_t width;
    uint16_t height;
    PglPixelFormat format;
};

class ParallelDisplayDriver : public DisplayDriver {
public:
    bool Initialize(const void* config) override;
    void Shutdown() override;
    PglDisplayInfo GetInfo() const override;
    void SetFramebuffer(const void* framebuffer) override;
    void SwapBuffers() override;
    void PollRefresh() override;
    bool IsBusy() const override;
    void SetBrightness(uint8_t brightness) override;
    void PushRegion(uint16_t x, uint16_t y,
                    uint16_t w, uint16_t h, const void* data) override;
};
```

**Implementation Note:** On RP2350, the parallel interface can be driven via PIO (1 SM for 8-bit + WR strobe), or on ESP32-P4 via the native LCD parallel peripheral. The `DisplayDriver` interface abstracts the platform-specific implementation.

---

## §5 Host-Side Display Configuration Protocol

### §5.1 GPU Registers (SPI Read / I2C Fallback)

| Register | Address | R/W | Size | Description |
|----------|---------|-----|------|-------------|
| `PGL_REG_DISPLAY_COUNT` | 0x14 | R | 1 | Number of active display drivers |
| `PGL_REG_DISPLAY_INFO` | 0x15 | R | 20 | Info for display N (includes fbOwner + gddramSize) |
| `PGL_REG_DISPLAY_CONFIG` | 0x16 | W | var | Configure display N (driver-specific) |
| `PGL_REG_DISPLAY_ROUTE` | 0x17 | W | 4 | Set output routing |
| `PGL_REG_DMA_FILL_THRESHOLD` | 0x1E | R/W | 2 | DMA fill threshold in pixels (default 128) |
| `PGL_REG_DIRTY_STATS` | 0x1D | R | 8 | Dirty-rect stats: pushed/skipped bytes last frame |

### §5.2 New SPI Commands

| Opcode | Name | Description |
|--------|------|-------------|
| `0x90` | `CMD_SET_DISPLAY_CONFIG` | Configure a display driver's parameters |
| `0x91` | `CMD_SET_DISPLAY_ROUTE` | Route a render target to a display |
| `0x92` | `CMD_SET_RENDER_TARGET` | Create/resize a render target |

### §5.3 Wire-Format Additions

```cpp
// CMD_SET_DISPLAY_CONFIG (0x90)
struct PglCmdSetDisplayConfig {
    uint8_t  displayIndex;    // Which display to configure
    uint16_t width;           // Desired resolution
    uint16_t height;
    uint8_t  pixelFormat;     // PglPixelFormat
    uint8_t  refreshMode;     // PglRefreshMode
    uint8_t  configDataSize;  // Size of driver-specific config blob
    // Followed by configDataSize bytes of driver-specific configuration
};

// CMD_SET_DISPLAY_ROUTE (0x91)
struct PglCmdSetDisplayRoute {
    uint8_t renderTargetId;   // Source render target
    uint8_t displayIndex;     // Destination display
    uint8_t layerId;          // Display layer (for multi-layer)
    uint8_t flags;            // bit0: enabled
};

// CMD_SET_RENDER_TARGET (0x92)
struct PglCmdSetRenderTarget {
    uint8_t  renderTargetId;  // 0 = primary, 1-3 = auxiliary
    uint16_t width;
    uint16_t height;
    uint8_t  pixelFormat;     // PglPixelFormat
    uint8_t  flags;           // bit0: double-buffered, bit1: has Z-buffer
};
```

---

## §6 RP2350 PIO Resource Allocation (Updated)

With multiple display types supported, PIO blocks must be allocated carefully:

| PIO Block | Default Allocation | Alternative |
|-----------|-------------------|-------------|
| **PIO0** | HUB75 display (SM0 data, SM1 row) | Parallel display (SM0) |
| **PIO1** | Octal SPI bidirectional (SM0 RX + SM1 TX) | QSPI display (SM0-SM1) only if Octal SPI not used |
| **PIO2** | QSPI VRAM (dual channel, RP2350B) | DVI TMDS (SM0-SM2) if no external VRAM needed |

For DVI output, PIO2 must be repurposed (no external VRAM). On RP2350A, PIO2 is always available for DVI since there is no external VRAM support. The GPU firmware detects the display configuration and chip variant at boot and allocates PIOs accordingly:

```
Display + Memory Profiles:
┌──────────────────────────────────────────────────┐
│ Profile A: HUB75 + QSPI VRAM (RP2350B)           │
│   PIO0 = HUB75                                    │
│   PIO1 = Octal SPI (bidir, SM0+SM1)               │
│   PIO2 = QSPI VRAM (Ch-A: SM0+SM1, Ch-B: SM2+SM3)│
│   I2C1 = HUD OLED (optional, 128×64 mono)         │
├──────────────────────────────────────────────────┤
│ Profile B: DVI + No External VRAM (RP2350A/B)     │
│   PIO0 = Octal SPI (bidir, SM0+SM1)               │
│   PIO1 = (unused)                                  │
│   PIO2 = DVI TMDS (3 SMs)                         │
├──────────────────────────────────────────────────┤
│ Profile C: SPI LCD + QSPI VRAM (RP2350B)          │
│   PIO0 = (unused for display)                      │
│   PIO1 = Octal SPI (bidir, SM0+SM1)               │
│   PIO2 = QSPI VRAM (Ch-A: SM0+SM1, Ch-B: SM2+SM3)│
│   SPI0 = LCD display (DMA)                        │
│   I2C1 = HUD OLED (optional)                      │
├──────────────────────────────────────────────────┤
│ Profile D: QSPI LCD + QSPI VRAM (RP2350B)         │
│   PIO0 = QSPI display (SM0)                       │
│   PIO1 = Octal SPI (bidir, SM0+SM1)               │
│   PIO2 = QSPI VRAM (Ch-A: SM0+SM1, Ch-B: SM2+SM3)│
├──────────────────────────────────────────────────┤
│ Profile E: HUB75 + DVI (mirrored output)          │
│   PIO0 = HUB75                                    │
│   PIO1 = Octal SPI (bidir, SM0+SM1)               │
│   PIO2 = DVI TMDS (3 SMs) — no external VRAM     │
│   (RP2350A native, or RP2350B sacrificing VRAM)   │
└──────────────────────────────────────────────────┘
```

---

## §7 Framebuffer Format Conversion & DMA Optimizations

### §7.0 DMA Chunk-Fill & Skip Optimization

The RP2350 DMA controller supports a **constant-source fill** mode: a DMA channel
can be configured with `read_increment = false` while `write_increment = true`,
causing it to blast a single value across a contiguous memory range at bus speed
(up to 150 MHz × 32-bit = 600 MB/s internal). This is significantly faster than a
CPU `memset`/fill loop (~40–80 MB/s) and — critically — **frees the CPU core**
to continue rendering or running shaders while the fill completes.

#### Use Cases

1. **Full-screen clear** — `Clear(uint32_t color)` on host-buffered displays triggers
   a single DMA fill of the entire framebuffer.
2. **Large solid rectangles** — 2D draw commands like `CMD_FILL_RECT` that cover
   more than a configurable threshold (e.g. 128 pixels ≈ 256 bytes at RGB565) are
   promoted to a DMA fill instead of a CPU rasterizer loop.
3. **Background layer fill** — The 2D layer compositor's bottom-most layer clear is
   always DMA-accelerated.
4. **Chunk skip** — When dirty-rect tracking determines that a region of the
   framebuffer is unchanged between frames, the display push can skip that region
   entirely (no transfer to GDDRAM). For host-buffered displays, the render
   pipeline can skip re-rasterizing unchanged tiles.

#### DMA Fill Engine

```cpp
/// DMA-accelerated fill for framebuffer / SRAM regions.
class DmaFillEngine {
public:
    /// Initialize with a dedicated DMA channel.
    void Init(uint8_t dmaChannel);

    /// Start a constant-fill DMA transfer.
    /// @param dst     Destination address (must be word-aligned for best perf).
    /// @param value   The fill value (16-bit or 32-bit depending on pixel format).
    /// @param bytes   Number of bytes to fill.
    void StartFill(void* dst, uint32_t value, uint32_t bytes);

    /// Start a DMA memcpy (for region blit).
    void StartCopy(void* dst, const void* src, uint32_t bytes);

    /// Poll completion (non-blocking).
    bool IsBusy() const;

    /// Block until fill completes.
    void Wait();

private:
    uint8_t channel_;
};
```

#### Integration Points

| Call Site | When DMA Fill Is Used |
|-----------|----------------------|
| `DisplayDriver::Clear()` | Always — replaces CPU loop |
| `Rasterizer2D::FillRect()` | If area ≥ `PGL_DMA_FILL_THRESHOLD` pixels |
| `Compositor::ClearLayer()` | Always — bottom layer wipe |
| `SpiDisplayDriver::FlushDirty()` | Skip unchanged regions (chunk skip) |
| `Hub75DisplayDriver::SwapBuffers()` | DMA copy back→front buffer |

#### CPU-vs-DMA Decision Flow

```
FillRect(x, y, w, h, color)
 ├── area = w × h
 ├── (area < PGL_DMA_FILL_THRESHOLD)  → CPU loop (inline, low overhead)
 └── (area ≥ PGL_DMA_FILL_THRESHOLD)  → DmaFillEngine::StartFill()
                                           └── CPU free → continue rendering
                                           └── DMA IRQ or poll → done
```

`PGL_DMA_FILL_THRESHOLD` default: 128 pixels (256 bytes at RGB565). Configurable
via SPI read command `SPI_READ_DIRTY_STATS` (0xE9) or I2C register `PGL_REG_DMA_FILL_THRESHOLD` (address 0x1E).

#### Chunk-Skip for Self-Buffered Displays

When pushing a frame to a GDDRAM display (SSD1331, SSD1351, ST7789, etc.), the
driver compares dirty-rect regions with the previous frame (if a shadow buffer
exists). Contiguous unchanged scanlines within a dirty rect are **skipped** —
the SPI window address is advanced past them without sending pixel data. This
reduces SPI bus utilization significantly for partially-animated frames (e.g. a
small avatar mouth moving while the rest of the face is static).

```
Dirty rect covers rows 10–50
  Row 10–15: changed   → push via SPI DMA
  Row 16–40: unchanged → skip (advance GDDRAM column/page pointer)
  Row 41–50: changed   → push via SPI DMA
  Total: 16 rows pushed instead of 41 → 61% bandwidth saved
```

When the render pipeline's internal format differs from the display's native format, the `DisplayManager` performs conversion:

| Internal Format | Display Format | Conversion |
|----------------|---------------|------------|
| RGB565 | RGB565 | Zero-copy (direct DMA) |
| RGB565 | RGB888 | Expand: R5→R8, G6→G8, B5→B8 |
| RGB565 | ARGB8888 | Expand + alpha=0xFF |
| RGB565 | Mono1 | Luminance threshold |
| RGB888 | RGB565 | Truncate (lossy) |
| RGB888 | RGB888 | Zero-copy |
| ARGB8888 | RGB565 | Alpha-composite then truncate |

For performance-critical paths (HUB75 BCM), the conversion happens during the PIO output stage (existing behavior — RGB565 → 6-bit per pixel is done inline).

---

## §8 Multi-Display & Render Target Management

### §8.1 Render Targets

A render target is a GPU-side framebuffer that the rasterizer can write to:

```cpp
static constexpr uint8_t PGL_MAX_RENDER_TARGETS = 4;

struct RenderTarget {
    uint16_t* colorBuffer;    // Pixel data (RGB565 or converted)
    float*    depthBuffer;    // Z-buffer (optional, per render target)
    uint16_t  width;
    uint16_t  height;
    PglPixelFormat format;
    bool      doubleBuffered;
    bool      hasDepthBuffer;
    uint8_t   activeBufferIndex;  // For double buffering
};
```

### §8.2 Routing Architecture

```
Camera 0 ──▶ Render Target 0 ──▶ Display 0 (HUB75, layer 0)
                                 ▶ Display 1 (DVI, layer 0)  [mirrored]

Camera 1 ──▶ Render Target 1 ──▶ Display 1 (DVI, layer 1)   [overlay]

2D Layer ──▶ Render Target 2 ──▶ Display 0 (HUB75, layer 1)  [UI overlay]
```

This enables:
- **Mirrored output**: Same 3D scene on both HUB75 and DVI
- **Multi-camera**: Different viewpoints on different displays
- **UI overlay**: 2D graphics composited on top of 3D scene
- **Debug view**: Depth buffer visualization on secondary display

---

## §9 Host-Side API Extensions (PglEncoder)

```cpp
// New PglEncoder methods for display management
class PglEncoder {
    // ... existing methods ...

    /// Configure a GPU-side render target.
    void SetRenderTarget(uint8_t renderTargetId, uint16_t width, uint16_t height,
                         PglPixelFormat format, bool doubleBuffered, bool hasDepthBuffer);

    /// Route a render target to a display output.
    void SetDisplayRoute(uint8_t renderTargetId, uint8_t displayIndex,
                         uint8_t layerId, bool enabled);

    /// Configure display driver parameters (driver-specific blob).
    void SetDisplayConfig(uint8_t displayIndex, uint16_t width, uint16_t height,
                          PglPixelFormat format, PglRefreshMode refreshMode,
                          const void* driverConfig, uint8_t configSize);
};
```

---

## §10 Implementation Plan

### Phase 1: Interface Abstraction (M11, Week 28-29)
- Define `DisplayDriver` abstract interface
- Refactor `Hub75Driver` namespace → `Hub75DisplayDriver` class
- Implement `DisplayManager` with single-display routing
- All existing behavior preserved — regression tests pass

### Phase 2: SPI Display Support (M11, Week 30)
- Implement `SpiDisplayDriver` for SSD1331 (existing headless code → refactored)
- Add ST7789/ILI9341 init sequences
- DMA push with partial update support

### Phase 3: DVI-D Output (M12, Week 31-32)
- PIO TMDS encoder (based on PicoDVI approach)
- DMA scanline generation with per-line color conversion
- 640x480 and 320x240 modes

### Phase 4: QSPI & Parallel Displays (M12, Week 33)
- PIO QSPI TX driver for RM67162
- Parallel 8080 interface driver

### Phase 5: Multi-Display Routing (M13, Week 34-35)
- Render target management
- Multi-display compositing
- Host-side display configuration API

---

## §11 SRAM Impact Analysis

| Component | SRAM Cost | Notes |
|-----------|-----------|-------|
| `DisplayDriver` vtable | ~40 bytes per driver | Includes new GDDRAM methods |
| `DisplayManager` | ~96 bytes | Registry + routes + dirty tracking |
| `DmaFillEngine` | ~16 bytes | Channel handle + state |
| `RenderTarget` (primary) | 16 KB (128×64 RGB565) | Existing cost |
| `RenderTarget` (secondary) | 16 KB per target | Only if multi-target used |
| DVI scanline buffers | ~5 KB | 2× 640×4 bytes TMDS |
| SPI command buffer | ~256 bytes | Init sequence + command staging |
| Shadow buffer (self-buffered) | 0–16 KB | Only for dirty tracking on GDDRAM displays |
| **Total new overhead** | **~420 bytes** | Without additional render targets or shadow buffers |

### SRAM Savings from Self-Buffered Displays

For self-buffered displays, the MCU does **not** need a continuous-scanout
framebuffer. The savings depend on usage mode:

| Mode | MCU SRAM Used | Notes |
|------|--------------|-------|
| Full shadow buffer | Same as host-buffered | Enables dirty-rect tracking, chunk-skip |
| Partial shadow (tile-based) | 1–4 KB | Shadow individual tiles for delta detection |
| No shadow (command replay) | 0 | Re-issue draw commands for changed regions only |
| Streaming render | ~1 scanline | Render + push one line at a time (very slow, minimal RAM) |

---

## Related Documents

- [Architecture_Split.md](Architecture_Split.md) — Dual-MCU architecture overview
- [GPU_API_Design.md](GPU_API_Design.md) — Full GPU design including memory tiers
- [ProtoGL_API_Spec.md](ProtoGL_API_Spec.md) — Wire-format specification
- [2D_Graphics_And_Compositing.md](2D_Graphics_And_Compositing.md) — 2D/multi-layer design
- [Memory_Management_API.md](Memory_Management_API.md) — Refined memory management
