# JSON Driven Animation Roadmap

Last updated: 2026-05-13

This document tracks the JsonDrivenProtogenAnimation migration work for ProtoTracer on ESP32-S3. It records the current status, the main code paths, and the smallest safe implementation slices for the remaining work.

## Current Validation State

- Remote asset selection now probes Gitee and GitHub and prefers the lower-latency healthy remote for user config, face model, and animation JSON.
- Auto firmware update now has a first milestone implementation: manifest-driven OTA checks use the same latency-based Gitee/GitHub selection and stream the application binary through the ESP32 OTA slot.
- `esp32s3-RELEASE` builds successfully after aligning the async stack on `ESP32Async/AsyncTCP@3.4.10` and `ESP32Async/ESPAsyncWebServer@3.11.0`.
- `esp32s3` also builds successfully after removing the duplicate standard `ElegantOTA` dependency so only `ElegantOTAPro` is linked.
- The remaining ESP32-S3 risk is now runtime validation on hardware rather than a current compile or link blocker.

## Status

| Item | Status | Notes |
| --- | --- | --- |
| 1. User RGB color drift | Local fix applied, build validated | Palette state is now reset before default material registration so repeated JSON loads do not inherit mutated colors. |
| 2. Slow visemes | Solved | auto_link now applies after JSON load, acts as per-morph overrides, and old vrc_v_uh configs are accepted through aliasing. |
| 3. Image sequence custom material | Planned | Existing ImageSequence support is compile-time only. |
| 4. Unified remote file pull path | Implemented, build validated | `RemoteFileSync` now handles user config, face model, and animation sync with shared auth, MD5, temp-file replacement, and progress UI. |
| 5. Device-specific face model | Implemented, build validated | Face download and face load now prefer `<device_id>_face.json` and fall back to `universal_face.json`. |
| 6. Auto firmware update via GitHub/Gitee | In progress, build validated | FirmwareUpdater now probes the manifest on both remotes, tries the lower-latency healthy source first, compares versions, and applies OTA from the app binary. |
| 7. Harden JSON animation path | In progress, mostly complete | Startup diagnostics now report remote animation sync source plus local animation fallback/load path; most config flow is solid, with remaining work mostly around reference warnings. |
| 8. Standardize animation JSON | Draft started here | Current supported keys are documented below. |
| 9. ESP32-S3 validation pass | In progress | Both `esp32s3` and `esp32s3-RELEASE` now build cleanly; hardware, network-fallback, and large-asset tests remain. |

## Recently Fixed

### Item 1: User RGB color drift

Root cause:
- JsonDrivenProtogenAnimation kept mutable gradient, rainbow, and background palette arrays alive across material re-registration.
- When JSON materials were reloaded, partial overrides could reuse already-mutated palette values instead of a clean base.

Current fix:
- `ResetMaterialPalettes()` now rebuilds user and stock palettes before `RegisterDefaultMaterials()`.
- File: `src/Animation/JsonDrivenProtogenAnimation.h`

Follow-up:
- Rebuild and test repeated config reloads on hardware.
- Confirm the visual issue reported by users is fully gone for `gradientSpectrum`, `rainbowMat`, and `backgroundMat`.

### Item 2: Viseme responsiveness

Root cause:
- `LoadAnimationConfig(config)` previously ran after `AutoLinkMorphs()`, so the JSON `auto_link` viseme frame overrides were ignored.
- `auto_link` also behaved like a whitelist instead of an override layer, which made the JSON animation path diverge from the older TasSimple animation path.

Current fix:
- `LoadAnimationConfig(config)` now runs before `AutoLinkMorphs()` and `LinkParameters()`.
- All morphs are registered, while `auto_link` overrides per-morph frame and basis/goal values.
- `vrc_v_*` morphs default to fast registration, and `vrc_v_uh` is accepted as an alias for `vrc_v_dd`.
- Example config updated to use `vrc_v_dd`, `vrc_v_rr`, and `vrc_v_ch` directly.

