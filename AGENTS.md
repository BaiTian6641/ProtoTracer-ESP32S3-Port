# AGENTS.md — ProtoTracer-ESP32S3-Port

Guidance for AI coding agents working in this repository. Read this before making changes.

## Project Overview

This is an **ESP32-S3/ESP32-P4 port of ProtoTracer** (originally by Coelacant1): a live 3D
rendering and animation engine for microcontrollers. It renders textured/morphing 3D face
models (loaded from JSON, originally converted from .OBJ/.FBX) onto chained **HUB75 LED
matrix panels** to animate "Protogen"-style character faces.

Key runtime features:

- JSON-driven face animation: morph targets, expressions, blink tracking, viseme-based lip
  sync (FFT voice detection from a microphone), "boop" interaction via a gesture sensor.
- **BLE UART remote control** (JSON command protocol) used by the bundled `web-app/`
  (browser, Web Bluetooth) and `android-app/` (native Kotlin) remotes. The older ESP-NOW
  remote is obsolete.
- Wi-Fi onboarding (NetWizard captive portal), remote asset sync (face/animation JSON with
  MD5 change detection, GitHub/Gitee failover), OTA firmware updates (ElegantOTA), and
  firmware self-update from a remote manifest.
- Local status display on M5Stack unit displays (M5UnitGLASS2/GLASS/LCD/OLED, auto-detected
  via I2C probe at runtime) plus a NeoPixel status LED.

License: **AGPL-3.0** (see `LICENSE`). Modified versions used in products must be published.

## Repository Layout

```
src/                     Firmware (Arduino framework, C++; header-heavy — most code lives in .h files)
  main.cpp               Entry point: setup()/loop(), WiFi→BLE startup phase machine, web server, OTA
  Animation/             Animation base class, JsonDrivenProtogenAnimation (main face), presets, example_animation.json
  Controllers/           Display controllers; TasESP32S3KitV1.h (HUB75 via I2S DMA) is the active one
  Render/                Scene/Camera/Object3D/PixelGroup/QuadTree rendering primitives
  Morph/                 Morph targets; JsonNukudeFace.h, universal_face.json (85 morph targets)
  Materials/  Math/  Screenspace/  Shapes/  Signals/  Filter/  Physics/  Controls/   Engine building blocks
  Menu/                  ESPMenu.h — BLE server, gesture input, on-device status UI
  Network/               UserConfigManager, RemoteFileSync, FaceModelUpdater, FirmwareUpdater, AnimationDownloader
  Sensors/               Microphone (FFT), PAJ7620/APDS9960 gesture, BNO055, buttons
  Auth/                  AuthToken.h / TassAuthToken.h (token auth; see Security)
  Flash/                 Compile-time assets: icons, images, pixel-group layouts (P3HUB75.h)
boards/                  Custom PlatformIO board defs (esp32_s3_n8r8, esp32_s3_n8r16, esp32p4)
data/                    LittleFS image contents (uploaded with `buildfs`/`uploadfs`)
lib/                     Vendored libs: ElegantOTAPro-3-1-4 (gitignored, licensed), RevEng_PAJ7620
local_packages/          Local tool-mklittlefs package used by release/profile envs
web-app/                 Browser BLE remote (vanilla JS/HTML/CSS, no build step); deployed to GitHub Pages
android-app/             Native Android BLE remote (Kotlin, Gradle)
docs/                    Design/review documents (rendering, memory, BLE, optimization plan)
```

There are stray `*.bak` files in the tree (e.g. `src/Controllers/TasESP32P4KitV0.h.bak`,
`src/Render/Scene.h.bak`). They are not compiled — do not edit them; treat them as history.

## Build, Flash, and Test Commands

Build system is **PlatformIO** (`platformio.ini` is the central config). All firmware work
happens through environments:

| Environment | Purpose |
|---|---|
| `esp32s3` | Debug build (`build_type=debug`, `-O3 -g`) for ESP32-S3 N8R8 (8 MB flash/PSRAM) |
| `esp32s3-RELEASE` | Release build; enables `USE_TOKEN_AUTH`, `ENABLE_M5_PIXEL_PREVIEW` |
| `esp32s3-PROFILE` | Release-style build with `PRINTINFO` telemetry, `-O2 -Wall` |
| `esp32p4` | Alternate ESP32-P4 target (16 MB flash) |

