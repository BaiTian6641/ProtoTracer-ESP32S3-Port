# ESP32-S31 PARLIO bring-up log

2026-10-08, branch `codex/esp32s31-parlio`.

## First implemented step

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
