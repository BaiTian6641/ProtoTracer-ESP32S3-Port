# ESP32-S31 PARLIO bring-up log

2026-10-08, branch `codex/esp32s31-parlio`.

## Full application and first hardware qualification

The S31 application now uses the real `src/main.cpp`, JSON face/morph engine,
materials, rasterizer, BLE remote/media services and network/OTA code. The new
`src/Controllers/TasESP32S31KitV0.h` keeps the S3 camera grid and mirrored
two-panel mapping, driving the existing `esp-hub75` PARLIO backend. Build and
flash instructions are in the experiment README and `tools/build_s31.ps1`.

Hardware confirmation: COM6 is built-in USB Serial/JTAG (USB-DBG), chip rev
v0.0, flash 16 MB and PSRAM 16 MB. esptool reports a 300 MHz maximum for this
revision; the initial SDK configuration uses 240 MHz CPU and 200 MHz octal
PSRAM. The original 16 MB flash was backed up to ignored
`build/s31-before-full-port.bin` before bringing up the board.

Full mode uses a custom partition table at 0x10000, two 3.5 MB OTA slots,
and 8.875 MB LittleFS at 0x720000. The first successfully linked application
was 2,859,280 bytes. Wired qualification boots with public local face and
animation assets and `PROTOTRACER_S31_OFFLINE_BOOT=ON`; normal onboarding is
selected by omitting `-Offline`. No S31 release manifest has been published.

Required ports and repairs:

- Arduino 4 BLE value handles, callbacks and MTU events preserve the existing
  UART JSON/media protocol. The Arduino 3 path is retained for S3.
- M5GFX generated copies use supported GPIO configuration and an Arduino Wire
  I2C adapter, including address probes for missing displays. Headless menu
  initialization and refresh both guard null display pointers. The first
  hardware panic was decoded to an unguarded `Menu::Update()` display call;
  the corrected firmware runs past it.
- GPIO35/36/37 are CLK/LAT/OE; 38/39/40 are RGB1; 42/43 are A/B. Unprobed
  C/D use 44/45 and RGB2 uses 46/47/48. Auxiliary I2C uses 2/3, external ADC
  microphone input uses 49, status LED is 60, and BOOT/OTA is 61. USB-DBG
  33/34 and onboard audio 50-57 are kept out of the HUB75 map.
- PARLIO swaps wait for the matching SDK `on_buffer_switched` event. Timeouts
  retain source ownership; brightness changes are deferred to the owned back
  buffer. Failed stop/delete quarantines ISR state and DMA storage.
- The first real trace exposed LAT asserted 150 ns before the final shift
  clock. LAT now pulses in the first clock-gated padding word, and the start
  and end of padding remain blanked. The corrected 40 MHz capture measured
  LAT 125 ns after the last clock at a 4 MHz output clock.
- ProtoGC requires C++ exceptions in the full SDK configuration. QRCode's
  legacy C source uses C17 because its manual `bool` conflicts with C23.
  Existing engine copy/clamp diagnostics remain warnings only in the S31
  main component; other compiler errors remain fatal. A legacy packet copy
  now validates four bytes instead of using `sizeof(pointer)`, and the
  network-probe printf arguments use portable unsigned casts.

Initial 4 MHz hardware results:

| Check | Result |
|---|---|
| Face/animation loading | PASS: 495 vertices, 523 triangles, 37 retained morphs, 18,944 B morph store |
| Dual-core renderer | About 37 FPS; 6.5 ms animation, 1.6 ms rasterization in one sampled frame |
| HUB75 ownership | 10,325 completed presents, zero failures |
| Memory after BLE/media tests | 200,515 B internal free, 135,168 B largest block; 16,109,352 B PSRAM free |
| Heap integrity | `dbg check`: PASS |
| Microphone sampler | Running on GPIO49; sampled telemetry showed zero dropped frames |
| SLogic waveform | 4 MHz, 128 clocks per burst, one latch per burst, OE blanked during clocks/LAT, A/B progression modulo four |
| Partial row address | Eight bit-plane latches per observed A/B row |
| Scan refresh | Approximately 72.79 Hz at the initial 4 MHz clock |
| BLE-to-USB-to-pins | Red/green/blue/black passed: exact 6144-byte pixel snapshots and correct RGB1 channel on real captures |

### Final faster configuration

The default controller now requests 10 MHz and a 200 Hz minimum scan rate.
Increasing clock alone kept the upstream library near its default 60 Hz scan
target by adding BCM padding: that intermediate setting measured 67.70 Hz scan
and 33.87 FPS. The refresh estimator now counts the actual serialized shift
and padding words before choosing its transition bit.