All environments use the **pioarduino** platform fork
(`platform = https://github.com/pioarduino/platform-espressif32.git`), i.e.
Arduino-ESP32 **3.x / IDF 5.x**. The official `espressif32` platform (Arduino
2.0.17 / IDF 4.4, gcc 8.4, `-std=gnu++11` default) does **not** compile this
project — NetWizard (every release) and a few call sites in `src/` use
Arduino-ESP32 3.x-only APIs (`WiFi.AP.*`, const `BLECharacteristic::setValue`,
`EXT_RAM_BSS_ATTR`, 3-arg `WiFi.disconnect`, `WIFI_AUTH_OWE`). Do not switch an
env back to the official platform without reverting those call sites. The S3
envs also pin the C++ standard (`build_unflags = -std=gnu++11`, `-std=gnu++17`);
**C++17 is required** by ProtoGC (namespace-scope `inline` variables,
multi-statement `constexpr`) and by project code (`std::make_unique`, aggregate
init of structs with default member initializers). NetWizard is pinned
(`ayushsharma82/NetWizard@^1.2.0`) to protect against upstream drift.

Common commands:

```bash
pio run -e esp32s3                  # build firmware
pio run -e esp32s3 -t upload        # flash via esptool
pio run -e esp32s3 -t buildfs       # build LittleFS image from data/
pio run -e esp32s3 -t uploadfs      # flash filesystem image
pio device monitor -b 115200        # serial monitor (esp32_exception_decoder enabled)
```

Build-time helper scripts wired via `extra_scripts` in `platformio.ini`:

- `select_elegantota.py` (pre) — uses local `lib/ElegantOTAPro-3-1-4` if present, otherwise
  falls back to the open-source ElegantOTA git dependency.
- `merge-bin.py` (post) — produces `${PROGNAME}_merged.bin` (bootloader + partitions + app)
  in the build dir via esptool `merge_bin`.
- `set_mklittlefs.py` — forces the repo-root `mklittlefs` binary for filesystem images.

Utility scripts:

- `generate_md5s.sh` — generates `.md5` sidecar files for hosted assets; the firmware's
  `RemoteFileSync` compares these against remote copies to decide what to re-download.
- `validate.ps1` (PowerShell, Windows) — sanity checks: (1) every `vrc_v_*` viseme referenced
  by `src/Animation/example_animation.json` `auto_link` exists in `src/Morph/universal_face.json`;
  (2) in `JsonDrivenProtogenAnimation::Initialize()`, `LoadAnimationConfig(config)` runs before
  `AutoLinkMorphs()`/`LinkParameters()`. Run it after touching animation JSON or that init path.

Companion apps:

```bash
cd web-app && python -m http.server 8080     # local web remote (Web Bluetooth needs Chrome/Edge)
cd android-app && gradle assembleDebug       # Android remote (Gradle 8.5+; see android-app/README.md)
```

**Testing:** there is no unit-test suite — `test/` and `include/` contain only the stock
PlatformIO placeholder READMEs. Verification is: compile the relevant env, run
`validate.ps1` when animation/morph data changes, and test on hardware (serial log at
115200 baud, `esp-builtin` JTAG debugging configured). Several subsystems log through
`Serial.printf`; define `PRINTINFO`/`VERBOSE_STARTUP` for more.

## Runtime Architecture Notes

- `main.cpp` boots Wi-Fi + async web server, starts a **background FreeRTOS download task**
  (core-pinned) to sync remote assets while the first frames render, then tears Wi-Fi down
  and brings up **BLE** (they coexist poorly — this ordering is deliberate, do not casually
  reorder it). A separate animation task (`ANIM_TASK_CORE 0`, stack 8192 B) runs the render
  pipeline when `ANIM_RENDER_PIPELINE` is on.
- The controller (`TasESP32S3KitV1`) drives two chained 64×32 HUB75 panels (128×32 virtual
  display via `ESP32-VirtualMatrixPanel-I2S-DMA`) and mirrors a 64×32 HUD preview into
  `gHudBuffer` for the M5 display.
