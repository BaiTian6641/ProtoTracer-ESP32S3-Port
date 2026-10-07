# Path A: ESP32-S31 single-chip renderer and PARLIO HUB75

Part of the [dual roadmap](esp32s31-dual-roadmap.md), 2026-10-08.
Implementation has started on `codex/esp32s31-parlio`; see the
[bring-up log](esp32s31-parlio-bringup.md) for the buildable first probe and
current hardware/integration gates.
This path keeps morph blending, rendering and screenspace effects on S31 and
uses PARLIO/GDMA for panel refresh. It has no external GPU requirement.

## Driver strategy

Use an existing driver where possible. First qualify the official S31
[advanced HUB75 example](https://github.com/espressif/esp-idf/blob/master/examples/peripherals/parlio/parlio_tx/advanced_rgb_led_matrix/README.md)
as a peripheral/pin/continuous-refresh smoke test. Then port and qualify the
MIT [esp-hub75 PARLIO backend](https://github.com/esphome-libs/esp-hub75/tree/main/components/hub75/src/platforms/parlio)
or another demonstrably S31-capable driver for production color/BCM behavior.
Do not begin by writing a complete HUB75 driver or copying the S3 I2S backend.

At inspected revision `d0bb571`, `esp-hub75` needs more than a target define:

- Add S31 target/backend selection in component dependencies, `CMakeLists`,
  platform detection, factory construction and Kconfig/pin handling.
- Audit SoC feature checks, register/GDMA access, clock gating and loop switching.
  Its clock resolver assumes a 160 MHz source; derive S31 rates from the selected
  SDK's clock APIs instead of importing the P4 calculation.
- Validate external DMA allocation caps, alignment/cache visibility and fallback
  under S31. Existing PSRAM allocation and cache code are a starting point,
  not proof that the same path works unchanged on this target.
- Confirm chain mapping, panel driver initialization, blanking, BCM weights,
  CIE/gamma, brightness changes and safe double buffering.

Retain MIT attribution in a fork/port and pin a tested revision. Prefer an
upstreamable, small target port. If that path fails, document the exact API or
hardware gap before choosing a narrow IDF example-derived implementation.

`src/Controllers/TasESP32P4KitV0.h` is useful historical code to inspect, but its
P4 guard/pins, five-bit output and transaction scheduling are not an S31
production baseline. Keep its `.bak` files untouched. Do not describe a
single-bitplane demo as equivalent to the S3 eight-bit output.

## A0: establish the S31 SDK and board

Deliver a separate minimal probe with an exact toolchain/SDK revision and board
definition. Test serial/reboot, actual CPU and PSRAM clocks, PSRAM allocation,
I2C, mic ADC/timer or agreed audio interface, SPI, LittleFS and basic task
affinity. Build the full dependency set before integrating the renderer.

The current `Vector2D::DotProduct` and `Vector3D::DotProduct` call the Xtensa
`dsps_dotprod_f32_ae32` entry point directly. Introduce portable, behavior-tested
dispatch and audit other DSP/FFT calls; defining an S31 board does not remove
those architecture assumptions. Audit FPU use in callbacks/ISRs and the chosen
SDK's FreeRTOS rules. Keep the established sampling/voice behavior initially.

Exit: a reproducible S31 build with working services and measured internal/
external allocation categories. No copied S3 `sdkconfig` defines masquerade
as changes to precompiled SDK libraries.

## A1: static panel and continuous refresh

Define a board pin map for RGB1/RGB2, row address A-D (E only when needed), LAT,
OE and CLK; validate GPIO capabilities/conflicts and actual logic/power wiring.
Use one 64x32 panel first, then the real two-panel chain. Probe stripes, walking
pixels, row/column numbers, gradients and independent panel colors so wiring
errors cannot hide behind a symmetric face.

Run the official PARLIO example at a conservative supported clock; confirm
blanking/LAT/address ordering and continuous scan independently of animation.
Record effective pixel clock and scan refresh. Its simple packing only proves
that the peripheral can drive a panel; full color-depth/brightness work is A2.

Exit: stable static output with documented pin/scan/chain mapping and measured
refresh, no ghosting or output enabled during unsafe latch/address transitions.

## A2: full-color driver and buffer ownership

Integrate the selected library port through a thin `Hub75Output` adapter with
initialization, RGB frame submission, brightness, completion and shutdown.
Use the existing logical 64x32 face and map/mirror to physical 128x32 once;
the panel driver should not require rasterizing the same face twice.

Start with fixed allocations and double buffering. The producer owns a buffer
only before submission and after explicit buffer-switch/release confirmation.
For continuous DMA, a completed submit or absence of a normal trans-done event
does not prove the active scan buffer is reusable. ISR callbacks notify tasks;
they do not pack frames, allocate or call OLED/JSON code. Audit callback/context
placement for cache-disabled flash work. Drain/disable before free or reinit.

Implement/test eight-bit color, the agreed gamma/CIE path, OE brightness and
blanking. Tune refresh, depth and BCM transition policy as separate documented
parameters. Never silently reduce depth or double buffering to claim a faster
rendering result. If allocation fails, return a clear error or enter an explicitly
selected lower-quality profile.

Host pack tests verify the selected format's RGB, BCM/control bits and mapping.
The existing S3 DMA equality test remains an S3 gate; its format cannot be used
as the oracle for a different PARLIO layout. Compare visible color semantics
and qualified waveform timing as well as native PARLIO buffer contents.

Exit: complete color/brightness/mapping coverage, bounded startup/reinit,
allocation-failure behavior, and stable refresh during frame updates.

## A3: integrate the local face renderer

Add `TasESP32S31KitV0.h` or an equivalent small controller that connects the
existing local `Camera`/`Scene` pipeline to `Hub75Output`. Keep transform,
half-float morph storage, culling and material math unchanged first. Use the
arithmetic rectangular pixel grid and retained legacy path for regression.

Preserve animation completion -> published vertices -> raster/effects -> panel
buffer submission. Keep current task ordering until profiling justifies overlap.
If overlap is later added, snapshot materials/effects as well as geometry.
Independent scan refresh does not make live mutable scene data safe to share.

Route BLE projection and decoded PTM/built-ins through the same pixel-frame
output interface. Feed the HUD/OLED preview from the completed logical image.
Keep brightness and absolute hue semantics consistent with both remotes.

Exit: deterministic local-render comparisons, negative-scale/transform/grid
coverage, all enabled material/effect modes, expressions/blink/speech, media and
HUD orientation correct on the actual chain.

## A4: complete service parity and S31 deployment

Port BLE APIs required by the selected Arduino 4 stack without changing the
external JSON ops/chunking/hue contract. Keep Wi-Fi asset startup/teardown and
BLE activation sequencing; change coexistence only as a separately measured
experiment. Validate provisioning, local-asset startup, OTA trigger, status LED,
gesture sensor and all supported M5 display variants.

Add a target/backend-specific OTA manifest and version metadata. Test panel
hold/blank and resume through LittleFS writes and OTA. Verify cache/ISR behavior
under the actual SDK, not just with the display-only probe. Do not flash a
local-backend binary onto the GPU-wired product or vice versa.

Exit: complete service matrix, authenticated OTA/recovery, heap integrity and
mixed-load soak at the agreed presentation target.

## A5: profile and tune the measured bottleneck

Measure animation/morph, raster/material/effects, packing, cache flush, menu/
OLED, microphone and presentation independently. Render FPS and panel refresh
are reported separately. Prioritize prepared math/material work and source-row
reuse only when measured, and test cache invalidation for brightness/gamma,
format/layout changes and reinitialization.

Continuous DMA still reads memory. Track PSRAM contention while rendering,
microphone work and BLE/flash activity occur. Place control/ISR/descriptor data
internally as required; large buffers use qualified external-memory paths.
Retain no per-frame heap allocation. Stack reductions require worst-case margin.

Approximate payload floors for planning (not allocator measurements):

| Allocation | Payload |
|---|---:|
| 64x32 logical RGB888 image | 6,144 B per image |
| 64x32 depth array of float32 | 8,192 B |
| 495 float32 positions | 5,940 B per base/animated/published copy |
| 523 uint16 triangle triples | 3,138 B |
| Current full compact morph entries | 35,200 B, before metadata |
| Two unpadded 128x32/1:16, 8-plane, uint16 scan buffers | 65,536 B |

Real panel allocation is driver/BCM dependent and can exceed the final row
substantially through dwell/blanking padding, descriptors and alignment. Count
the chosen backend's allocation formula and measured largest block before
startup. Record color depth/refresh alongside every memory comparison.

Exit: measured 60/90/120 FPS results under the shared workload, accepted
latency/quality, internal/PSRAM headroom and overnight stability. Do not promote
S31 CPU/PSRAM specification gains directly into a speedup estimate.

## Concrete application change locations

| Location | Planned change |
|---|---|
| `platformio.ini`, `boards/` or isolated IDF probe | S31-only SDK/build definition, output dependencies and board pin map |
| `src/main.cpp` | Target/backend selection, init/presentation metrics, release manifest routing |
| `src/Controllers/Controller.h` | Small local/output/backend boundary shared with Path B |
| New S31 controller/output adapter | RGB input, qualified PARLIO driver, brightness/completion/reinit |
| `src/Math/Vector2D.h`, `Vector3D.h`; microphone/DSP includes | Portable dispatch and SDK/FPU compatibility |
| `src/Menu/ESPMenu.h` | BLE migration, backend-aware preview and platform-independent HUD access |
| `src/Media/MediaService.h` | Receive-buffer ownership fix and common pixel-frame output |
| Network updater/build manifests | Explicit S31-local identity and existing authentication preservation |
| Host/device regression tooling | PARLIO format/ownership tests, shared workload and presentation evidence |

No Path A source changes are made by this document. Work is ready to start at
A0/A1 when the SDK and exact board are available; GPU firmware is not required.
