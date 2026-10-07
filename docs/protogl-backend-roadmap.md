# Path B: general ProtoGL graphics backend

Part of the [dual roadmap](esp32s31-dual-roadmap.md), 2026-10-08.
Pinned review revision: ProtoGL **`5ce6fc290a73d3f6a81f14baaee4a6f0773f8196`**,
protocol 9. The user will provide GPU firmware later. This plan defines host
work and the later firmware interface gates without claiming to have audited
the unpublished GPU implementation.

## General backend boundary

The application adapter targets ProtoGL capabilities and generation-checked
resources rather than RP2350 registers, panel DMA words or a specific ESP32
controller class. The current concrete GPU is RP2350; a different GPU/output
requires a matching implementation and capabilities, not a new expression engine.

Keep CPU-target porting in the transport/build layer. Keep panel layout and
output completion in the GPU profile. Preserve the S3 local renderer as the
numerical/image reference and an independently selected product build.

First use existing protocol-9 commands with host-computed vertices. Resident
GPU morph banks are a second, matched library/firmware extension; the newly
pulled clock-management update does not implement them. Do not make that
extension a prerequisite for proving the basic renderer/output adapter.

## B0: contract and asset mapping, available now

Define a manifest of supported mesh/layout, camera, material/blend/effect and
2D/media operations. Identify every active application material/effect and its
ProtoGL equivalent; unsupported modes must be explicit, not silently replaced.
Preserve base-color/absolute-hue, brightness, visibility, transform offsets,
negative scale, mesh winding and the logical-to-physical panel mapping.

Add a bounded `ProtoGLBackend` or `ProtoGLController` connected through the
shared interface. `Controller::Render` is currently nonvirtual and calls the
local rasterizer; replacing `Display` alone does not offload rendering. Prevent
GPU builds from constructing local camera caches, depth/geometry copies,
HUB75 DMA buffers and driver patch dependencies that they do not use.

For the first host-vertex prototype, current animation publishes world-space
vertices; send an identity object transform. For resident GPU morph mode,
upload morph-local base geometry and send the actual object transform. Applying
both world-space vertices and the original transform would deform/transform twice.

Exit: reviewed mapping specification, frame/asset ownership design, build
selection and deterministic encoder fixtures. Host simulations can verify
encoding/credits/lifetimes, but cannot certify rendering or panel wiring.

## B1: S3 host adapter and bounded resource upload, available now

Pin the pulled library revision as an implementation dependency when source
work begins. Use `PglDevice`, streamed resource uploads, sessions, exact retries,
capabilities and completion fences. Do not duplicate that transport/recovery
state machine inside the application. Preserve source buffers until DMA drains.

The face's bare mesh payload is 9,078 B before command headers and exceeds
the default 8 KB asset slot. Use streamed creation or a justified startup
capacity, with explicit encoder overflow/capacity/error handling. Keep resource
upload separate from frame commands. Reconstruct from authoritative flash
assets after each new session; generation invalidation must reach the adapter.

Start with one solid material, fixed camera and the current mesh. Publish
geometry/material snapshots at the animation boundary, then encode/upload/draw.
Retain bounded host-vertex bandwidth: 5,940 coordinate B/frame for this face.
Schedule background uploads and diagnostic queries without starving frames.

Exit before GPU arrival: host build, encoder/resource boundary tests, failure
injection for Busy/overflow/disconnect/drain, and bounded memory accounting.
Actual initialization/render success remains unverified until B2.

## B2: GPU firmware intake, boot and link qualification, after source arrives

Inspect the provided GPU build recipe, protocol/build identity, storage profile,
scene heap/caches, output configuration, clocks, pin map and allocation rules.
Publish a resolvable pinned firmware dependency instead of the current URL-less
gitlink. Decide how matching firmware/assets are built and distributed.

Use FLASH_LOCAL initially: reset -> identity/capabilities -> fresh session.
RAM_HOST UART loading can be added after runtime graphics works. Qualify
SCK/CS/D0-D3/READY/RUN and board-specific wiring separately from CPU flash/PSRAM.
Start with 1 MHz and one data lane, then capability-negotiated quad bulk data.
Control/read commands remain single-lane; the bulk prefix also stays single-lane.
Preserve READY, minimum CS gap, CRC, transfer sequence and frame-credit behavior.

Record actual SCK and test CRC/retry/stale responses, READY low, reset during
DMA, failed drain and resource reconstruction. Never free/reuse owned source
bytes on timeout. A successful reserve is not terminal batch acceptance.

Configure the actual HUB output and 128x32 two-panel mapping; examples use
64x64 and do not establish our layout. Validate a triangle plus color/grid
patterns on the actual panel before starting full expression work.

Exit: matched identity/protocol, stable qualified link and panel, bounded error
paths and usable presentation fences. NONE/render-only firmware is not a panel
qualification. If only Transferred is available, obtain external displayed-rate
evidence and label counters accordingly.

## B3: materials, effects, media and preview parity

Upload resident meshes/materials/layouts and map active shader/effect values per
frame. Compare deterministic poses, colors, depth/tie behavior and effect output
against the local reference. Cross-architecture arithmetic/raster differences
require measured tolerances or intentional behavior decisions; the existing
local direct/legacy byte equality gate does not establish GPU equality.

