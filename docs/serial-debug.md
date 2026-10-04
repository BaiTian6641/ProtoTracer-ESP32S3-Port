# Serial debug snapshots

The firmware includes a read-only debug transport over Arduino `Serial`, which is
USB Serial/JTAG on the current ESP32-S3 board (COM5, 115200). This is a small
inspection interface, not an Android ADB implementation. It is independent of
BLE and the Wi-Fi startup sequence, and works without recompiling for each dump.

## Commands

Send ASCII lines ending in a newline from a serial terminal:

```text
dbg help
dbg list
dbg stats
dbg check
dbg dump rgb0
dbg dump vertices0
dbg dump rgb0 6130 14
dbg cancel
```

`stats` reports internal/PSRAM heap, minimum internal free heap, largest internal
block, loop/animation/microphone task stack high-water marks (bytes remaining),
animation/render timing, and microphone frame/drop counters.
`check` explicitly walks the heaps to check integrity; it can extend that frame.
Offsets and lengths are decimal **bytes**, not array element indices.

| Target | Contents | Encoding |
|---|---|---|
| `rgbN` | Rendered/effected camera N color array | Interleaved RGB888 |
| `scratchN` | Camera N effect scratch array | Interleaved RGB888, may contain older intermediate data |
| `hud` | Last auxiliary-screen image | RGB565, little endian; may be older than the HUB75 frame |
| `verticesN` | Object N published render vertices | XYZ float32, little endian |
| `indicesN` | Object N triangle vertex indices | ABC uint16, little endian |
| `samples` | Published microphone input, 256 values | Float32, little endian |
| `fft` | Published spectrum, 128 values | Float32, little endian |
| `filtered` | Published filtered spectrum, 128 values | Float32, little endian |
| `dmafrontR` | Last submitted HUB75 DMA buffer, physical row R (0..15) | DMA uint16 words, little endian |
| `dmabackR` | Current drawing DMA buffer, physical row R (0..15) | DMA uint16 words, little endian |

Object and camera indices are separate namespaces. Missing indices and inactive
buffers return errors. RGB image dimensions describe this port's 64x32 camera.
Snapshots represent engine buffers, not physical LED luminance after CIE/OE
brightness processing. RGB data uses PixelGroup order; HUD data already includes
its own screen orientation and monochrome/color conversion.
DMA dumps include RGB1/RGB2 and row address/LAT/OE bits. Width is the physical
chain width (128), height is color depth, and planes run from LSB to MSB. Each
target is one contiguous physical row across all bit planes. `dmafront` identifies
the buffer submitted by the last flip; the asynchronous scan may still be
finishing its previous buffer at the instant of the snapshot.

## Host client

Install `pyserial` in the Python interpreter used to run the client. On this
development machine it is already installed in PlatformIO's penv Python.
Close the PlatformIO monitor or other terminal before opening the same COM port.

```powershell
python tools/serial_debug.py --port COM5 stats
python tools/serial_debug.py --port COM5 check
python tools/serial_debug.py --port COM5 dump rgb0 --output face.ppm
python tools/serial_debug.py --port COM5 dump hud --output hud.ppm
python tools/serial_debug.py --port COM5 dump vertices0 --output vertices.bin
python tools/serial_debug.py --port COM5 dump filtered --output spectrum.bin
python tools/serial_debug.py --port COM5 dump dmafront0 --output dma-row0.bin
python tools/serial_debug.py --port COM5 dump rgb0 --offset 6130 --bytes 14 --output tail.bin
```

The client checks ordered chunk offsets, total length, and FNV-1a checksum before
saving. It also writes a JSON sidecar with format, dimensions, snapshot frame,
range, and checksum. `.ppm` exports require a complete RGB888/RGB565 image;
other extensions receive unchanged raw bytes. For example, decode vertices with
Python `struct.iter_unpack('<fff', data)` or NumPy `dtype='<f4'` followed by
`reshape(-1, 3)`.

## Runtime behavior

The firmware handles requests after `controller.Display()`, after the animation
worker has finished and before requesting its next update. The registered buffers
are stable at that point, including the published microphone arrays. A dump copies
the requested range into PSRAM and transmits that immutable snapshot while normal
rendering continues. Snapshot frame numbers count completed loop displays; they
are not camera exposure timestamps. HUD and effect scratch have their own update
history.

The protocol accepts at most 95 command characters, processes at most 64 RX bytes
per frame, and sends at most four short TX records per poll. It checks TX capacity
before writing. A client that cannot make progress for five seconds releases the
snapshot. Only one dump can be active; `dbg cancel` frees it. A maximum 65,536-byte
snapshot is allocated from PSRAM only, with no internal-SRAM fallback. Snapshot
copying and checksum work happen once per request and can extend that frame;
normal operation allocates no debug heap. Serial driver buffers and fixed protocol
state still consume some internal SRAM.

`Serial` remains the existing logging transport. The host ignores ordinary log
lines and fails visibly if a transfer is incomplete or corrupted. Commands are
available once the main render loop is running; wait for startup if a device was
just flashed. Set `-DSERIAL_DEBUG_ENABLED=0` to omit the debug interface. The
snapshot interface exposes registered arrays; it does not dereference arbitrary
addresses, write those arrays, reset the device, or change configuration.

## Wire format

```text
@DBG BEGIN ID NAME OFFSET BYTES FNV FORMAT WIDTH HEIGHT FRAME
@DBG DATA ID RELATIVE_OFFSET HEX_BYTES
@DBG END ID BYTES FNV
```

Chunks contain up to 32 data bytes. Checksums are lowercase eight-digit hex.
Other records include `HELP`, `LIST`, `STATS`, `STACK`, `TIMING`, `CHECK`, `CANCEL`,
and `ERROR`. The checksum detects accidental transfer errors; it is not
authentication. Stats end with `TIMING`, and dump transfers end with `END`.

PROFILE builds also support `dbg profile normal|spiral|gradient|solid|verify`.
The shader override applies only while rendering the face and never writes
configuration/assets. `normal` or reboot clears it. `verify` compares the
original and cached spiral shader on the target and reports colors/timing.
These optional controls are unavailable in RELEASE. The regular snapshot
commands remain read-only. See
[dynamic material measurements](s3-dynamic-material-optimization-2026-10-04.md)
for the benchmark and soak clients.

## Repeatable stability check

CDC uses zero-wait TX. Ordinary telemetry may be dropped if the host stops
reading; snapshot records retain unsent bytes and expire after five seconds.
USB attachment alone must not force rendering to wait for a serial reader.
The build applies `patch_usb_cdc.py` to make zero-timeout ring-send retries
bounded on the selected Arduino HWCDC implementation. See the dynamic material
document for the tested host-pressure and reconnect scenarios.

```powershell
python tools/serial_debug_soak.py --port COM5 --seconds 600 --dumps 160
host-tests\build_hub75.cmd
```

The soak requires PROFILE telemetry. It checks all ten target types, partial
ranges, invalid ranges/indices/long commands, finite float arrays, face index
bounds, mirrored DMA color bits, checksums, heap integrity, microphone drops, and serial fault messages.
It saves a summary and serial log under the ignored `.pio/build/debug-soak/`
directory. Changing RGB hashes demonstrates updates across requested snapshots;
this does not replace visual or audio validation on the actual panel.

The host HUB75 test includes the actual production packing code. It compares
complete DMA words, including control bits, against the previous algorithm under
1,408 input cases and tests unchanged rows, a changed row, and retention of an
existing dirty mark.
