# ESP32-S3 Port Feature Overview

This document summarizes the main functionality implemented in the `ProtoTracer-ESP32S3-Port` repository.
It is focused on the ESP32-S3 port and the features exposed by the current codebase.

## 1. Project Purpose

- This is an ESP32-S3 port of the Prototype `ProtoTracer` 3D rendering and animation engine.
- It is designed for microcontroller-based live rendering to pixel matrices and face displays.
- The port targets ESP32-S3 / ESP32-P4 hardware with PSRAM, LittleFS, BLE, Wi-Fi, and HUB75 output.

## 2. Build Targets and PlatformIO Environments

Configured in `platformio.ini`:

- `env:esp32s3` – debug build for `esp32_s3_n8r8`.
- `env:esp32s3-RELEASE` – release build for `esp32_s3_n8r8` with token auth and remote config features.
- `env:esp32p4` – alternate ESP32-P4 build target.

Build features enabled by flags:

- `TASESP32S3` and `CONFIG_IDF_TARGET_ESP32S3` for ESP32-S3-specific code.
- `BOARD_HAS_PSRAM`, `CONFIG_SPIRAM_*` flags to use external PSRAM for large JSON and render buffers.
- `ARDUINO_USB_CDC_ON_BOOT=1` for USB serial support.
- `USE_PSRAM_FOR_FACE_JSON` to allocate face model JSON parsing in PSRAM.
- `NETWIZARD_USE_ASYNC_WEBSERVER=1` and `ELEGANTOTA_USE_ASYNC_WEBSERVER=1` for async web services.

## 3. Core Runtime Entry Point

### `src/main.cpp`

- Initializes Wi-Fi, display, BLE, gesture sensor, file system, and rendering pipeline.
- Selects the controller implementation based on build flags:
  - `TasESP32S3KitV1` for ESP32-S3.
  - `TasESP32P4KitV0` for ESP32-P4.
- Creates main runtime objects:
  - `JsonDrivenProtogenAnimation animation`
  - `AsyncWebServer server`
  - `UserConfig userConfig`
  - `FaceUpdateConfig faceUpdateConfig`
- Sets up remote file sync and HTTP relay routes for asset download.
- Registers OTA portal routes using ElegantOTA when available.
- Handles OTA button state and runtime loop scheduling.

## 4. Display and Output Hardware

### Local display

- Uses `M5UnitGLASS2` for device status, progress, icons, and QR codes.
- Local display code appears in `main.cpp`, `src/Menu/ESPMenu.h`, and animation classes.

### LED matrix output

- `src/Controllers/TasESP32S3KitV1.h` implements HUB75 output via `ESP32-VirtualMatrixPanel-I2S-DMA`.
- Supports `64x32` panel modules chained as configured by `PANEL_RES_X`, `PANEL_RES_Y`, `NUM_ROWS`, `NUM_COLS`.
- `virtualDisp` renders to a matrix panel and also mirrors pixels to the M5 display for local preview.
- Controller base behavior is in `src/Controllers/Controller.h`.

### Pixel indicator

- Uses a single `Adafruit_NeoPixel` LED for status indicators.

## 5. Animation Engine

### Base animation framework

- `src/Animation/Animation.h` defines the generic `Animation<numObjects>` base class.
- It exposes:
  - `GetScene()`
  - `GetAnimationTime()`
  - `FadeIn()`, `FadeOut()`, `Update()` abstract hooks.
  - `UpdateTime(ratio)` wrapper for timed updates.

### JSON-driven animation

- `src/Animation/JsonDrivenProtogenAnimation.h` is the main animated face implementation.
- It loads animation metadata from `LittleFS` files:
  - `<device_id>_animation.json`
  - fallback `/example_animation.json`
- Supports JSON-driven:
  - face models and morph targets from `JsonNukudeFace`
  - material selection and animated materials
  - scene effects (blur, anti-aliasing, spectrum analyzer, rainbow noise/spiral)
  - expression maps, blink tracking, voice detection, boop interaction
- Uses PSRAM-friendly JSON allocation when enabled.
- Contains animation objects like:
  - `RainbowNoise`, `RainbowSpiral`, `GradientMaterial`, `SimpleMaterial`
  - `SpectrumAnalyzer`, `BlinkTrack`, `FFTVoiceDetection`
  - screen-space effects: `HorizontalBlur`, `VerticalBlur`, `RadialBlur`, `AntiAliasingEffect`
- Configurable behavior includes blink/voice, mouth and eye shape toggles, hue shift bindings, and auto-link specs.

### Other animation classes

- `src/Animation/BetaAnimation.h`
- `src/Animation/ClockAnimation.h`
- `src/Animation/CoelaBonkAnimation.h`
- `src/Animation/GammaAnimation.h`
- `src/Animation/ProtogenHUB75Animation.h`
- `src/Animation/ProtogenHUB75AnimationSplit.h`
- `src/Animation/TasSimpleProtogenHUB75Animation.h`

These provide alternate animation presets, clock demos, and HUB75-specific rendering examples.

## 6. Input and User Interaction

