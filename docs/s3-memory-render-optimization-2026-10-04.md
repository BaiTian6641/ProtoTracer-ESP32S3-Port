# ESP32-S3 memory and rendering optimization — 2026-10-04

## Device and measurement

Device: ESP32-S3-WROOM N8R8, revision 0.2, COM5, 240 MHz, 8 MB PSRAM.
Both baseline and optimized measurements use `esp32s3-PROFILE`, the same
pioarduino toolchain, device assets, 8-bit HUB75 depth, and DMA double buffering.
The face has 523 triangles. Network startup, the HUB75 reinitialization pause,
and pre-pipeline frames are excluded from the steady-state results.

The baseline was rebuilt from the pre-optimization sources with only `[FRAME]`
telemetry added. Its binary and ELF were initially saved in
`.pio/build/optimization-baseline/` (later cleared with generated build caches).
The measurements below were recorded before that cleanup. Local build and serial logs are ignored
`build_*.log` artifacts; they are not release assets.

| Metric | Baseline | First batch |
| --- | ---: | ---: |
| Timed-loop throughput, median | 24.07 FPS | 42.47 FPS |
| Mean frame work per reporting window, median | 41.54 ms | 23.55 ms |
| Sampled render time, median | 8.01 ms | 3.55 ms |
| Internal free heap | 38,016 B | 65,852 B |
| Largest internal free block | 22,516 B | 31,732 B |
| ELF `.dram0.data + .dram0.bss` | 86,504 B | 82,424 B |

Final results cover 33 steady-state reporting windows in a 90-second serial
observation. Render timing is a sampled frame, whereas FPS and frame work are
aggregated over each reporting window. `[FRAME]` includes menu work, render,
HUB75 conversion, and the existing delay; it excludes the trailing yield and
telemetry printing. These are measured loop-processing rates, not a separate
measurement of panel scan refresh or input-to-photon latency.

The optimized loop is approximately 76% faster on this measured baseline, and
rendering takes approximately 56% less time. This baseline differs from the
user's previously observed approximately 40 FPS; do not describe the change as
a measured improvement from that earlier setup.

## Changes

- Added a procedural, positive-spacing row-major `PixelGroup` constructor.
  The active S3 controller uses 64 columns, 32 rows, origin (0,0), spacing (3,3).
  It stores neither coordinates nor neighbor tables. Coordinates and all four
  neighbors match the original P3HUB75 layout. Existing arbitrary layouts retain
  their neighbor tables; existing rectangular layouts no longer allocate unused
  neighbor tables.
- Kept the primary RGB buffer internal-first. Moved the effect scratch RGB
  buffer to PSRAM-first with an internal fallback. The grid removes 16 KiB of
  neighbor payload; scratch placement moves up to 6 KiB out of internal memory.
- Replaced the HUD static array with an explicit, persistent 4 KiB PSRAM
  allocation at controller initialization. Preview conversion and display are
  guarded if allocation fails. ELF confirms `gHudBuffer` is now a 4-byte pointer,
  rather than a 4096-byte internal BSS symbol.
- Cached direct-rasterizer pixel rays until camera rotation or XY scale changes.
  Cache reallocation and legacy rendering invalidate the direct cache. Pixel
  coordinates are immutable for the current PixelGroup API.
- Prepared quaternion normalization and invariant rotation coefficients once
  per object projection. Kept the original RotateVector arithmetic, including
  its original-quaternion coefficients, instead of changing to a differently
  rounded matrix implementation.
- Deferred barycentric division until after off-screen bounding-box rejection.
- Decoupled auxiliary display refresh from the face loop: direct displays use
  `MENU_DISPLAY_INTERVAL_MS=100` by default; relay-MCU displays retain at least
  200 ms. Gesture reads and BLE servicing still run on every menu update.
- Removed the discarded `PeakDetection::GetStdDev(i-lag+1, i, ...)` call. It
  used the sample index as a window length, could read beyond the input, and
  its results were immediately overwritten. The existing fixed-baseline peak
  detector behavior is retained; this is not a DSP algorithm rewrite.
