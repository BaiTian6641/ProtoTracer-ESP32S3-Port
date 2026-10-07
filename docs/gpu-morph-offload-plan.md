# GPU morph offload: design and change list

Follow-up to the [S31 / ProtoGL architecture review](esp32s31-protogl-architecture-plan.md),
2026-10-08. Inspected application baseline `5772870` and ProtoGL
`c099f90fe09e7d8db7b976048a8ab98905466642`. This document proposes changes;
it does not implement them. The RP2350 firmware source is still unavailable
through the inspected ProtoGL gitlink, so its internal paths remain to be mapped.

## Recommendation and expected effect

Offload **morph storage and vertex blending** to the GPU. Keep expression
interpolation, blink overrides, lip-sync decisions, boop state and sensor input
on the main CPU. Send a completed weight snapshot and object/camera/material
state per frame. Upload the immutable mesh and morph bank once per session.

The current ProtoGL `DrawObjectMorphed` API is an inline final-vertex override:
it still sends `vertexCount * 12` bytes. Neither it nor `UpdateVerticesDelta`
establishes resident morph targets or GPU blending. Add an explicit morph-bank
resource and weights-based draw rather than changing existing vertex semantics.

Computed from `src/Morph/universal_face.json`:

| Quantity | Value |
|---|---:|
| Base vertices / triangles | 495 / 523 |
| Morph targets / sparse delta entries | 99 / 4,400 |
| Compact delta data (uint16 index + 3 binary16 components) | 35,200 B |
| Base float32 positions / uint16 triangle indices | 5,940 B / 3,138 B |
| One working float32 position array | 5,940 B |
| Complete 99-target float32 weight snapshot | 396 B |

An earlier `build_optimized_serial.log` records a culled device load of 42
targets / 2,370 entries / 18,960 B of delta data; that is historical evidence,
not a measurement of the current device configuration. A 42-weight snapshot
would occupy 168 B. Geometry traffic therefore falls from 5,940 B/frame to
396 B/frame for all 99 targets, or 168 B for that subset: approximately 15x
or 35x less payload, excluding draw headers and other frame traffic.

