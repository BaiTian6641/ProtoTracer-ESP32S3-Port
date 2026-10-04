# ESP32-S3 next optimization review (2026-10-04)

This is an assessment, not a firmware change. COM5 was sampled without resetting,
flashing, changing controls, or touching persisted configuration. Only public
render/audio debug targets and timing/heap records were read. Analysis probes
and raw observations are under the ignored `build/optimization-review/` directory.

## Current baseline

A 60.079-second window advanced the debug frame counter by 3,250 frames:
**54.10 FPS by elapsed wall time**. Thirty PRINTINFO windows reported a median
54.325 FPS / 18.41 ms of timed loop work. These metrics differ because PRINTINFO
excludes work after Display, including debug servicing and its own log writes.

| Observation | Value |
| --- | ---: |
| Internal free heap, before and after | 65,200 B |
| Largest internal free block | 31,732 B |
| Minimum-ever internal free heap | 65,024 B |
| Free PSRAM, before and after | 7,711,812 B |
| Heap integrity, before and after | OK |
| Microphone drops, before and after | 0 |
| Animation duration, median of point samples | 9.295 ms |
| Render duration including effects, median of point samples | 4.175 ms |
| Mean menu duration, median reporting window | 1.35 ms |
| Mean HUB75 Display duration, median reporting window | 3.10 ms |
| Per-window maximum frame duration, median | 19.98 ms |
| Face mesh | 495 vertices, 523 triangles |

Animation/render samples are point observations printed every two seconds;
menu/Display/frame metrics are window averages. They are not synchronized CPU
profiles and must not be added as exact measured stage costs. This window had
low menu overhead, consistent with an idle OLED; its sleep state was not queried.
It is not directly comparable to the preceding 50-52 FPS observation with
display activity. The new mesh also differs from earlier optimization fixtures.

Typical RAST records contained one enabled object, 33-35 submitted triangles,
488-490 bbox rejects, and approximately 252-256 shaded pixels. Thus shader
shortcuts may help complex materials, but this specific pose is dominated more
by animation, transforms/triangle traversal, and output than by filling the whole
2048-pixel panel. Keep the same mesh, expression, microphone input, OLED state,
and BLE activity for comparisons. This is a short observation, not an endurance
or all-expressions stability test.

## Internal SRAM: specific candidates

ELF allocated DRAM sections total 82,880 B: `.dram0.data` 24,776 B and
`.dram0.bss` 58,104 B. Debug type sizes below are from this S3 ELF, not host
`sizeof` results with 64-bit pointers.

| Candidate | Current payload | Proposed treatment | Caveat |
| --- | ---: | --- | --- |
| SpectrumAnalyzer bounce objects | 128 x 72 = 9,216 B, plus allocation overhead | One contiguous PSRAM allocation, or initialize on first actual use | Small-object internal placement is inferred from allocator policy; addresses are not currently exported |
| Microphone `fftFilters` | 5,632 B in internal BSS | PSRAM allocation once, with proper constructors | Compare FFT throughput and cache contention |
| Animation task stack | 8,192 B; 5,520 B unused at high-water mark | Trial 6,144 B | Exercise every expression and failure/debug paths |
| Microphone task stack | 6,144 B; 5,260 B unused at high-water mark | Trial 4,096 B | Include initialization, fallback and audio peaks |
| FastTrig table | 1,024 B in internal BSS | Generated read-only float table in flash | Match current table rounding or explicitly validate differences |
| EasyEaseAnimator<180> | 12,068 B inside the internal animation object | Allocate storage once for the actual linked parameter count, retaining a hard 180 limit | Active count must be measured; preserve all interpolation state and method changes |

SpectrumAnalyzer currently creates all 128 BouncePhysics objects in its
constructor because the animation declares it with `bounce=true`, even when the
background is disabled. Under the bundled allocator's 4,096-byte preference
threshold, 72-byte allocations normally consume internal heap. Moving this
block also removes 128 independent allocations. If allocation is deferred, the
feature must retain its current behavior when selected, and its pointer table
must be initialized to null before changing destruction/ownership logic.

EasyEaseAnimator's 180-slot allocation contains 5,760 B of springs and 2,160 B
of ramp filters, plus pointers, targets, bases, previous values and dictionaries.
Much of this is unused when fewer parameters are linked. Its storage is roughly
67 B per slot plus header/alignment. For example, 60 slots would save about
8,040 B of payload; 60 is an illustration, not the device's measured count.
Do not simply lower the template limit or remove spring state for slots whose
interpolation method can later change. Keep frequently touched state internal
where useful, and use one bounded arena rather than many per-parameter objects.