- Added frame/menu/HUB75 timing to PRINTINFO builds. Repaired two existing
  Windows PowerShell issues in the host comparison script.

Effective internal heap recovery includes allocator overhead and placement
effects, so it is not identical to the sum of buffer payloads. Only about
4 KiB of the gain appears in static ELF sections; the rest is runtime memory.

## Verification and installed firmware

- `esp32s3-PROFILE`: build passed.
- `esp32s3-RELEASE`: build passed.
- `host-tests/build_and_compare.cmd`: passed. Legacy and direct output match
  byte-for-byte for the synthetic scene (6144 bytes, FNV-1a `0xBE8B37A9`).
- Host checks cover all 2048 grid coordinates and neighbors against the
  original table, repeated cached renders, scale changes versus a fresh camera,
  and 128 successive synthetic audio frames against fixed-baseline decisions.
- Temporary device `RASTER_VERIFY_AB=1`: 600 frames, zero frames with nonzero
  pixel differences. Both paths render the same real animated scene.
- Installed final PROFILE firmware on COM5 with `RASTER_VERIFY_AB=0`; flash
  hashes verified. LittleFS and user settings were not rewritten.
- Final 90-second observation: no captured panic, heap corruption, watchdog,
  brownout, or fatal messages; `micDrops=0`. Internal free memory stayed around
  65.9 KB. This is short-run validation, not an hours-long soak test.

No visual hardware inspection or recorded-audio viseme validation was performed.
The auxiliary preview now refreshes more slowly by design. Network/OTA startup
ordering, token authentication, model precision, and HUB75 depth are unchanged.

## Remaining performance work after the first batch

Final HUB75 conversion averages approximately 5.50 ms, exceeding the sampled
3.55 ms rasterization cost. Auxiliary menu work averages approximately 6.66 ms,
and window maxima remain approximately 38 ms because display pushes still block.
Animation and rendering remain sequential; only menu work overlaps animation.

The next useful experiments are a fixed-layout HUB75 packing kernel, moving
auxiliary display transactions off the critical frame path with explicit
ownership, expression command-slot preprocessing, and a complete immutable
frame snapshot for true animation/render overlap. FFT and animation task
competition should be measured before changing affinity or priorities. Larger
PSRAM-first migrations should be judged against hot-data bandwidth and latency,
not only free-memory totals.

## Second batch: HUB75 output and serial snapshots

The next iteration retains 8-bit output, CIE correction, mirrored panel layout,
and DMA double buffering. Changes:

- Correct each RGB input once per physical row pair, rather than once per plane.
- Combine RGB1 and RGB2 into one masked DMA-word update; preserve every non-RGB
  bit, including address, LAT, and OE.
- Spread each corrected 8-bit channel into a packed 24-bit RGB plane stream,
  using a 1024-byte constexpr lookup table in **`.flash.rodata`**. The inner loop
  extracts the two RGB triplets with two loads/shifts, rather than six.
- Compare against the actual current back buffer (from two flips ago). Write
  only changed words and mark/flush only changed physical rows. Existing dirty
  marks from other drawing operations remain valid. Color depths above 8 retain
  the wider channel representation; unsupported layouts use the original path.
- Keep the original driver's method body as fallback and temporary verification
  oracle. `patch_hub75.py` reproducibly patches the pinned generated dependency
  in each S3 environment, refuses an unexpected legacy method hash, and copies
  the project-owned kernel into that build's dependency directory. No edits to
  the external library checkout are needed. The debug row accessor is inserted
  at a checked public-method anchor; it changes neither object size nor buffers.

Intermediate 90-second device runs, each with the same assets and 16 checksummed
engine/audio snapshots:

| Output kernel | Median FPS | Median HUB75 work |
| --- | ---: | ---: |
| First batch | 42.47 | 5.50 ms |
| Combined halves, one CIE conversion | 43.815 | 4.94 ms |
| Changed-word/row tracking | 44.47 | 4.33 ms |
| Compact plane stream + changed rows | 45.98 | 3.18 ms |