With a proposed 12-byte target directory, the full bank's delta data, directory,
base positions, indices, one blend array and weights total about 51,802 B.
This is an illustrative payload budget, not the GPU's incremental allocation:
some base/working arrays may already exist or be reusable. It excludes firmware,
render caches, clipping, framebuffer/effects, HUB75 DMA and upload staging.
[RP2350 has 520 KB of SRAM](https://www.raspberrypi.com/products/rp2350/),
shared by those users. Query and enforce actual scene-heap/contiguous limits;
the chip's total SRAM is not an available morph-bank budget.

On the host, compact deltas currently occupy PSRAM. Offload frees roughly
18.5 KiB for the historical subset, or 34.4 KiB if all current targets are
resident, plus their allocation/metadata overhead. Removing local geometry,
renderer scratch and panel buffers saves additional memory. Morph offload
alone does not remove those allocations or the existing large JSON parse peak.

It moves blending work to RP2350, so measure total presented frame rate. Lower
host CPU work and wire traffic are expected; higher FPS depends on the GPU's
blend + transform + raster + output workload, not just the transfer savings.

## Proposed resource and frame contract

- A generation-checked **MorphBank** belongs to a specific immutable base mesh,
  identified by its handle and asset fingerprint. The fingerprint covers vertex
  ordering, base geometry, morph ordering and conversion/schema version.
- A compact directory records each target's entry offset/count. Each entry is
  `{vertexIndex:u16, dx:binary16, dy:binary16, dz:binary16}` in little-endian
  format. No wire pointers, C++ object layouts or JSON parsing on the GPU.
- Reuse bounded `STREAM_BEGIN/DATA/COMMIT` asset uploads for banks. Declare total
  counts/bytes/checksum; validate and allocate within negotiated limits before
  making the bank visible. Avoid a second complete temporary copy. Interrupted
  uploads must release incomplete allocations without changing the displayed face.
- Start with one dense float32 weight snapshot per morph draw. Append a bank
  handle, target count and weights to a new explicitly negotiated draw variant.
  Illustrative API names are `CreateMorphBank`, `UploadMorphBank`,
  `DrawObjectMorphWeights`, and `DestroyMorphBank`; they do not exist today.
- Every draw includes **all weights**, including zeros. No persistent sticky
  weights, incremental accumulation or dependency on the previous frame. Attach
  the snapshot to the accepted frame so one executing frame and one waiting
  frame cannot overwrite each other's state. Different draws of the same mesh
  may have different weights; keep deformation results per draw or safe to recompute.
- Reject combining a morph-weight draw with inline vertex override. In this
  mode reject mutation of a bank's bound base mesh until the bank is destroyed
  and rebuilt after its referencing work drains.
- Advertise morph blending and limits: bank/target/entry counts, bytes, supported
  formats, weights per draw, and blend-work budget. The existing capability
  record is tightly packed: use an explicit morph-capability query record rather
  than silently extending its 46-byte payload.
- For this first implementation, use a new matched protocol version (proposed
  10) on host and GPU, updating both graphics and runtime versions and boot
  identity/image compatibility checks. Reserve opcode/flag/resource-class values
  after auditing the actual firmware. Do not claim compatibility with protocol 9.
  Old firmware must fail negotiation clearly; a protocol-9 host-vertex path can
  remain a separately selected build/backend.

GPU deformation for each draw:

```text
workingVertices = immutableBaseVertices
for target in original bank order:
    if weight[target] > 0:
        for entry in original target entry order:
            workingVertices[entry.index] += float32(entry.halfDelta) * weight[target]
apply object transform once
apply camera transform, clip, rasterize, effects, output
```

Match `JsonNukudeFace::Update` / `MorphCompact::MorphObject3D`: fresh base each
frame, float32 accumulation, positive-weight gate, stable addition order, and
the same binary16 conversion as `HalfFloat.h`. Preserve weights greater than
one; do not introduce an implicit `[0,1]` clamp. Reject nonfinite asset values
or weights explicitly. Test negative/zero behavior, repeated indices, extremes
and half conversion boundaries. Do not sum different targets or reorder their
additions during the first parity implementation. Cross-architecture FMA and
rounding may differ; specify arithmetic and measure geometry/image differences.

## Required changes by component

| Component | Files or subsystem | Required change |
|---|---|---|
| Asset packer | New `tools/pack_gpu_face.py` and package specification | Pack base mesh, morph directory/deltas, name-to-ID metadata and fingerprint. Validate counts, finite numbers, indices and offsets. Support bounded streaming, retain original ordering and existing half conversion. |
| Asset selection | `JsonDrivenProtogenAnimation::CollectUsedMorphNames` | Preserve always-needed targets, aliases and configuration references. Build a stable manifest for the uploaded subset; do not infer IDs from independent CPU/GPU enumeration. |
| Asset delivery | `FaceModelUpdater`, `RemoteFileSync`, LittleFS | Obtain/cache the binary package alongside metadata through the existing authenticated asset workflow. Keep authoritative bytes on flash for reboot/recovery. Version face/config together and fail clearly on mismatched banks. |
| Host face representation | `src/Morph/JsonNukudeFace.h` or new `GpuFaceState.h` | GPU mode loads names/IDs and a fixed float32 weight table, transform/material references and GPU handles. Avoid creating `MorphCompact` buffers, `TriangleGroup` or `Object3D` geometry merely to expose weights. Keep the local implementation available. |
| Animation bindings | `src/Animation/JsonDrivenProtogenAnimation.h` | Bind `eEA` and blink to stable weight addresses. Preserve reset, voice/expression precedence, flips, aliases and blink-after-interpolator order. Separate pose/material state from mesh access; skip CPU `UpdateFace` and `UpdateTransform` in GPU mode. |
| Frame publication | `src/main.cpp`, scene publication/backend seam | Replace vertex copies with an immutable weights + pose + material snapshot. Publish only after animation/blink finish. Prevent animation-worker writes while encoding/DMA owns the submitted bytes. |
| Render adapter | New `src/Controllers/ProtoGLController.h` or equivalent backend | Upload/reconstruct banks, encode weights-based draws and handle errors/credits/fences. Submit morph-local base geometry plus the real object transform. Bypass local rasterization, screenspace effects and HUB75 packing. |
| Shared wire definitions | ProtoGL `PglTypes.h`, `PglOpcodes.h`, `PglRenderCommands.h`, `PglRuntimeProtocol.h` | Define bank handles, resource streams, weights draw, strict decoding and capability query. Specify endian/count/length rules, version negotiation and clear unsupported/capacity errors. |
| Host support library | ProtoGL `PglEncoder.h`, `PglDevice.h`, `PglLink.h`, `PglParser.h` | Add bounded upload/draw/destroy/query helpers, bank handle tracking, capacity checks and recovery reconstruction. Keep encoder overflow/invalid-command behavior and DMA lifetime guarantees. |
| GPU resources/parser | RP2350 resource arena, stream receiver, mesh table and command parser; paths pending source | Store banks with generation/mesh checks; validate the complete resource before commit and the complete frame before execution. Protect referenced resources across queued/executing work. Reject out-of-range indices/counts and byte-length arithmetic overflow. |
| GPU vertex stage | RP2350 deformation/transform scheduler; paths pending source | Blend into preallocated float32 scratch, then transform once. Reuse proven scratch only with explicit ownership. Account for active entries in admission; no per-frame allocation. Keep panel refresh independent of long blend work. |
| GPU telemetry | Memory and metrics records, morph-capability query | Report resident bank bytes, scratch/staging peaks, active targets/entries, blend time, render time, frame latency, capacity failures and display underruns. |
| Host memory cleanup | Camera/PixelGroup allocations, `Object3D` buffering, S3 controller/debug/HUD paths | Do not construct local render allocations in GPU builds. Size host frame buffers for weight-only frames after measuring every enabled command; preserve media/asset limits and preview policy. |
| Regression/release | Both repositories' host tests, GPU tests and device soak; build manifests/docs | Verify numerical/visual parity, malformed assets, stale handles, reset/recovery, two-frame isolation and resource lifetime. Pin matched CPU/GPU revisions and test interrupted/mismatched upgrades. |

No change to BLE/web/Android expression commands is required for this first
offload: those commands still select CPU animation state. Only the CPU-to-GPU
graphics protocol changes. Do not move networking, FFT/audio acquisition or
expression timing into this extension.

Weights exposed to `eEA`/blink are pointers. Allocate the final table before
`AutoLinkMorphs` and `LinkParameters`, keep addresses stable, and never resize
it afterward. A face/config reload must quiesce work, rebind interpolators and
blink tracks, and atomically replace the matching metadata/GPU resources.

## Startup, memory and recovery

Prefer generating the binary package off-device (asset pipeline or host tool).
The main CPU reads a small manifest and streams bank bytes from LittleFS with
bounded buffers; it never needs a resident JSON DOM or all delta arrays. A
transitional JSON-to-binary startup converter can prove the GPU blend but still
has the old parse peak. Uploading/freeing the existing face's buffers afterward
reduces steady-state use only; do not confuse the two improvements.

Weight-only frame buffers can be smaller than ProtoGL's current 16 KB default.
Derive capacity from maximum draws, weights, materials, cameras and effects,
then test overflow rejection; media texture updates and startup resources use
the separate bounded asset path. Do not shrink buffers based on one solid-face
demo and assume all configurations fit.

After a GPU reset/session change, invalidate all handles, stream the authoritative
mesh/bank assets again, then send a complete current weight snapshot. Continue
or pause CPU expression time according to an explicit recovery policy; never
apply an old queued snapshot to a new bank. Replacing/destroying resources waits
for referencing GPU work, not merely host DMA completion. Repeated frames/retries
must not apply morph deltas cumulatively.

## Implementation order and acceptance

1. Obtain the actual GPU source and agree on the matched protocol extension.
2. Implement offline packing and a CPU reference blender consuming that same
   package. Check the entire current face and larger/malformed assets.
3. Implement GPU resource upload and blending for a solid, fixed-camera face;
   keep the existing host-vertex backend for comparison.
4. Bind the CPU metadata-only face and stable weights; verify expressions,
   aliases, visemes, blink, boop, flips, zero/negative/greater-than-one weights,
   reset, camera/object offsets and negative scale.
5. Exercise queued frames with differing weights, repeated/rejected batches,
   partial uploads, memory exhaustion, resource destruction and full recovery.
6. Remove local renderer allocations in GPU builds and switch startup to binary
   streaming. Measure host peak/steady internal SRAM and PSRAM, GPU peak/free
   memory, wire traffic, blend time and actual presented FPS under mixed-workload
   soak. Keep higher FPS conditional on those measurements.

No new firmware code or tests were added for this plan. Asset arithmetic and
the existing host/library semantics were inspected; physical GPU execution,
allocation headroom and achievable frame rate remain unverified.
