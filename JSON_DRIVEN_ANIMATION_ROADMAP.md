# JSON Driven Animation Roadmap

Last updated: 2026-05-13

This document tracks the JsonDrivenProtogenAnimation migration work for ProtoTracer on ESP32-S3. It records the current status, the main code paths, and the smallest safe implementation slices for the remaining work.

## Current Validation State

- Remote asset priority is now Gitee first, then GitHub fallback, for user config, face model, and animation JSON.
- `esp32s3-RELEASE` builds successfully after aligning the async stack on `ESP32Async/AsyncTCP@3.4.10` and `ESP32Async/ESPAsyncWebServer@3.11.0`.
- `esp32s3` also builds successfully after removing the duplicate standard `ElegantOTA` dependency so only `ElegantOTAPro` is linked.
- The remaining ESP32-S3 risk is now runtime validation on hardware rather than a current compile or link blocker.

## Status

| Item | Status | Notes |
| --- | --- | --- |
| 1. User RGB color drift | Local fix applied, build validated | Palette state is now reset before default material registration so repeated JSON loads do not inherit mutated colors. |
| 2. Slow visemes | Fixed in current branch, build validated | auto_link now applies after JSON load, acts as per-morph overrides, and old vrc_v_uh configs are accepted through aliasing. |
| 3. Image sequence custom material | Planned | Existing ImageSequence support is compile-time only. |
| 4. Unified remote file pull path | Implemented, build validated | `RemoteFileSync` now handles user config, face model, and animation sync with shared auth, MD5, temp-file replacement, and progress UI. |
| 5. Device-specific face model | Implemented, build validated | Face download and face load now prefer `<device_id>_face.json` and fall back to `universal_face.json`. |
| 6. Auto firmware update via GitHub/Gitee | Planned | ElegantOTA Pro is currently used only as a manual OTA portal. |
| 7. Harden JSON animation path | In progress, build validated | Startup diagnostics now report remote animation sync source plus local animation fallback/load path; unknown-key and bad-reference reporting still remain. |
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

### Items 4 and 5: Shared remote sync and device-specific face model

Current fix:
- Added `src/Network/RemoteFileSync.h` and `src/Network/RemoteFileSync.cpp` to centralize:
  - base URL path construction
  - GitHub and Gitee auth headers
  - remote `.md5` fetch
  - local MD5 compare
  - temp-file download and replace
  - shared progress UI updates
- `src/Network/UserConfigManager.cpp` now uses `RemoteFileSync` instead of its own dedicated HTTP and MD5 implementation.
- `src/Network/AnimationDownloader.cpp` now uses `RemoteFileSync`.
- `src/Network/FaceModelUpdater.cpp` now uses `RemoteFileSync` and tries `<device_id>_face.json` before `universal_face.json`.
- `src/Animation/JsonDrivenProtogenAnimation.h` now loads `/<device_id>_face.json` first and falls back to `/universal_face.json`.
- `src/main.cpp` now wires config, face, and animation sync through the same GitHub and Gitee base URLs and prefers Gitee before GitHub fallback.

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
- User config, animation, and face model now share `RemoteFileSync` for auth, MD5, download, and atomic replace behavior.
- Startup now prefers Gitee first and uses GitHub only as fallback.

Smallest safe slice:
- Validate the Gitee-first order on hardware with GitHub intentionally blocked.
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
- ElegantOTA Pro currently exposes portal features such as `begin()`, `setTitle()`, `setID()`, `setFWVersion()`, callbacks, and reboot handling.
- There is no built-in remote release checker or automatic pull-from-repo updater in the local library wrapper.

Smallest safe slice:
- Add a dedicated `FirmwareUpdater` class.
- Fetch latest firmware metadata from GitHub first, then Gitee.
- Compare a current firmware version string with the remote version.
- Use the ESP32 update path directly, while keeping ElegantOTA Pro as the local portal and status surface.

ESP32-S3 notes:
- Confirm partition layout supports OTA firmware replacement safely.
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
2. Continue hardening item 7 with warnings for unknown morphs, materials, and effect references.
3. Implement item 3 once the JSON schema surface is stable enough to extend safely.
4. Split out the formal schema document for item 8.
5. Implement item 6 after the downloader and validation infrastructure are already stable.
6. Run the remaining hardware-heavy validation from item 9.