The final configuration measures **244.88 Hz scan and 48.5-49.6 FPS** on the
supplied face at 240 MHz CPU. LAT follows the last clock by 50 ns; the trace
contains 128 clocks and one latch per burst, zero unblanked clock/latch events,
and eight bit-plane latches per A/B address. Both DMA buffers together occupy
163,328 B of PSRAM, down from 219,776 B in the initial 4 MHz test. The library's
hybrid BCM/PWM scheme trades lower-bit precision for refresh; visible panel
gradients still require qualification.

The final red/green/blue/black BLE-to-USB-to-pins tests all pass at 40 MHz
analyzer sampling. Subsequent diagnostics report **15,703 completed presents,
zero failures**, heap integrity good, 203,143 B internal free (135,168 B largest
block), 16,165,800 B PSRAM free, and zero microphone sampler drops. Loop,
animation and microphone stack high-water values are 5368/7436/5504 B.
Application size is 2,859,456 B. Normal Wi-Fi boot also passes an application
API compile check; runtime network/OTA qualification remains pending.

SLogic Combo8 is VID359F/PID0300 with its existing WinUSB interface. Workspace
Python uses `pyusb`/`libusb-package` and a GPL-3.0-or-later adapter based on
[Sipeed's published protocol](https://github.com/sipeed/libsigrok/tree/slogic-dev/src/hardware/sipeed-slogic-analyzer/slogic).
Large queued USB reads avoid FIFO overflow caused by gaps between small
synchronous reads. Both 20 and 40 MHz acquisition have been exercised.
Raw captures, logs and framebuffer dumps stay under ignored `build/`; analysis
JSON records capture SHA-256 values and times for reproduction.

Regression gates: the final shared changes compiled on S3; host direct and
legacy rendering remained byte-identical (6144 B, FNV-1a `BE8B37A9`); portable
DSP vectors and frame-ownership fault cases passed; the analyzer's known-trace
tests passed; `validate.ps1` passed both checks. The prepared components retain
upstream licenses, and regeneration preserves timestamps when content is
unchanged.

Remaining qualification is the complete HUB75 connector and visible panels,
auxiliary displays/gesture hardware, ES8311 onboard audio, Wi-Fi onboarding and
OTA runtime, and longer operation with larger faces. ADC49 sampling does not
qualify the onboard codec. The RP2350/ProtoGL route remains the separate roadmap
awaiting GPU firmware. Nested PAJ7620 changes and private/paid source were
preserved outside this work.

Primary hardware/API references:
[Function-Core pin map](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp32-s31-function-coreboard-1/user_guide.html),
[IDF 6.1 S31 PARLIO](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s31/api-reference/peripherals/parlio/parlio_tx.html).

## First implemented checkpoint (b82c8a6, historical)

Created an [isolated IDF/Arduino probe](../experiments/esp32s31-parlio/README.md)
for the user's ESP32-S31 Function-Core board, reported module
ESP32-S31-WROOM-3 E1H16R16V. Defaults target 16 MB flash and 16 MB PSRAM,
320 MHz CPU and initially 200 MHz octal PSRAM. Actual module marking, memory
size, GPIO wiring and panel behavior still need hardware verification.

The existing application selects `framework = arduino` in PlatformIO and uses
Arduino 3 on IDF 5.5 plus direct IDF APIs. Its engine does not need a native-IDF
rewrite. The successful probe retains Arduino APIs as an IDF component and
establishes an IDF 6.1/Arduino 4 build path independently of the S3 environment.
It is an SDK/peripheral port; it is not yet a full S31 application port.

## Research and dependency decisions

- [Arduino 4.0.0-RC1](https://github.com/espressif/arduino-esp32/releases/tag/4.0.0-RC1)
  exists and explicitly includes S31 support on IDF 6.1. The earlier roadmap's
  draft PR observation was incomplete; the release tag is the concrete route
  used here. Pin: `76f683d935b390f2805301ec10a5562bbbb37811`.
- IDF v6.1 pin: `fff9895c82d744c7237be8847347bdd1b07c6643`. Its S31 SoC
  definitions confirm 16-bit PARLIO, clock gating, looping and default
  PLL_F160M. The existing installed GCC `esp-15.2.0_20251204` matches the
  SDK's recommended RISC-V compiler, so another compiler installation was
  unnecessary. A separate IDF 6.1 Python environment lives in ignored build
  storage; the existing SDK/package installations are not replaced.
- pioarduino develop, inspected at `1643aa8e512e65962d7da0b04c729835ebe40f36`,
  still lacks S31 in its MCU toolchain selection and board inventory. Its
  default IDF package is 5.5.5 even though the IDF builder knows the S31
  bootloader offset. Therefore this first experiment uses direct IDF/CMake
  instead of adding an unverified PlatformIO environment to `platformio.ini`.
- Existing MIT `esp-hub75` pin: `d0bb571fdae6ec6215615de4b73a6e8ce8868a85`.
  The preparation tool copies its component and adds S31 selection in build,
  factory, platform and external-buffer guards. It replaces the deprecated
  sample-edge field with IDF 6.1's shift-edge field. No new packing/BCM driver
  was written. The S31 clock source matches the backend's existing 160 MHz
  resolver for this pinned SDK; hardware rates still require qualification.
- [Function-Core documentation](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s31/esp32-s31-function-coreboard-1/user_guide.html)
  confirms UART0 GPIO58/59, RGB LED GPIO60 and BOOT GPIO61. The probe reserves
  these from HUB75 assignments. The board also has Ethernet/USB/audio wiring;
  SDK GPIO capability checks do not identify every board-level conflict.
- Its onboard ES8311 codec uses I2S/I2C, unlike the current MAX9814 ADC path.
  Audio-source selection is a later adapter decision; no DSP/viseme rewrite
  is included in this step.

## Source changes and build repairs

1. Optional vector DSP paths now call public `dsps_dotprod_f32` using its
   actual `destination, length` argument order. The previous Xtensa-specific
   call also had those arguments reversed. Default scalar paths are retained.
2. `Mathematics::IsFinite` / `IsInfinite` use `std::isfinite` / `std::isinf`
   with `<cmath>`, fixing name lookup under the newer toolchain.
3. The generated Arduino and HUB75 components retain licenses and are ignored
   by Git. Preparation validates pinned revisions, clean tracked source and
   exact source anchors before patching copies.
4. IDF 6.1/Arduino RC1 have valid partial aggregate initializers that trigger
   default warning-as-error policy. The probe downgrades only that diagnostic
   for its three C++ components; it does not disable all compiler errors.
   Arduino's secure-client library also requires the TLS PSK settings enabled
   in the probe's defaults.
5. Ninja response files avoid Windows' command-line limit from Arduino's
   public include list. No Windows global policy changes were made.

## Verification

| Check | Result |
|---|---|
| S31 IDF 6.1 / Arduino RC1 probe build | PASS, bootloader and app binaries generated |
| S31 app binary | 293,232 B; default 1 MB factory partition has 72% free |
| S31 configured target | `esp32s31`, 320 MHz, 16 MB flash, 200 MHz PSRAM, 8-bit HUB75 external buffers |
| Complete panel setup code | Type-checked even with output disabled; no GPIOs are automatically assigned |
| Existing S3 `pio run -e esp32s3` after final math fixes | PASS, including merged image |
| DSP-enabled vector host gate | PASS, public API signature, signed/orthogonal arithmetic and finite/inf/NaN classification |
| Host direct/legacy render gate | PASS, 6,144 bytes identical, FNV-1a `BE8B37A9` |
| Component preparation repeated | PASS, generated HUB75 source hashes unchanged |
| Board flash / boot / PSRAM reads / panel electrical output | Not performed |
| S31 full ProtoTracer application, BLE and audio runtime | Not yet integrated/tested |

The bootloader occupies `0x5a80` B of its current reserved region, leaving
`0x580` B. Choose a suitable production partition/bootloader layout when
security/update settings and the full application are integrated; this small
probe uses the standard factory layout, not the S3 merged-image script.

## Next implementation boundary

The upstream PARLIO flip swaps buffer indices and queues transmission without
exposing old-buffer release to the caller; brightness changes can also touch
both scan buffers. The current probe makes one static pattern submission and
does not reuse those buffers for animation. Before connecting the renderer,
add/qualify a frame lease/completion boundary and deferred brightness mutation,
including timeout/failure ownership and cache visibility.

Then add the S31 local controller/output adapter, confirm the board GPIO map,
and retain existing morph/material/render behavior for the first face test.
Port Arduino 4 BLE callbacks and complete Wi-Fi/asset/OTA/OLED/gesture/audio
parity afterward. Keep external MAX9814 support or select an ES8311 adapter
explicitly. The GPU firmware is not required for any of these local steps.

Nested PAJ7620 edits remain outside this checkpoint and were preserved. No
auth token or licensed OTA library source was added to this branch.