### Gesture/proximity sensor

- Supports two gesture sensor drivers:
  - `PAJ7620` when `NEW_GESTURE` is defined.
  - `APDS9960` otherwise.
- Gesture and proximity are used for "boop" detection and menu interaction.
- Implementation is in `src/Menu/ESPMenu.h`.

### BLE remote control

- `src/Menu/ESPMenu.h` implements a BLE UART service with:
  - BLE service UUID `73cf57c7-6797-46e8-8202-dc5e7f956b57`
  - RX/TX characteristic UUIDs configurable via `UserConfig`
- BLE supports both:
  - JSON command protocol (`ApplyBleJsonWrite`)
  - legacy raw control commands queued in `remoteCommandQueue`
- Reports remote state, device info, and manifest responses over BLE.
- BLE also enables remote setup of brightness, expressions, voice detection, and display mode.

### On-device menu state

- `Menu` class in `src/Menu/ESPMenu.h` stores UI state:
  - brightness, accent brightness, microphone state, expression index
  - spectrum mirror, color, face size, voice enable
  - BLE connection status and boop sensor activity
- `Menu::Update()` refreshes the M5 display with current status icons and controls.

## 7. Networking and Remote Assets

### User config management

- `src/Network/UserConfigManager.h` handles:
  - loading/saving `UserConfig` to LittleFS
  - reading unique device IDs
  - Wi-Fi connection via stored credentials or NetWizard captive portal
  - remote download of `user_config.json` from GitHub or Gitee

### Remote file synchronization

- `src/Network/RemoteFileSync.h` provides:
  - MD5-based change detection
  - failover across multiple remote sources
  - file download and local cache management
  - optional UI messages via `M5UnitGLASS2`

### Firmware update

- `src/Network/FirmwareUpdater.h` supports:
  - remote firmware manifest checks
  - primary/fallback source selection
  - version comparison and OTA update execution

### Face model update

- `src/Network/FaceModelUpdater.h` ensures the correct face JSON is present:
  - device-specific face model or fallback universal face JSON
  - remote download and local caching

## 8. Rendering and Scene Management

### Controller architecture

- `src/Controllers/Controller.h` is the abstract rendering controller.
- It manages camera arrays, render timing, brightness soft-start, and scene effects.
- Derived controllers implement `Initialize()` and `Display()`.

### ESP32-S3 matrix controller

- `src/Controllers/TasESP32S3KitV1.h` drives HUB75 panels using I2S DMA:
  - configures RGB, row, clock, latch, OE pins
  - allocates `MatrixPanel_I2S_DMA`
  - creates `VirtualMatrixPanel` for pixel rendering
  - copies render output to the local M5 display mirror

## 9. External Dependencies

PlatformIO libraries used include:

- `Adafruit GFX` / `Adafruit SSD1306` / `Adafruit BusIO`
- `NTPClient`
- `Adafruit BNO055` / `Adafruit Unified Sensor`
- `ArduinoFFT`
- `ESPAsyncWebServer` / `AsyncTCP`
- `ArduinoJson`
- `Adafruit NeoPixel`
- `ESP32-HUB75-MatrixPanel-DMA`
- `QRCode`
- `SparkFun APDS9960` or `RevEng_PAJ7620`
- `M5GFX`, `M5Unified`
- `NetWizard`
- `ElegantOTAPro` / `ElegantOTA`

## 10. Storage and Filesystem

- Uses `LittleFS` for local JSON assets and configuration files.
- Supports:
  - face model JSON files
  - animation JSON files
  - user_config.json
  - remote manifest caches

## 11. Known Issues and Limitations

From `README.md`:

- long-term memory issues may occur during extended runtime
- spectrum analyzer currently does not work and needs DSP rewrite
- ESP-NOW remote controller is marked obsolete in favor of BLE
- APDS9960 is deprecated in this port and replaced by PAJ7620

## 12. Key Source Directories

- `src/main.cpp` — startup, runtime loop, web server, OTA, config load.
- `src/Animation/` — animation engine, JSON-driven face/animation, presets.
- `src/Controllers/` — display controllers and HUB75 hardware abstraction.
- `src/Menu/` — BLE menu, gesture interaction, on-screen UI state.
- `src/Network/` — remote config, asset sync, firmware and face updates.
- `src/Flash/` — icons, materials, on-device flash assets.
- `src/Render/` — scene, camera, object, material rendering primitives.
- `src/Sensors/` — microphone and sensor front-end for voice and boop detection.

## 13. Recommended Entry Points for Developers

- `src/main.cpp` — understand the port startup flow.
- `src/Animation/JsonDrivenProtogenAnimation.h` — understand the animation and face rendering pipeline.
- `src/Menu/ESPMenu.h` — understand BLE, gesture, and menu input.
- `src/Controllers/TasESP32S3KitV1.h` — understand HUB75 output and matrix display.
- `src/Network/UserConfigManager.h` — understand remote configuration and Wi-Fi onboarding.

---

This overview is intended to help you quickly identify what features are implemented in the ESP32-S3 port and where to find their code.
