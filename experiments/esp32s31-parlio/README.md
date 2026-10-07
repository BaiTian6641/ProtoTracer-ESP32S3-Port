# ESP32-S31 Function-Core firmware and PARLIO probe

This isolated project provides the S31 build path on `codex/esp32s31-parlio`.
It uses **ESP-IDF 6.1 with Arduino 4.0.0-RC1 as a component**, so existing
Arduino application APIs can be retained. The main PlatformIO environments
continue to use their S3 SDK; they are not migrated by this experiment.

Target supplied by the user: ESP32-S31 Function-Core Board,
ESP32-S31-WROOM-3 E1H16R16V. Defaults assume 16 MB flash / 16 MB PSRAM,
240 MHz CPU, QIO/80 MHz flash and initial 200 MHz octal PSRAM. The connected
revision-v0.0 chip reports a 300 MHz maximum through esptool, so this initial
configuration uses the SDK's lower 240 MHz choice. Flash and PSRAM have both
been verified as 16 MB over the board's USB-DBG interface.

The probe builds the existing Arduino APIs, real ProtoTracer vector headers
with DSP enabled, and a small S31 port of the existing MIT `esp-hub75` PARLIO
component. Both modes use **Serial over built-in USB Serial/JTAG (USB-DBG)**,
currently COM6, with nonblocking output. The full-firmware mode compiles the
real `src/main.cpp`, network services, face/morph/material renderer and BLE
remote protocol, using `TasESP32S31KitV0` for two chained 64x32 panels.

## Pinned inputs

| Input | Revision |
|---|---|
| ESP-IDF v6.1 | `fff9895c82d744c7237be8847347bdd1b07c6643` |
| Arduino 4.0.0-RC1 | `76f683d935b390f2805301ec10a5562bbbb37811` |
| `esphome-libs/esp-hub75` | `d0bb571fdae6ec6215615de4b73a6e8ce8868a85` |
| ESP-DSP | `1.8.0` through the component manager |

Clone the reference inputs into the gitignored `reference/` directory:

```powershell
git clone --depth 1 --branch v6.1 --recurse-submodules --shallow-submodules https://github.com/espressif/esp-idf.git reference/esp-idf-s31
git clone --depth 1 --branch 4.0.0-RC1 https://github.com/espressif/arduino-esp32.git reference/arduino-s31-review
git clone https://github.com/esphome-libs/esp-hub75.git reference/esp-hub75-review
git -C reference/esp-hub75-review checkout d0bb571fdae6ec6215615de4b73a6e8ce8868a85
python tools/prepare_s31_probe.py
```

The preparation tool verifies revisions and refuses tracked local edits. It
copies Arduino's component files and the HUB75 component into this project's
gitignored `components/` directory, then applies checked source-anchor edits
only to those copies. Re-running it reconstructs the same patched sources.
Existing reference checkouts and global PlatformIO packages are untouched.
Upstream MIT/LGPL license files are retained in the generated component copies.

The port adds S31 build/factory/platform and external-buffer selection, and
uses the IDF 6.1 `shift_edge` field instead of its deprecated alias. IDF 6.1's
S31 definitions confirm the driver's default PLL_F160M clock source, 16-bit
width, clock gating and loop-transmission capabilities. No new HUB75 packing
or BCM driver is written here.

## Build

For the full firmware, first resolve the existing S3 PlatformIO libraries,
then prepare the generated dependency component:

```powershell
pio pkg install -e esp32s3
python tools/prepare_s31_firmware.py
tools/build_s31.ps1 -Offline
tools/build_s31.ps1 -Offline -Action flash -Port COM6
```

`build_s31.ps1` describes this Windows machine's installed compiler/CMake/Ninja
versions and workspace IDF Python environment; override `-ToolRoot` if needed.
It limits both the outer build and the nested bootloader build. Alternatively,
activate IDF 6.1 normally and use:

```powershell
idf.py -C experiments/esp32s31-parlio -B build/s31-full -DPROTOTRACER_FULL_FIRMWARE=ON -DPROTOTRACER_S31_OFFLINE_BOOT=ON build
```

Offline mode skips onboarding for wired tests with seeded public face and
animation files. Omit `-Offline` for normal Wi-Fi onboarding and the existing
Wi-Fi-to-BLE startup sequence. The prepared LittleFS image contains public
test assets and minimal test settings; `flash` writes that image as well as the
app. Preserve existing device data before flashing it. Full mode reserves
two 3.5 MB OTA slots and an 8.875 MB filesystem, with the partition table at
0x10000 and first application at 0x20000. Use this project's flash arguments.

The full dependency adapter reads resolved libraries from `.pio/libdeps/esp32s3`
and keeps paid OTA source in its existing local directory. M5GFX is patched in
generated copies for S31 GPIO types and an Arduino Wire I2C adapter; M5Unified
board autodetection is not needed on this board. Auxiliary I2C transactions
are bounded to 8192 bytes with persistent Wire buffers and 50 ms timeouts.
Hardware qualification of auxiliary displays remains separate from headless
rendering. S31 external MAX9814 input uses ADC1 GPIO49; the onboard ES8311
codec needs a separate audio adapter.

