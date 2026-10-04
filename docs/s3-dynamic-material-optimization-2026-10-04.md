# ESP32-S3 dynamic material and memory optimization

## Scope and implementation

The device is an ESP32-S3-WROOM-N8R8 on COM5. Only the application partition
at `0x10000` was flashed; LittleFS, user configuration and downloaded face /
animation assets were retained. These changes follow the registration display
work and do not change its foreground ownership or OLED protection policy.

`SpiralMaterial::GetRGB()` previously constructed an Euler rotation and
normalized its quaternion for every shaded pixel. `GradientMaterial` repeated
the same work. `PreparedMaterialRotation` prepares those coefficients in
`SetRotationAngle()` and preserves the exact arithmetic of
`Quaternion::RotateVector()`, including its use of original coefficients.
Angle-zero offset behavior, radius, `atan2`, `powf`, color buckets and gradient
interpolation are retained. No approximate lookup table or reduced color depth
is introduced. Rotation preparation allocates no heap.

Additional material fixes:

- `RainbowSpiral::HueShift()` now updates the palette actually used by the
  spiral, using an absolute rotation of its original palette. Previously it
  cumulatively modified a separate array which the shader had already copied.
- `SpiralMaterial` owns its two palettes through value-safe vectors. Copies
  have independent storage and destruction frees it. Allocation occurs when
  constructing/copying the material, never during normal shading or updates.
- JSON-created gradients copy their base palette into owned storage. Their
  former base pointer referred to a local array in `CreateGradientMaterial()`.
  Stock gradients retain their external-palette `UpdateRGB()` behavior, and
  owned gradients remain safe after copy/assignment.
- `Material` now has a virtual destructor, matching ownership through
  `std::unique_ptr<Material>`. The gradient's previously unnamed fourth
  constructor parameter also correctly sets stepped interpolation.

Memory and audio changes:

- The 128 FFT filter histories (5,632 payload bytes) move from internal static
  storage into explicitly allocated PSRAM and retain their constructors.
  Initialization checks all audio buffers and releases partial allocations
  if allocation fails. FFT working storage remains internal.
- Spectrum bounce state is allocated on first use as one PSRAM block rather
  than 128 small allocations during global initialization. Its 9,216-byte
  payload and per-allocation overhead no longer occupy internal heap when
  the effect is unused. Failed allocation falls back to raw spectrum values;
  uninitialized spectrum input returns black safely.
- ADC preprocessing retains noise subtraction, but removes redundant
  magnitude/phase reconstruction for nonnegative real 12-bit samples.
  Bin averaging iterates only included bins, preserving summation order,
  exclusion of bin zero and the original divisor.
- Task stack sizes, sampling architecture and render scheduling are unchanged.

## Measurements

Live shader overrides were used on the same downloaded 495-vertex / 523-triangle
face. They replace the face shader only during rendering and restore the
configured material before the next animation update. They bypass layer blending
and do not reproduce every expression/transition. FPS depends on geometry,
expression, OLED state and material coverage; these numbers are scenario
measurements, not universal performance guarantees.

The initial cold normal run included BLE/startup transitions and is excluded.
The initial rotation/cache-stage awake measurements had substantial OLED work (menu about 6–8 ms
per frame window). The following uses actual debug-frame deltas over wall time:

| Metric | Original | Optimized |
| --- | ---: | ---: |
| RainbowSpiral, OLED awake | 32.89 FPS / 20.16 s | 41.49 FPS / 30.08 s |
| Spiral render time, sampled median | 11.24 ms | 5.14 ms |
| Default material, OLED awake | 45.28 FPS | 45.86 FPS |
| Runtime internal free heap | 64,952 B | 81,936 B |
| Largest internal free block | 31,732 B | 40,948 B |

The spiral awake frame rate improves about 26%; its sampled render time falls
about 54%. Default frame rate changes little. Audio throughput in these windows
increases from about 27.4 to 29.6 processed blocks/second with zero reported
sampler drops. This is throughput, not a measurement of continuous ADC duty.

With the OLED idle, the original spiral measured 38.83 FPS, and the original
default measured 54.34 FPS. After the USB changes described below, a 30-second
wall-clock test measured **47.23 FPS spiral** and **53.60 FPS default** with the
OLED idle. Spiral render sampled median was 6.05 ms. Internal free heap was
82,108 B, largest block 40,948 B. The spiral improvement versus the original
idle run is about 22%. Awake and idle results must not be mixed when reporting
improvements; code layout, scene phase and coverage also affect measurements.

On the ESP32-S3 itself, an independent original shader and the cached shader
were evaluated over eight rotations and a full 64×32 coordinate grid:

```text
[MATERIAL_VERIFY] samples=16384 diffs=0 legacyUs=238144 cachedUs=117649 checksum=12531720
```

This is about 2.02× faster shader evaluation, independent of frame/animation
overhead. The check covers this grid and compiler, not every possible float.
Host tests additionally cover 737,280 random 3D positions over 180 rotations.

## Verification and tooling

`host-tests/build_materials.cmd` checks exact rotated coordinate bits,
spiral/linear/radial gradient RGB, owned palette lifetime and copying, external
palette updates, absolute hue shifts, virtual destruction, lazy PSRAM bounce
allocation, 100-frame bounce equivalence, destruction and allocation failure.
The existing direct-versus-legacy renderer harness also passes, with identical
6,144-byte frame buffers. Serial tests cover valid/invalid/disabled PROFILE
controls and rejection during snapshot transfers.

An exhaustive isolated arithmetic probe passes 40,960 ADC/noise pairs and
136,764 bin ranges with bit-identical results. This supports the arithmetic
change; it is not a substitute for microphone/listening validation.