The first five candidates sum to about **19,968 B of payload**, before allocator
effects. This is an opportunity estimate, not a measured recovery. Retain the
8,192-byte loop stack: only 2,752 B remains after its observed worst usage, and
the HUB75 packer already uses substantial stack scratch. Never move stacks
performing flash operations to PSRAM.

Camera is 37,484 B, including 350 Triangle2D objects (92 B each) and 128 Node
objects (40 B each): **37,320 B of legacy arena storage**. It is dynamically
allocated and exceeds the allocator's internal-preference threshold, so this
must not be described as another 37 KB of recoverable internal SRAM. Lazy
legacy arenas are useful PSRAM cleanup, especially while preserving A/B and
unsupported-grid fallback, but are lower priority with 7.7 MB PSRAM free.

Three Triangle3D arrays for a 523-triangle face cost approximately 100,416 B
at 64 B/triangle. A compact indexed rendering representation could remove much
of this PSRAM duplication. It is an architectural change, not a quick internal
SRAM saving. Preserve legacy material/UV/normal behavior and snapshot ownership.

## CPU: start with behavior-preserving audio changes

In `MicrophoneFourier_MAX9814.h`, every new raw sample is nonnegative 12-bit ADC
data and the imaginary input is explicitly zero. The preprocessing then computes
sqrt, atan2, cos and sin for each of 256 samples. In that domain it can be
expressed as:

```cpp
float real = float(rawSamples[i]) - noiseMagnitude[i];
if (real < 0.0f) real = 0.0f;
SampleArray[2 * i] = real;
SampleArray[2 * i + 1] = 0.0f;
```

An isolated MSVC probe compared 40,960 ADC/noise pairs, covering every value
0..4095 and ten finite nonnegative noise levels: both output floats were
bit-identical. This is domain-specific and does not authorize changing the
noise estimate, sample normalization, FFT ordering or viseme algorithm. Verify
on the S3 with captured input/FFT frames and recorded speech before shipping.
The expected benefit is less competition with animation on core 0; task CPU
time has not yet been measured separately.

`AverageMagnitude` currently checks all 255 usable indices for each of 127
adjacent-bin averages. Restrict iteration to `[max(1, binL), min(255, binH)]`
while retaining the original denominator. For the current adjacent bins, this
reduces source-level checks from 32,385 to 253 included additions per block.
The first range excludes bin zero but still divides by two: preserve this.
An isolated probe passed 136,764 ranges across four synthetic signed float
spectra with bit-identical averages, including bounds outside the valid indices.
Inspect/measure target code to establish the actual cycle reduction.

## Animation: compile configuration into runtime handles

Morph names in anim_parameters are already resolved to IDs, but
`AddParameterFrame(id)` and `SetInterpolationMethod(id)` still linearly scan
linked parameter slots. Every frame also performs expression-name lookup,
material/effect lookup, and repeats persistent expression settings.

At initialization, resolve expression indices, material/effect pointers, colors,
interpolation slot handles, and parameter-write handles. Apply persistent state
when the expression changes, but continue writing its target parameters every
frame: EasyEaseAnimator resets parameterFrame to its basis during Update.
Also preserve writes/ordering from reset_state, voice input and BlinkTrack.
Do not skip an entire unchanged expression or mark interpolating spring/morph
state idle solely because its target value is unchanged.

This reduces string traversal and unnecessary work without sacrificing half
storage precision. Measure reset/expression application, voice detection, morph
application and transforms separately before deciding to decode morph deltas
into larger float32 PSRAM caches. Larger decoded caches can lose the intended
benefit through bandwidth/cache pressure.

## Foreground display: reduce dirty regions before adding tasks

ESPMenu redraws fixed text, icons, border and preview every 100 ms. M5GFX's
OLED driver marks the supplied drawing rectangle dirty before checking pixel
values, so redrawing unchanged content expands the transfer region. Its display
method already supports dirty rectangles/page ranges; this is not a missing
driver capability.

Redraw labels/icons only on state change or protection-induced shift/wake.
Update only the preview rectangle during ordinary preview frames, and avoid
dirtying it when its monochrome result is unchanged. OLED protection still
forces redraw when it moves content, wakes the panel, or changes registration
mode. Do not leave traces of old shifted pixels or disturb the persistent QR.

At 400 kHz, 1024 data bytes alone take approximately 23 ms of I2C wire time
including ACK bits; a smaller region can materially reduce long-frame spikes.
Actual transfers depend on rotation, page alignment, controller protocol and
driver overhead. A monochrome preview can use a 256-byte packed payload rather
than the current 4096-byte RGB565 staging payload, with a separate color path
for UnitLCD. These savings are PSRAM payload savings, not 3,840 B of internal
heap recovery. Keep display and shared PAJ7620 bus access on one owner; adding
another task introduces a stack and arbitration cost.

