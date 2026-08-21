# host-tests — Host-side A/B validation of the render core

Compiles the **real** firmware render headers (`src/Math`, `src/Render`,
`src/Materials`, `src/Flash/PixelGroups/P3HUB75.h`) on Windows x64 with MSVC
and verifies that the **new direct triangle rasterizer**
(`Camera::RasterizeDirect`, `DIRECT_RASTERIZER=1`) produces **byte-identical**
output to the **legacy pixel-driven ray-cast + QuadTree** path
(`Camera::Rasterize`, `DIRECT_RASTERIZER=0`) for one fixed synthetic scene.

## Run

```bat
host-tests\build_and_compare.cmd
```

Requires the MSVC BuildTools environment
(`C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\...\vcvars64.bat`,
path configurable at the top of the .cmd). Exits **0 = PASS** (byte-identical),
**1 = FAIL** (mismatch or build/run error), **2 = missing input files**.

Individual steps, if you want them:

```bat
:: build one variant (run from host-tests\, after vcvars64)
cl /nologo /std:c++17 /EHsc /O2 /MT ^
   -Ishim -I..\src -I..\src\Math -I..\src\Render -I..\src\Materials -I..\src\Flash -I..\src\Screenspace ^
   -DDIRECT_RASTERIZER=0 main.cpp /Fe:legacy.exe
legacy.exe legacy.bin
powershell -File compare.ps1 -LegacyBin legacy.bin -DirectBin direct.bin
```

## What it validates

- The 2048-entry `P3HUB75` grid passes `Camera::CheckDirectSupport()`
  (regular 64×32 grid, 3-unit spacing), so the direct path actually engages.
- For the fixed scene (4 triangles: two flat at Z=300, one tilted toward
  Z=340 overlapping the flat ones, one separate flat triangle; one
  `SimpleMaterial`; camera at (0,0,0) with ZForward/YUp layout, mirroring
  `TasESP32S3KitV1`), both paths write the same 2048×3-byte RGB888 frame.
  The overlapping triangles exercise depth arbitration
  (`averageDepth` / strict-closer tie-break) in both paths; the tilted
  triangle exercises non-axis-aligned pixel bboxes.
- `compare.ps1` reports file sizes, FNV-1a 32-bit checksums, non-black pixel
  counts, the number of differing bytes and the first 10 differing byte
  indices with values.

## Layout

| File | Purpose |
|---|---|
| `main.cpp` | Single-TU harness: scene setup, one `Rasterize()` call, frame dump + report |
| `shim/Arduino.h` | Empty guard (does **not** define `ARDUINO`) |
| `shim/WString.h` | Minimal `String` (ctors, `c_str`, `+`, `isEmpty`, …) |
| `shim/esp_attr.h` | `IRAM_ATTR` etc. as no-op macros |
| `shim/esp_heap_caps.h` | `MALLOC_CAP_*` bits + `heap_caps_*` → host malloc/posix_memalign |
| `shim/esp_dsp.h` | Scalar `dsps_mulc_f32`, `dsps_add_f32`, `dsps_sub_f32`, `dsps_dotprod_f32_ae32` |
| `shim/ProtoGC.h` | `protogc::ProtoGC` alloc/free → malloc/free, collect/poll no-ops |
| `build_and_compare.cmd` | Builds both variants, renders, compares, propagates exit code |
| `compare.ps1` | Byte-for-byte diff + checksums + report |

## Notes

- Deterministic: fixed scene, no time/random inputs. Both builds use the same
  scalar `esp_dsp.h` shim, so only the render algorithm differs.
- `src/` is never modified; only headers are consumed.
- If `src/` gains new platform includes (e.g. FreeRTOS under
  `CAMERA_RASTER_WORKER=1`), add matching shims under `shim/` — the include
  dir is placed **first** on the `/I` list so shims win.

## Environment caveat (this dev machine)

The machine's **App Control (WDAC)** policy blocks *freshly-built, unsigned*
host executables by hash (`"An Application Control policy has blocked this
file"`, exit code 4551). The `.cmd` builds `legacy.exe`/`direct.exe` fine, but
running them may be denied on such a machine. On 2026-08-21 the harness ran to
completion and produced **byte-identical** frames (`legacy.bin` == `direct.bin`,
2048×3 bytes, 777 lit pixels, FNV-1a match); those artifacts are kept in this
directory as evidence. To re-run on an App Control-enforced machine, either
allowlist the output exes or run on a machine without the policy.