Main files:
- `src/Animation/JsonDrivenProtogenAnimation.h`
- `src/Animation/example_animation.json`

Remaining check:
- Hardware validation with live microphone input on ESP32-S3.

### Item 6: Auto firmware update milestone

Current fix:
- Added `src/Network/FirmwareUpdater.h` and `src/Network/FirmwareUpdater.cpp`.
- Startup now checks firmware metadata before face and animation sync so a newer firmware can be applied and rebooted early in boot.
- Firmware update source order now matches the rest of startup: probe both remotes, prefer the lower-latency healthy source, then fall back if the update check fails.
- The updater uses the application OTA binary (`firmware.bin` target content), not the merged serial-flash image.
- ElegantOTA Pro now reports the same firmware version string used by the auto-update check instead of a separate hardcoded portal version.

Current manifest contract:
- Default manifest name for ESP32-S3 builds: `esp32s3.json`
- Default manifest name for ESP32-P4 builds: `esp32p4.json`
- Required JSON keys:
  - `version`
  - `file` (or `filename`, `bin`, `url`)
- Optional JSON keys:
  - `md5`

Example manifest:
```json
{
  "version": "1.0.1",
  "file": "firmware.bin",
  "md5": "0123456789abcdef0123456789abcdef"
}
```

Remaining check:
- Publish the manifest and application binary in the remote repo layout actually used by devices.
- Verify a full on-device upgrade from an older build to a newer build.
- Verify failed downloads continue normal startup without corrupting the running app.
- Decide whether future releases should override the default manifest filename per environment, for example `esp32s3-release.json`.

### Items 4 and 5: Shared remote sync and device-specific face model

Current fix:
- Added `src/Network/RemoteFileSync.h` and `src/Network/RemoteFileSync.cpp` to centralize:
  - base URL path construction
  - GitHub and Gitee auth headers
  - latency probing and remote ordering
  - remote `.md5` fetch
  - local MD5 compare
  - temp-file download and replace
  - shared progress UI updates
- `src/Network/UserConfigManager.cpp` now uses `RemoteFileSync` instead of its own dedicated HTTP and MD5 implementation.
- `src/Network/AnimationDownloader.cpp` now uses `RemoteFileSync`.
- `src/Network/FaceModelUpdater.cpp` now uses `RemoteFileSync` and tries `<device_id>_face.json` before `universal_face.json`.
- `src/Animation/JsonDrivenProtogenAnimation.h` now loads `/<device_id>_face.json` first and falls back to `/universal_face.json`.
- `src/main.cpp` now wires config, face, and animation sync through the same GitHub and Gitee base URLs and prefers the lower-latency healthy remote before fallback.

Build validation:
- `esp32s3-RELEASE` passes after pinning the async web stack and aligning `ElegantOTAPro` onto the same `ESP32Async` dependency family.
- `esp32s3` passes after removing the duplicate standard `ElegantOTA` dependency from that environment.

Remaining check:
- Cold boot with only `/<device_id>_face.json` present.
- Cold boot with only `/universal_face.json` present.
- GitHub unavailable with Gitee fallback available.
- No Wi-Fi with previously cached local face and animation files.

### Item 7: JSON animation path hardening

Current fix:
- `src/Network/AnimationDownloader.cpp` now logs whether animation sync succeeded via Gitee or GitHub and when it falls back between them.
- `src/Animation/JsonDrivenProtogenAnimation.h` now logs:
  - the preferred animation JSON path it tried
  - when it falls back to `/example_animation.json`
  - when both files are missing and it uses the built-in minimal default
  - which source file failed JSON parsing

Remaining check:
- Add warnings for unknown morph names in `auto_link`, `flipped_morphs`, and expression parameters.
- Add warnings for missing materials and unknown effect types.
- Confirm the new logs are readable enough on real hardware bring-up.

## Remaining Work

### 3. Allow image sequence as a custom material