PROFILE firmware supports these nonpersistent diagnostics:

```powershell
python tools/serial_debug.py --port COM5 profile verify
python tools/profile_materials.py --seconds 30 --output build/materials/profile.json
python tools/soak_material.py --material spiral --seconds 600 --dumps 160 --directory build/materials/soak
python tools/serial_debug.py --port COM5 profile normal
host-tests\build_materials.cmd
host-tests\build_and_compare.cmd
```

`profile normal` restores the configured shader; reboot also clears overrides.
`profile verify` temporarily occupies one frame for the diagnostic. Benchmark
and soak clients restore normal in `finally`. PROFILE controls and the golden
shader are excluded from RELEASE; the read-only snapshot commands remain.
Benchmark logs and binaries are under ignored `build/` directories.

## USB host backpressure

The first 600-second material soak did **not** pass: after about 470 seconds
the CDC telemetry stopped, and final commands / restoration timed out. Received
snapshots had passed, but missing telemetry cannot establish continued rendering
or heap health. The user separately reported intermittent stuttering only when
USB was attached to a host, even without a serial monitor; USB power alone was
fine, and resetting restored behavior.

The selected local Arduino `HWCDC` implementation uses a default 100-ms TX
timeout and up to 20 consecutive send retries under host backpressure. A host
USB connection can remain active without any application reading CDC. Application
logging must not wait for that reader. The firmware now sets the CDC TX timeout
to zero. `patch_usb_cdc.py` reproducibly patches the selected framework's
ring-send failure branch to return a short write immediately for zero timeout,
instead of continuing a retry loop. It retains the other installed framework
patches and rejects an unknown send-guard shape; framework updates require
reviewing this patch. It does not affect nonzero-timeout callers.

Normal frame telemetry uses a bounded stack buffer and sends a complete line
only when there is enough available space. Ordinary logs may be dropped under
backpressure. SerialDebug keeps unsent protocol bytes and retains its timeout /
cancel behavior. Periodic animation-worker boop logging is disabled by default
(`-DBOOP_DEBUG_LOG=1` restores it). The FPS budget now includes debug and logging
work, which the previous counter omitted. No larger serial buffer is allocated.

Hardware stress results on the final USB build:

| USB / host behavior | Duration | Wall FPS |
| --- | ---: | ---: |
| CDC continuously read | 45.06 s | 44.69 |
| Port closed, USB still attached to host | 45.03 s | 44.90 |
| CDC continuously read again | 45.06 s | 44.72 |
| 20 stats commands/second, responses deliberately unread | 46.77 s including resync | 44.67 |
| CDC read after pressure | 45.03 s | 49.50 |
| 30 port close/reopen cycles, no intentional reset | 15.44 s | 53.70 |

The final two phases cross the OLED idle transition, explaining their higher
FPS. All phases had increasing frame counters, valid final heap checks, stable
82,108-byte internal free heap and zero microphone drops. This tests specific
host-pressure/reconnect scenarios; it cannot prove every intermittent USB state
issue is eliminated.

The final spiral soak passed **600.3 seconds / 160 snapshots / 505,696 bytes**.
All 16 RGB snapshots had distinct hashes; published float arrays were finite,
face indices were in bounds and mirrored DMA color bits matched. Internal free
heap stayed exactly **82,108 B**, largest block was **40,948 B**, reported minimum
was 81,908 B, and microphone drops stayed zero. There were no decoded faults or
transport-progress timeouts. Final heap integrity passed. Available task stack
high-water values were loop 2,752 B, animation 6,800 B, microphone 5,260 B;
stack capacities are unchanged. The configured material was restored at exit.
The 298 telemetry windows mix awake/idle OLED and snapshot load: their 43.62-FPS
median is a stability workload result, not a like-for-like material benchmark.

A final target color check on the USB build also passed:

```text
[MATERIAL_VERIFY] samples=16384 diffs=0 legacyUs=189873 cachedUs=106014 checksum=12531720
@DBG CHECK heap=ok
```

That final synthetic shader timing is about 1.79× faster. The earlier 2.02×
result is a separate build/stage and should not be substituted for this result.
The final runtime internal heap improvement over the original warm run is
17,156 B (about 16.75 KiB), without reducing task stacks.

```powershell
python tools/usb_render_stress.py --seconds 45 --cycles 30 --output build/materials/usb.json
python host-tests/usb_cdc_patch.py C:/Users/weyst/.platformio/packages/framework-arduinoespressif32/cores/esp32/HWCDC.cpp
```

The soak monitor now fails if neither frame telemetry nor main-loop debug
progress arrives for 15 seconds, rather than reporting an empty fault list while
the link is silent. Both final PROFILE and RELEASE builds pass; RELEASE excludes
the golden shader and live override. The final tested PROFILE application SHA256
is `f526aa59cce6717dc6b78bb939a83a6eb0fb470de818061e14ddfdb46c6ff56d`.

## Remaining work

The dominant remaining common costs are animation updates (roughly 10 ms in
this scenario), OLED refresh while awake, geometry/raster work and HUB75
packing/display. Spiral still computes its nonlinear radius/polar functions
per pixel. Further lookup or coordinate caching requires a clearly defined
error budget and invalidation for changing geometry, rotation and offsets.
OLED field/region updates and animation lookup/capacity improvements are
separate next candidates. OLED wake detection also currently compares the
cached proximity sample against the previous sample (`proximity`) rather than
the actual boop threshold/hysteresis; below-threshold fluctuations can create
wake edges. Review that separately before attributing every wake to user input.
Cross-core frame overlap needs immutable material
and effect state before it can be considered safe.
