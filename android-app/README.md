# ProtoTracer Remote — Android BLE App

Native Android recreation of the `web-app/` ProtoTracer Remote.

## What It Mirrors

- BLE service-filtered ProtoTracer scan
- Manual connection by BLE address or device-name filter
- RX/TX characteristic discovery from the ProtoTracer service
- Notification-driven JSON receive path with chunk reassembly
- `config.get` manifest request after connect
- Manifest-driven expression grid
- Brightness, hue shift, lip sync, and display mode controls
- Ping, manifest refresh, disconnect, and event log actions
- English and Simplified Chinese UI via the in-app language toggle

## BLE Protocol

| Item | Value |
|------|-------|
| Service UUID | `73cf57c7-6797-46e8-8202-dc5e7f956b57` |
| RX characteristic | First writable / write-without-response characteristic in the service |
| TX characteristic | First notify characteristic in the service |
| Chunk size | 160 bytes |

Commands match the web app:

```json
{"op":"config.get"}
{"op":"control.set","expression":0}
{"op":"control.set","brightness":105}
{"op":"control.set","hue_shift":180.0}
{"op":"control.set","voice_enabled":true}
{"op":"control.set","display_mode":1}
{"op":"ping"}
```

Incoming JSON handles `control.state`, `pong`, and manifest-style payloads containing `visual`, `device`, or `pairing`.

## Hue Shift vs. Base Color

The manifest's `visual` object may carry the device's user-configured base expression color as
`red` / `green` / `blue` integers (0-255, from `user_config.json`; firmware default 25/125/235).
When all three are present, the app computes a **base hue** (standard RGB->HSV, 0-360°) and treats
the hue slider and preset swatches as **target hues**:

- Sending: `hue_shift = (target - baseHue) mod 360`, normalized to [0, 360). The firmware applies
  `hue_shift` as an absolute rotation about the gray axis, so this relative computation makes the
  face land on the requested target hue.
- Receiving: a `control.state` `hue_shift` echo `S` is displayed as target hue `(S + baseHue) mod 360`
  on the slider, label, and preset highlighting.

On manifest receipt — before any `control.state` hue echo — the slider and value label are
initialized to the base hue, matching the device's boot state (firmware hue shift is not
persisted and boots at 0). The slider thumb is tinted with the effective color at the
displayed target hue (base saturation/value, or fully saturated when the base is unknown).

If the manifest has no `red`/`green`/`blue` (older firmware), the app falls back to legacy
absolute behavior: the slider value is sent as-is and echoes are displayed as-is.

## Build

Open `android-app/` in Android Studio and run the `app` configuration, or build from a shell with Gradle 8.5+:

```powershell
gradle assembleDebug
```

This workspace currently has Gradle 8.5 cached under the user Gradle wrapper cache; the debug build was verified with:

```powershell
%USERPROFILE%\.gradle\wrapper\dists\gradle-8.5-bin\5t9huq95ubn472n8rpzujfbqh\gradle-8.5\bin\gradle.bat assembleDebug
```

## Requirements

- Android device with BLE support
- Android 6.0 / API 23 or newer
- Bluetooth permissions granted at runtime
- Location permission and Location services enabled on Android 6 through 11 for BLE scanning