Current code paths:
- `src/Animation/ImageSequence.h`
- `src/Materials/Image.h`
- `src/Animation/CoelaBonkAnimation.h`
- `src/Animation/JsonDrivenProtogenAnimation.h`

Current state:
- ImageSequence already inherits from Material.
- Existing image sequences are compile-time assets under `src/Flash/ImageSequences`.
- JSON material registration does not know how to create or own an ImageSequence material.

Smallest safe slice:
- Extend `RegisterMaterialsFromJson()` to support a material type such as `imageSequence`.
- Start with LittleFS-backed or compile-time-registered named sequences instead of arbitrary raw frame blobs.
- Register the created sequence in `materialRegistry` and update it per frame in `JsonDrivenProtogenAnimation::Update()`.

ESP32-S3 notes:
- Frame data can get large quickly; avoid loading full decoded sequences into internal RAM.
- Prefer PSRAM or LittleFS streaming for larger animations.

### 4. Optimize required remote file pulls into one GitHub path and one Gitee path

Current code paths:
- `src/Network/AnimationDownloader.cpp`
- `src/Network/FaceModelUpdater.cpp`
- `src/Network/UserConfigManager.cpp`
- `src/Auth/TassAuthToken.h`
- `src/main.cpp`

Current state:
- User config, animation, and face model now share `RemoteFileSync` for auth, MD5, download, atomic replace behavior, and latency-based source ordering.
- Startup now probes both remotes and prefers the lower-latency healthy source while keeping the other as fallback.

Smallest safe slice:
- Validate latency-based source choice on hardware with one remote intentionally slowed or blocked.
- Confirm config, face, and animation all remain bootable from cached local files when remote sync fails.

ESP32-S3 notes:
- Reuse one HTTP path implementation to reduce duplicated bugs in slow or unstable Wi-Fi conditions.

### 5. Add device-specific face model using `<device_id>_face.json` and `<device_id>_face.md5`

Current code paths:
- `src/Animation/JsonDrivenProtogenAnimation.h`
- `src/Network/FaceModelUpdater.cpp`
- `src/Network/UserConfigManager.h`
- `src/main.cpp`

Current state:
- Face sync and face load now prefer `/<device_id>_face.json`.
- Both paths fall back to `/universal_face.json` so existing deployments still boot.
- Startup already knows `device_id`, and animation loading already uses `<device_id>_animation.json`.

Smallest safe slice:
- Validate the new fallback path on hardware with and without a device-specific face file present.
- Confirm the remote repo actually publishes both `<device_id>_face.json` and `<device_id>_face.md5` where needed.
- Add serial diagnostics if both device-specific and universal files are missing after sync.

ESP32-S3 notes:
- Face JSON is one of the largest files in the boot path; keep PSRAM allocation enabled.
- Avoid repeated parse attempts when the device-specific face file is absent.

### 6. Add auto firmware update from GitHub or Gitee and integrate with ElegantOTA Pro

Current code paths:
- `src/main.cpp`
- `lib/ElegantOTAPro-3-1-4/src/ElegantOTAPro.h`
- `src/Auth/TassAuthToken.h`

Current state:
- ElegantOTA Pro still provides the local upload portal and now shares the same firmware version string as the auto-update path.
- A first `FirmwareUpdater` implementation now checks a remote manifest and applies OTA directly through the ESP32 update API.
- The current milestone assumes the manifest and firmware binary live under the same Gitee/GitHub base URLs already used for config and animation assets.

Smallest safe slice:
- Validate the end-to-end upgrade path on hardware using a published manifest and `firmware.bin`.
- Add version/channel overrides when you want separate debug/release or per-target manifests.
- Consider adding a cached cooldown or last-failed version record if repeated failed update attempts become noisy on boot.

ESP32-S3 notes:
- The current `default_8MB.csv` layout already has `otadata`, `app0`, and `app1`, so OTA app replacement is supported.
- Remote auto-update should target the application binary, not `firmware_merged.bin`, because the merged image is only for serial flashing at offset `0x0`.
- Network timeout and partial download handling matter more here than on JSON assets.