- **Memory is the scarce resource.** PSRAM flags (`CONFIG_SPIRAM_*`, `BOARD_HAS_PSRAM`)
  matter; large/static buffers use `EXT_RAM_BSS_ATTR` and `heap_caps_malloc`, and face-JSON
  parsing goes to PSRAM under `USE_PSRAM_FOR_FACE_JSON`. Internal DRAM is limited (~512 KB)
  — avoid per-frame heap allocation in render hot paths (see `docs/review-summary.md`;
  known issue: heap fragmentation during long runs).
- Feature selection is done almost entirely through **build flags / `#define`s**:
  `TASESP32S3`/`TASESP32P4` (controller), `NEW_GESTURE` (PAJ7620 vs APDS9960),
  `USE_TOKEN_AUTH`, `ENABLE_M5_PIXEL_PREVIEW`, `LANG_CN` (Chinese UI strings via the
  `TXT(en, cn)` macro), `OTA_BTN` (GPIO for OTA trigger), firmware version in
  `PROTOTRACER_FW_VERSION` (currently `1.2.10`, bump it in `main.cpp` when releasing).
- BLE protocol: service UUID `73cf57c7-6797-46e8-8202-dc5e7f956b57`, JSON ops
  (`config.get`, `control.set`, `ping` → manifest / `control.state` / `pong`), payloads
  chunked at 160 bytes. Both remotes and `ESPMenu.h` must stay in sync — update all three
  when changing the protocol.

## Code Style Guidelines

- Language: C++ (Arduino/ESP-IDF mix), **English** for code, comments, and docs. (A Chinese
  doc translation `ESP32S3-Feature-Overview.zh.md` exists; UI strings support `LANG_CN`.)
- 4-space indentation; Allman-ish braces in `main.cpp`, mixed but consistent-per-file
  elsewhere — match the file you're editing.
- Implementations live in headers (single-header classes are the norm here); only
  `src/Network/` uses `.cpp`/`.h` pairs. New engine-level classes should follow the
  header-only pattern; new network services should follow the `.cpp`/`.h` split.
- Classes are often templated on capacity (e.g. `Animation<numObjects>`,
  `MaterialAnimator<10>`) — fixed-size arrays, no dynamic growth. Respect the bounds.
- Guard optional hardware/features with `#if __has_include(...)` / build-flag `#ifdef`s,
  as in `main.cpp` (ElegantOTA, M5 unit displays).
- Minimal diffs: this codebase has known fragile spots (see `docs/`), so don't reformat or
  "clean up" unrelated code when fixing something.

## Security Considerations

- `src/Auth/TassAuthToken.h` is **gitignored** — it holds a private auth token used when
  `USE_TOKEN_AUTH` is defined (RELEASE/PROFILE envs). Never commit it or print its
  contents. `src/Auth/AuthToken.h` is the non-token fallback for debug builds.
- `lib/ElegantOTAPro-3-1-4/` is a paid, licensed library and is gitignored; the build
  falls back to open-source ElegantOTA when absent. Don't commit or redistribute it.
- Wi-Fi credentials and user settings persist in LittleFS (`user_config.json`); remote
  assets are fetched over the network from GitHub/Gitee with MD5 verification only —
  keep that threat model in mind when touching `RemoteFileSync`/`FirmwareUpdater`.
- OTA endpoints (ElegantOTA) and the async web server are exposed on the local network;
  token auth (`USE_TOKEN_AUTH`) is the mechanism that gates sensitive routes in release
  builds — don't bypass it.

## CI / Deployment

- `.github/workflows/pages.yml` deploys `web-app/` to GitHub Pages on pushes to
  `main`/`master` that touch `web-app/**`. No firmware CI exists.
- Firmware releases are manual: build `esp32s3-RELEASE`, bump `PROTOTRACER_FW_VERSION`,
  and publish the merged binary + filesystem image; `FirmwareUpdater` on devices compares
  versions from the remote manifest (`esp32s3.json` / `esp32p4.json`).

## Known Issues (from README/docs)

- Memory issues during long continuous runs (heap fragmentation — see `docs/review-summary.md`).
- Spectrum analyzer is broken pending a DSP rewrite.
- APDS9960 is discontinued; PAJ7620 (`NEW_GESTURE`) is the supported gesture sensor.
