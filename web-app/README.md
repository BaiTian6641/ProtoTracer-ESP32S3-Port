# ProtoTracer Web Remote Control

A browser-based remote control application for ProtoTracer LED face displays, using the **Web Bluetooth API** to communicate directly with your ESP32-S3 device over BLE.

## Features

- **Expression Selection** — Browse and trigger all facial expressions configured in your animation JSON
- **Brightness Control** — Real-time slider (0–255) with live preview
- **Hue Shift** — Pick a target hue with presets and a gradient slider; the required shift is computed relative to the device's configured base color
- **Lip Sync Toggle** — Enable/disable microphone-driven voice detection
- **Display Mode Toggle** — Switch display modes on the device
- **Device Ping** — Verify the BLE connection is alive
- **Manifest Auto-Discovery** — Automatically reads expression names, animation info, and device identity from the firmware
- **Event Log** — Full log of all BLE commands sent and responses received
- **Dark Theme** — Cyberpunk-inspired UI with gradient accents and glowing status indicators

## Requirements

### Browser

Web Bluetooth is supported in:

| Browser | Desktop | Android |
|---------|---------|---------|
| **Chrome** 79+ | ✅ | ✅ |
| **Edge** 79+ | ✅ | ✅ |
| **Opera** 66+ | ✅ | ✅ |
| Samsung Internet | — | ✅ |
| Firefox | ❌ | ❌ |
| Safari / iOS | ❌ | ❌ |

> On **Windows**, you may need to enable the `chrome://flags/#enable-experimental-web-platform-features` flag if the device picker doesn't show your ProtoTracer.

### Device

- ProtoTracer firmware running on ESP32-S3 with BLE UART service enabled
- BLE advertising active (service UUID: `73cf57c7-6797-46e8-8202-dc5e7f956b57`)
- Animation JSON loaded with expressions configured

## Quick Start

### Option 1: GitHub Pages (recommended — zero setup)

The web app is automatically deployed to **GitHub Pages** on every push to `main`.  
After the first deployment, open:

```
https://<your-username>.github.io/<repo-name>/
```

> Replace `<your-username>` with your GitHub username and `<repo-name>` with this repository's name.

**One-time setup** (already done if `.github/workflows/pages.yml` exists):
1. Go to **Settings → Pages** in your GitHub repo
2. Under "Build and deployment", set **Source** to **GitHub Actions**
3. Push to `main` — the workflow deploys automatically

No server, no hosting, no configuration needed after that.

### Option 2: Local (for development)

```bash
cd web-app
python -m http.server 8080
# Then open http://localhost:8080
```

Chrome treats `localhost` as a secure context, so Web Bluetooth works without HTTPS when served from localhost.

2. **Power on your ProtoTracer** — Ensure BLE is advertising.

3. **Click "Scan for ProtoTracer"** — Your browser will show a device picker dialog. Select your ProtoTracer device.

4. **The manifest loads automatically** — Expression buttons, brightness, and hue controls will populate.

5. **Control your device** — Click an expression, drag a slider, or toggle options. Changes are sent instantly over BLE.

### Manual Connection

If auto-scan (service UUID filter) doesn't find your device:

1. Enter your device's BLE name in the manual input field (e.g., `Protogen-01`)
2. Click **Connect**
3. Select the device from the browser dialog

## Architecture

```
web-app/
├── index.html          # Main application page
├── css/
│   └── style.css       # Dark cyberpunk theme
├── js/
│   ├── ble.js          # Web Bluetooth protocol layer
│   └── app.js          # UI controller and application logic
└── README.md           # This file
```

### BLE Protocol

The app implements the ProtoTracer BLE JSON protocol:

| Direction | Op Code | Description |
|-----------|---------|-------------|
| App → Device | `config.get` | Request device manifest (expressions, colors) |
| App → Device | `control.set` | Set expression, brightness, hue, voice, display mode |
| App → Device | `ping` | Connectivity check |
| Device → App | *(manifest)* | JSON with `device`, `pairing`, `visual`, `repo` fields |
| Device → App | `control.state` | Acknowledgment of control changes |
| Device → App | `pong` | Response to ping |

**Service UUID:** `73cf57c7-6797-46e8-8202-dc5e7f956b57`

The protocol is chunked at 160 bytes per BLE notification to accommodate ESP32 BLE stack limitations.