### 7. Refine the JSON animation path so it is as solid as TasSimpleProtogenHUB75Animation, but more flexible

Current code paths:
- `src/Animation/JsonDrivenProtogenAnimation.h`
- `src/Animation/TasSimpleProtogenHUB75Animation.h`
- `src/Animation/example_animation.json`

Current state:
- JSON path is already more flexible than TasSimple, but it still relies on permissive parsing and silent fallback.
- Missing morph names, unknown keys, and malformed sections are not reported clearly enough.

Smallest safe slice:
- Add validation warnings for unknown morphs, missing materials, and bad effect types.
- Improve serial diagnostics around fallback to `example_animation.json`.
- Keep the boot path tolerant, but make misconfiguration visible.

ESP32-S3 notes:
- Favor validation that does not require a second full parse of large JSON.

### 8. Standardize animation configuration JSON and create a real schema document

Current supported top-level keys:
- `user`
- `animatiuon_num`
- `x_offset`
- `y_offset`
- `screen_effect_register`
- `flipped_morphs`
- `auto_link`
- `Mat_register`
- `offsetFaceInd`
- `offsetFaceIndSA`
- `expressions`

Current expression keys:
- `reset`
- `faceMat`
- `backgroundMat`
- `voice_enable`
- `blink`
- `show_mouth`
- `eye_shape`
- `brightness` (optional per-animation brightness override, 0-255; omit to follow the menu/user config default)
- `scene_effect`
- `interpolation`
- `anim_parameter`

Current built-in material names:
- `gradientSpectrum`
- `rainbowMat`
- `backgroundMat`
- `rainbowNoise`
- `rainbowSpiral`
- `SpectrumAnalyzer`
- `redMat`
- `greenMat`
- `blueMat`
- `yellowMat`
- `purpleMat`
- `whiteMat`
- `orangeMat`

Current built-in effect types:
- `HorizontalBlur`
- `VerticalBlur`
- `RadialBlur`
- `AntiAliasingEffect`

Next step for standardization:
- Split this roadmap from a dedicated schema document, for example `ANIMATION_SCHEMA.md`.
- Document required keys, optional keys, defaults, fallback behavior, and naming rules.
- Add the future `imageSequence` material spec here once item 3 is implemented.

### 9. Make sure all changes stay solid on ESP32-S3

Relevant files:
- `platformio.ini`
- `src/Animation/JsonDrivenProtogenAnimation.h`
- `src/Morph/JsonNukudeFace.h`
- `src/Network/AnimationDownloader.cpp`
- `src/Network/FaceModelUpdater.cpp`
- `src/main.cpp`

Constraints already visible:
- PSRAM is required for larger JSON assets.
- LittleFS must stay reliable through boot, download, and parse.
- Downloads happen over a single Wi-Fi connection and are sensitive to unstable networks.
- Audio processing, scene updates, and JSON-driven assets all share RAM budget.

Validation checklist:
- Build `esp32s3` and `esp32s3-RELEASE` after each major slice.
- Completed in this pass: `esp32s3` build.
- Completed in this pass: `esp32s3-RELEASE` build.
- Test cold boot with no Wi-Fi.
- Test cold boot with GitHub blocked and Gitee available.
- Test large face JSON plus large animation JSON together.
- Test repeated config reloads without palette drift.
- Test live viseme response on hardware microphone input.

## Recommended Implementation Order

1. Validate items 1, 2, 4, and 5 on real ESP32-S3 hardware.
2. Validate the new auto-update milestone on hardware with a real published manifest and app binary.
3. Continue hardening item 7 with warnings for unknown morphs, materials, and effect references.
4. Implement item 3 once the JSON schema surface is stable enough to extend safely.
5. Split out the formal schema document for item 8.
6. Run the remaining hardware-heavy validation from item 9.