## HUB75: reuse unchanged source rows per DMA buffer

The current packer still converts and compares all 16 row pairs and all bit
planes, even though changed-word tracking suppresses most writes. A next
experiment can compare input RGB rows before conversion, using **two separate
6144-byte PSRAM source snapshots**, one for each physical back buffer.

Compare against the buffer from two flips ago, not only the immediately previous
frame. Invalidate on relevant format/CIE/layout changes and driver reset. Track
external RGB writes through other driver methods; otherwise a matching input
cache can incorrectly skip repairing a changed DMA buffer. Preserve pending
control-bit dirty marks and flush them even when RGB packing is skipped. Exact
memcmp/generation checks are preferable to relying solely on hash equality.

This trades about 12 KB PSRAM for fewer CIE lookups, plane traversals and DMA
reads in static/low-motion scenes. Benefit is unmeasured and may be smaller for
full-screen effects. Require all-word legacy/new DMA comparisons, unchanged
frame checks, alternating-buffer cases and external-writer cases before use.

## Framework and longer-term work

The bundled qio_opi SDK header uses a 16 KB instruction cache, 32 KB data cache,
4 KB internal-allocation preference, eight static WiFi RX buffers and 32 dynamic
RX buffers. The SDK configuration records XIP/fetch/rodata and external BSS
options as disabled. Some application build flags name different settings.
Those flags do not prove the precompiled IDF libraries implement those settings.
Audit effective configuration and runtime allocations before further flag tuning.
Changing cache sizes, BLE host, WiFi features or PSRAM execution behavior may
require rebuilding the SDK. Keep Arduino 3.x/IDF 5.x and C++17; do not replace
the pioarduino platform with Arduino 2.x.

Continuous ADC/DMA sampling is a later CPU/jitter experiment: current acquisition
dispatches up to 8000 analogRead callbacks per second on the core-0 timer task,
then stops sampling while processing the completed block. The current sample
window completed 1,629 microphone blocks, about 27.1 blocks/s. Hardware
continuous sampling can reduce callback overhead and gaps, but requires new
buffer ownership, ADC resource handling and audio behavior validation. It must
not be mixed into the first behavior-preserving batch.

True animation/render overlap is another later step. Current scheduling overlaps
menu with animation, then waits for animation before rendering. Vertex copies
alone do not make rendering independent: materials, palettes, effects and their
parameters are also mutable. Full immutable frame snapshots and explicit
buffer ownership are required. The ideal bound changes from roughly
`max(animation, menu) + render + output` to the slower core's frame workload,
but shared PSRAM/cache traffic, ADC activity and OLED transactions can limit
real gains. Do not promise that the ideal bound is achievable.

## Stability issue to fix before broader concurrency

CreateGradientMaterial builds a local `spectrum[N]` array and passes it to a
GradientMaterial constructor. That constructor copies visible rgbColors but
also retains the supplied pointer as baseRGBColors. HueShift/UpdateRGB later
read that pointer after the stack array's lifetime has ended. This is a concrete
lifetime bug for dynamically configured gradients; the current observation
does not establish whether that configuration was active. Give the material
owned base palette storage before optimizing or parallelizing that path.

## Suggested experiment order and gates

1. Repair dynamic gradient palette ownership and record a fixed-scenario baseline.
2. Simplify audio preprocessing and bin averages; compare captured audio outputs
   and speech decisions, and measure FFT task time separately.
3. Move/initialize spectrum state as one bounded block, then trial task stack
   reductions. Gate each on heap integrity and worst-case stack margins.
4. Optimize OLED dirty regions and repeated state drawing, preserving QR/OLED
   protection and shared-bus ownership. Measure awake and asleep separately.
5. Add direct animation handles and actual-count parameter storage; retain all
   interpolation modes, reset/voice/blink precedence and overflow handling.
6. Experiment with per-buffer HUB75 input-row reuse and material requirement
   flags to skip unused position/normal/UV work. Gate renderer changes on host
   and on-device A/B, and packer changes on full DMA equality.
7. Only then consider ADC/DMA, SDK cache/host configuration and true frame overlap.

At the observed 54.10 wall FPS (about 18.49 ms/frame), 60 FPS needs about
1.82 ms/frame saved, 70 FPS about 4.20 ms, and 80 FPS about 5.99 ms. These are
budget calculations, not speedup predictions. A sensible next target is stable
60 FPS with the OLED awake, more internal heap headroom, no audio drops, byte
equality where promised, and an extended soak across expressions, BLE commands,
serial dumps, provisioning, recovery and OTA. No firmware was rebuilt/flashed
or modified for this review.