### `ble.js` — Protocol Layer

- IIFE module exposing `ProtoTracerBLE` global
- Handles BLE scanning, connection, service/characteristic discovery
- Implements chunked writes (160-byte max per notification)
- Assembles chunked JSON responses from the device
- Emits events: `status`, `connected`, `disconnected`, `manifest`, `controlState`, `pong`, `sent`, `received`

### `app.js` — Application Controller

- IIFE that wires `ProtoTracerBLE` events to the DOM
- Manages UI state (current expression, brightness, hue, toggles)
- Renders expression grid from manifest data
- Provides toast notifications and scrollable event log
- Handles all user interactions (sliders, toggles, buttons)

## Base color & hue shift

The hue slider and preset swatches select a **target hue** — the color the face should become. The firmware's `hue_shift` field, however, is an *absolute rotation of the device's base color* (RGB-space rotation about the gray axis, in degrees), not a target hue. The app converts between the two using the base color reported by the device.

### Manifest fields

The BLE manifest (response to `config.get` / `pair.discover` / `pair.info`) carries the user-configured base expression color inside the `visual` object:

| Field | Type | Meaning |
|-------|------|---------|
| `visual.red`   | int 0–255 | Base color red channel |
| `visual.green` | int 0–255 | Base color green channel |
| `visual.blue`  | int 0–255 | Base color blue channel |

All three must be present and finite; each is clamped to 0–255. From them the app computes the base hue **B** (standard RGB→HSV hue, 0–360; grayscale bases yield 0) and logs the received color once (`log.baseColor`).

### Relative hue computation

- On user selection of target hue **T** (slider release or preset tap), the app sends `control.set` with `hue_shift = (T − B) mod 360`, normalized into [0, 360).
- A `control.state` echo replies with the **raw** `hue_shift` value **S** that was applied; the UI displays the resulting target hue `(S + B) mod 360` on the slider and badge.

### Connect-time display

The firmware's hue shift is not persisted (it boots at 0) and the manifest does not carry the current shift, so on manifest receipt — until the first `control.state` hue echo arrives — the wheel thumb and badge are initialized to the **base hue**, i.e. the color the face is actually showing after boot. The first echo (or any hue interaction) takes over from there.

### Legacy fallback

If the manifest contains no `visual.red/green/blue` (older firmware), the base hue is unknown (`baseColor = null`): the slider value is sent as-is and echoes are displayed as-is, matching the original behavior.

The badge next to the slider is tinted with an approximation of the effective color — `hsl(displayed target hue, base saturation%, base lightness%)` derived from the base RGB, or fully saturated (`100%, 50%`) when the base color is unknown.

### Regression test

`hue-math.test.cjs` extracts the hue helpers from `js/app.js` and asserts the RGB→hue math, the `(T − B) mod 360` send formula, and the echo round-trip. Run it with `node hue-math.test.cjs` after touching the hue logic.

`fw-rotation-sim.test.cjs` replays the firmware's exact `ProtoRGBColor::HueShift` (normalized quaternion == Rodrigues rotation about the gray axis, constrain + uint8 truncation) against the app-side send formula, proving each swatch tap lands on the swatch hue on-device (~1° for typical user colors, ≤8.2° for pure primary bases — an inherent RGB-rotation vs HSV-hue property, not an app bug). Run with `node fw-rotation-sim.test.cjs`.

## Compatibility Notes

- **Windows:** Chrome may require the "Experimental Web Platform features" flag enabled at `chrome://flags`
- **macOS:** Works out of the box in Chrome/Edge
- **Linux:** BlueZ may need `Experimental Web Platform features` flag; some distributions require `chrome://flags/#enable-web-bluetooth-new-permissions-backend`
- **Android:** Works in Chrome; grant location permission when prompted (required by Android BLE stack)

## Troubleshooting

| Symptom | Likely Cause | Solution |
|---------|-------------|----------|
| "No devices found" in picker | BLE not advertising | Restart ProtoTracer; check firmware |
| Device picker doesn't open | Browser doesn't support Web Bluetooth | Use Chrome or Edge |
| Connects but no expressions | Manifest parse failed | Check animation JSON on device |
| "GATT operation failed" | Device went out of range | Move closer; reconnect |
| Slider doesn't update device | BLE write failed | Check connection; reconnect |