Present BLE/PTM/built-in media as textures/sprites. Keep phone-side GIF handling
and BLE ops intact. Use bounded texture updates; admit resident atlases only
when the GPU profile/scene heap can fit them alongside the face/effects.

Choose the OLED preview policy explicitly. Protocol 9 does not expose raw
framebuffer reads. Options are a qualified low-rate local preview with its
memory cost, a status-only preview, or a later bounded GPU readback extension.
Do not retain all local renderer allocations accidentally just to keep HUD
globals linking. Replace S3-specific DMA debug views with backend capabilities
and bounded ProtoGL metrics/resource diagnostics.

Exit: complete enabled-feature matrix, expression/speech/blink/media behavior,
OLED policy and output parity at the agreed latency/quality target.

## B4: resident morph storage and blending, matched extension

Implement the [morph-offload design](gpu-morph-offload-plan.md) after inspecting
GPU resource/vertex ownership. Upload immutable base mesh and compact morph
bank, keep CPU names/IDs/stable weight pointers, and send complete float32
weights + pose per draw. Preserve fresh-base accumulation, positive-weight
gate, original target order, half conversion and weights above one.

Use a new capability/query/resource/draw contract and a matched version decision;
the previous suggested protocol 10 is provisional, not a version already used
by the pulled library. Add the extension to both source repositories and the
GPU's compatibility/build identity together. Keep the protocol-9 host-vertex
backend available as an explicitly selected comparison build during validation.

The current full bank contains 35,200 B of compact deltas; all 99 float32 weights
need 396 B/frame, versus 5,940 B of coordinates. A historical culled load had
18,960 B of deltas and would need 168 B for 42 weights. The proposed full-bank
payload budget of about 51.8 KB includes base/indices/work array/metadata/weights,
not the rest of GPU memory; actual incremental allocation depends on reuse.

Generate a versioned binary face package off-device and stream from LittleFS
to remove the host JSON parse peak as well as resident delta storage. Host
weights stay at stable addresses for interpolators/blink. No local geometry
is allocated merely to expose pose/material APIs in this mode.

Exit: bank/weight numerical and image parity, queued-frame isolation, no
cumulative retries, malformed-resource/limit handling, resource destruction
fences and complete reset/reload/rebind. Measure peak and steady memory on both
chips, blend time and unique presentation rate.

## B5: S31 host portability

Port the proven adapter to the separately verified S31 SDK. The library's SPI
and boot GPIO validators still encode S3 pin ranges. Use target GPIO capability
checks and module exclusions; audit SPI hosts, UART/reset/strap paths, DMA
allocation/alignment/cache behavior and SDK API changes. Generic `ESP_PLATFORM`
guards do not prove portability across ESP32 chips.

Retain the transport abstraction so another supported CPU can inject a host
implementation without changing asset/animation logic. RAM_HOST bootstrap pins
and protocol are a separate optional hardware profile. Validate the complete
S31 BLE/Wi-Fi/audio/OLED/provisioning/OTA stack, not just a library compile.

Exit: S31 + ProtoGL host build and service/presentation/heap gates with no local
HUB75 dependency. Continue to support S3 + ProtoGL independently of S31 progress.

## B6: clock and throughput tuning from the pulled update

Start with the 150 MHz GPU boot profile and qualified QSPI clock. Query actual
capabilities, clock state/configuration, memory and metrics. `SetClockMHz`
accepts only 75/100/125/150/240/250/288/300/336; unsupported values do not round.
Info9's extended profile mask is required before requesting 336 MHz and is
session-scoped, so re-query after recovery. Do not assume enum ordering is
frequency ordering (250 and 288 have different historical IDs).

Tune GPU clock and QSPI separately. For each supported/qualified profile,
record actual GPU/domain rates, wire time, prepare/raster/effects time,
presentation latency, errors/underruns, power/temperature where measurable and
memory/soak results. Higher GPU clocks do not imply higher panel/SPI clocks.
VSEL-derived setpoints are not measured core voltage, and one board's
300 MHz observation is not validation of every board/profile.

Exit: documented profiles that meet the full workload/quality target, explicit
requested versus actual configuration, and stable recovery to a qualified
profile. Do not make >150 MHz operation the prerequisite for basic correctness.

## Concrete application/library changes

| Area | Planned changes |
|---|---|
| `platformio.ini`, backend selection and `main.cpp` | S3/S31 ProtoGL build variants, qualified transport/pins, init/recovery and telemetry |
| `Controller.h` and new ProtoGL adapter | Shared frame/media/brightness/preview interface and explicit local-render bypass |
| `JsonDrivenProtogenAnimation.h`, face/scene state | Geometry-independent pose/material/weight publication and stable bindings |
| Local controller, `ESPMenu.h`, serial debug | Backend-aware HUD and diagnostics; no mandatory S3 DMA globals |
| Asset tools, LittleFS, face sync | Streamed/binary resources, schema/fingerprint and reconstruction |
| ProtoGL library and later GPU firmware | Morph extension, capabilities/limits/ownership/version and host-target transport audit |
| Release manifests | CPU/backend identity plus matching GPU build/protocol; tested partial-update recovery |

All phases use the shared measurement/release gates. B0/B1 can begin now; GPU
firmware intake and hardware phases are scheduled for when the user supplies
that source. No application/GPU firmware implementation is included in this plan.
