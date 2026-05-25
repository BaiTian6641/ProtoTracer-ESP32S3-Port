# ProtoTracer Remote — Android BLE Companion App

A native Android companion app for ProtoTracer ESP32-S3 firmware.

## Features

- **BLE device scanning** — discover nearby ProtoTracer devices
- **QR code scanning** — scan the RX UUID from device display (no Google Play Services)
- **Expression control** — browse and select expressions with Prev/Next + Confirm
- **Brightness slider** — adjust LED panel brightness 0–255
- **Hue Shift slider** — adjust color hue with live preview
- **Voice Detection toggle** — enable/disable microphone voice detection
- **Fan toggle** — enable/disable cooling fan
- **Bilingual** — English (en_US) and Simplified Chinese (zh_CN)
- **Device info** — displays animation name, hardware revision, expression count

## BLE Protocol

The app communicates with the ProtoTracer firmware over BLE:

| Item | Value |
|------|-------|
| Service UUID | `73cf57c7-6797-46e8-8202-dc5e7f956b57` |
| RX Characteristic | Discovered via service, or obtained by QR scan |
| TX Characteristic | Discovered via service (has NOTIFY property) |
| MTU | 160 bytes per chunk (matches firmware `kBleJsonChunkBytes`) |

**JSON Commands:**

```json
// Discovery (sent automatically after connection)
{"op":"pair.discover"}

// Control (expression index, brightness, hue shift, voice)
{"op":"control.set","expression":0,"brightness":128,"hue_shift":45.0,"voice_enabled":true}
```

## How to Build

Open the `android-app/` directory in **Android Studio** (Arctic Fox or later).
Gradle will sync and download dependencies. Then:

1. Connect an Android device (API 26+) via USB
2. Click **Run** → **Run 'app'**

Or build from the command line:
```bash
cd android-app
./gradlew assembleDebug
```

### Prerequisites
- Android SDK 34
- JDK 17
- Android device with BLE support (API 26+)

## Dependencies

| Library | Purpose |
|---------|---------|
| ZXing Android Embedded | QR code scanning (**no Google Play Services**) |
| Nordic BLE Library | BLE abstraction (optional, direct Android BLE APIs used) |
| Gson | JSON serialization |
| Material Components | UI widgets (Sliders, Switches, Buttons) |
| AndroidX Lifecycle + Coroutines | State management |
