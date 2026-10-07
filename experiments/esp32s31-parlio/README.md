# ESP32-S31 Function-Core PARLIO probe

This isolated project is the first bring-up step on `codex/esp32s31-parlio`.
It uses **ESP-IDF 6.1 with Arduino 4.0.0-RC1 as a component**, so existing
Arduino application APIs can be retained. The main PlatformIO environments
continue to use their S3 SDK; they are not migrated by this experiment.

Target supplied by the user: ESP32-S31 Function-Core Board,
ESP32-S31-WROOM-3 E1H16R16V. Defaults assume 16 MB flash / 16 MB PSRAM,
320 MHz CPU, QIO/80 MHz flash and initial 200 MHz octal PSRAM. Boot telemetry
reports actual flash/PSRAM size; verify the module marking and readings before
using these defaults for a release.

The probe builds the existing Arduino APIs, real ProtoTracer vector headers
with DSP enabled, and a small S31 port of the existing MIT `esp-hub75` PARLIO
component. It reports memory/SDK information over **Serial0 at 115200**, using
the board's USB-to-UART port. It is not the complete ProtoTracer application.

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
No continuous animation updates are made. The selected upstream driver's
`flip_buffer()` does not expose release confirmation before reusing the old
scan buffer; animation integration must add/qualify that ownership boundary
first. A single static submission cannot establish tear-free animated output.

The onboard microphone uses an ES8311 I2S/I2C codec. It is a different input
from this application's current MAX9814 ADC path; keep external microphone
support or add a separate codec input adapter when choosing the audio wiring.

The full application still needs controller/output integration, board pin
mapping, Arduino 4 BLE compatibility and services/hardware qualification.
No firmware is automatically flashed by the preparation or build commands.

## Primary references

- [Function-CoreBoard user guide](https://documentation.espressif.com/esp-dev-kits/en/latest/esp32s31/esp32-s31-function-coreboard-1/index.html).
- [Arduino 4.0.0-RC1 release](https://github.com/espressif/arduino-esp32/releases/tag/4.0.0-RC1).
- [S31 PARLIO TX API](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s31/api-reference/peripherals/parlio/parlio_tx.html).
- [Existing PARLIO HUB75 implementation](https://github.com/esphome-libs/esp-hub75/tree/d0bb571fdae6ec6215615de4b73a6e8ce8868a85/components/hub75/src/platforms/parlio).