The smaller probe remains available with `PROTOTRACER_FULL_FIRMWARE=OFF`.
Install/activate the pinned IDF 6.1 environment using its standard tools, then
run from the repository root:

```powershell
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'
idf.py -C experiments/esp32s31-parlio -B build/s31-probe build
idf.py -C experiments/esp32s31-parlio -B build/s31-probe menuconfig
```

The installed IDF 6.0.2 RISC-V compiler on this machine matches IDF 6.1's
recommended `esp-15.2.0_20251204`; the experiment uses a separate IDF 6.1 Python
environment under `build/s31-tools`. A normal IDF activation sets
`ESP_IDF_VERSION` as well as SDK, compiler and Python paths. Do not bypass
dependency/version checks or repoint the working S3 PlatformIO installation.

The probe enables the TLS PSK configuration required by Arduino's secure
client library. It also keeps IDF 6.1/RC1 partial-aggregate-initialization
diagnostics as warnings for its Arduino, HUB75 and main C++ components only;
other warning/error checks remain enabled.
Ninja response files are enabled because the Arduino component's public
include set can exceed Windows command-line limits.

## Panel output and current limits

HUB75 output is **disabled by default**, and its 13 GPIOs default to `-1`.
Assign actual board wiring in `ProtoTracer S31 probe` under menuconfig before
enabling output. The Function-Core board also connects GPIOs to Ethernet, USB,
audio and its RGB LED: match the schematic rather than copying P4/S3 pins.
The probe rejects incomplete, duplicate and non-output-capable assignments,
plus GPIO58-61 used for USB-UART, RGB LED and BOOT;
that check does not identify every board-level peripheral conflict.

When enabled, it requests a 10 MHz clock, two chained 64x32 panels, eight-bit
color and PSRAM double buffering, then submits a static red/green panel pattern.
No continuous animation updates are made in probe mode. The generated port
adds an IDF `on_buffer_switched` acknowledgment, retains DMA ownership across
timeouts, and applies brightness only to the owned back buffer. The full
controller waits before drawing and presenting each animation frame.

The onboard microphone uses an ES8311 I2S/I2C codec. It is a different input
from this application's current MAX9814 ADC path; keep external microphone
support or add a separate codec input adapter when choosing the audio wiring.

The full controller's bring-up map is:

| Signal | GPIO | SLogic assumed channel |
|---|---:|---|
| CLK / LAT / OE | 35 / 36 / 37 | D0 / D1 / D2 |
| R1 / G1 / B1 | 38 / 39 / 40 | D3 / D4 / D5 |
| A / B | 42 / 43 | D6 / D7 |
| C / D | 44 / 45 | Unprobed |
| R2 / G2 / B2 | 46 / 47 / 48 | Unprobed |
| E | -1 | Unused for 64x32, 1/16 scan |

The full controller requests 10 MHz with a 200 Hz minimum scan target.
`S31_HUB75_CLOCK_HZ` and `S31_HUB75_REFRESH_HZ` can select other settings.
The library's hybrid BCM/PWM timing trades lower-bit precision for refresh;
visible gradients still need panel qualification. The initial waveform tests
used 4 MHz for detailed edge inspection before testing the faster setting.
GPIO33/34 remain USB-DBG, 58/59 remain USB-UART, 60 is the status LED and 61
is BOOT/OTA. Auxiliary I2C uses GPIO2/3. Confirm the complete panel wiring
before using the unprobed signals.

For the detected SLogic Combo8 (VID359F/PID0300), workspace Python can capture
directly through its existing WinUSB interface:

```powershell
python -m pip install pyusb libusb-package numpy
python tools/slogic_combo8_capture.py --output build/s31-hub75.bin --rate-mhz 20 --duration 0.05
python tools/analyze_s31_hub75.py build/s31-hub75.bin
python tools/serial_debug.py --port COM6 stats
python -m pip install bleak
python tools/test_s31_ble_hub75.py --port COM6 --output build/s31-ble-hub75
```

The GPL-3.0-or-later capture adapter follows Sipeed's published Combo8 protocol
and performs no device firmware writes. Capture duration is bounded to 250 ms.
The analyzer reports row clock counts, latch ordering, OE blanking, partial
address progression and RGB activity. Its channel map is an assumption until
confirmed by the trace. Capture JSON includes its SHA-256 and acquisition time.

See [the bring-up log](../../docs/esp32s31-parlio-bringup.md) for measured results
and remaining hardware checks. Preparation and build do not flash the board;
`-Action flash` does.

## Primary references

- [Function-CoreBoard user guide](https://documentation.espressif.com/esp-dev-kits/en/latest/esp32s31/esp32-s31-function-coreboard-1/index.html).
- [Arduino 4.0.0-RC1 release](https://github.com/espressif/arduino-esp32/releases/tag/4.0.0-RC1).
- [S31 PARLIO TX API](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s31/api-reference/peripherals/parlio/parlio_tx.html).
- [Existing PARLIO HUB75 implementation](https://github.com/esphome-libs/esp-hub75/tree/d0bb571fdae6ec6215615de4b73a6e8ce8868a85/components/hub75/src/platforms/parlio).