The final compact kernel was compared against the old kernel on COM5 for **64
animated frames**, across all DMA words, with zero differences. Verification
restores the initial back-buffer contents before running the new kernel, so
changed-word comparisons start from the same data. Each intermediate variant
also passed 64 full-buffer comparisons. The normal build omits verification
allocation, duplicate rendering, and logs; this was checked in the final ELF.

`host-tests/build_hub75.cmd` passes 1,408 cases using the actual production
kernel, including all DMA control bits, 2..12-bit data paths, channel ranges,
random initial words, and both FIFO coordinate orders. It additionally checks
no writeback for an unchanged frame, the correct dirty row for a changed source
pixel, and retention of a pre-existing dirty mark. The serial-header tests pass
immutable snapshot checks, four-record TX bounds, timeout/cancel cleanup,
invalid ranges/long lines, allocation failure, and zero idle allocations.

The serial interface is documented in [serial-debug.md](serial-debug.md). It
captures an immutable PSRAM snapshot at the completed-frame boundary, then
streams bounded chunks without holding up subsequent animation work. Targets
cover engine RGB, effects, HUD, render vertices/indices, published audio arrays,
and front/back DMA rows. Heap integrity and task stack headroom can be queried.
The largest request is 64 KiB and has no internal-heap fallback.

Final static DRAM is **82,832 bytes**, 408 bytes above the first batch. The new
lookup table stays in flash. The packing scratch is a roughly 1.5 KiB stack
union, rather than persistent internal heap; device checks report at least
2,784 bytes of remaining loop-task stack so far. Snapshot payload allocation is
PSRAM-only and is released on completion, cancellation, or timeout.

PROFILE and RELEASE builds both pass. The installed COM5 image is PROFILE with
the temporary packer verification disabled; application flash hashes were
verified and LittleFS/user settings were not rewritten.

### Final continuous-run gate

The final installed image ran for **600.1 seconds**, including **160 complete
checksummed snapshots** (505,696 payload bytes) across all ten target types and
one additional 14-byte partial-range snapshot. Final results:

| Metric | Previous batch | This batch |
| --- | ---: | ---: |
| Median timed-loop throughput | 42.47 FPS | **45.85 FPS** |
| Median HUB75 output work | 5.50 ms | **3.14 ms** |
| Internal free heap | 65,852 B | **65,208..65,248 B** |
| Largest internal free block | 31,732 B | **31,732 B** |
| Static DRAM | 82,424 B | **82,832 B** |

This is approximately **8% higher loop throughput** and **43% less HUB75 output
work** than the previous batch. All 287 reporting windows are retained in the
soak summary; microphone drops remain zero. Internal free heap has no sustained
downward trend over this observation. The final integrity check reports
`heap=ok`, and no panic, heap corruption, watchdog, brownout, or fatal messages
were captured. Loop/animation/microphone task stack headroom ends at
2,784/5,640/5,268 bytes respectively.

The 16 RGB snapshots have 16 distinct hashes, showing ongoing animation across
exports. Float values are finite, face indices fit the exported vertex count,
and front/back DMA RGB words have the expected panel mirroring. Invalid ranges,
indices, extra arguments, and oversized command lines return errors. Generated
logs, summary JSON, and a PPM snapshot are under the ignored
`.pio/build/debug-final-soak/`; these are disposable build artifacts, so copy
them elsewhere before clearing build caches.

`[FRAME]` measures work through Display, so snapshot copy/checksum/transmit work
after Display is excluded from that particular timing statistic. Debug requests
can extend a frame; steady rendering continues while chunks stream. Normal
operation has only the idle poll cost. This is a ten-minute stability gate, not
an hours-long endurance result, and no physical panel/recorded-speech inspection
was performed. Auxiliary-screen I2C latency and animation/FFT CPU contention
remain the main candidates for the next iteration.
