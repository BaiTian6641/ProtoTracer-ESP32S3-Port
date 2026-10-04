# BLE media v1 (ESP32-S3, firmware 1.3.0+)

The Web Bluetooth remote supports camera, image/video and browser screen capture,
GIF conversion/upload/play/delete, and the Wave, Hex, Orb and Bad Apple flash clips. Media
uses the existing RX/TX UUIDs; legacy JSON controls remain compatible. Android's
native remote recognizes the capability and offers built-in clips / restore face;
capture and GIF upload are currently provided by the web remote.
The web playlist is a dropdown grouped into built-in clips and uploaded GIFs.
Play acts on the selected entry; Delete is enabled only for uploaded GIFs. Refresh
keeps the selection when that entry still exists, and a successful upload selects
the uploaded clip. Selected clips loop until stopped or replaced by another selection.

## Runtime

The phone decodes GIF, handles transparency, disposal 1/2/3 and interlacing, scales
to 64x32, quantizes and chooses raw or RLE. `omggif` is vendored with its MIT notice;
no CDN or phone image upload to a server is involved. GIF conversion runs in a
Worker. Source limits: 16 MiB, 4M canvas pixels, 1000 frames, 128M total canvas
pixels processed. Output limit: 1 MiB per clip, 8 numbered flash slots. GIF frame
delays are retained with a 20 ms minimum and a 100 ms default for missing/zero delay.
The GIF loop extension is deliberately replaced by continuous device looping.

The firmware does not decode GIF or compress images. It expands bounded raw/RLE
frames to RGB888 and publishes only valid complete frames. While media is active,
the main loop skips animation-worker dispatch and 3D rasterization, then copies
media to the existing camera color buffer. HUB75 panel chaining, channel mapping,
brightness and auxiliary preview use the existing controller. Exiting resumes the
same scene/expression state without moving vertices or changing morph settings.
Camera capture permission is requested by the browser; capture uses no audio.
Since 1.3.1, projection defaults to filling a landscape canvas: 64 pixels wide and
32 pixels high. This scales the source horizontally/vertically to fill every pixel.
Aspect-fit (letterbox) and aspect-fill (crop) are selectable alternatives. Rotation
0/90/180/270 degrees and horizontal mirroring run on the phone before compression;
rotation is also applied during GIF conversion. The transmitted frame stays 64x32
for every source orientation.

Media buffers are allocated once on first use in PSRAM: two RGB888 frames (12,288 B)
plus one 4,101 B assembly buffer. No extra task or internal frame buffer is added.
JSON/ack handling still allocates small transient messages like existing BLE controls.
The firmware requests MTU247 and a 7.5–15 ms connection interval on NimBLE; the
central chooses the final settings. Packet size is negotiated and limited to 244 B,
with a working 20 B fallback. FPS depends on phone/browser/radio/content and color
depth. The UI displays achieved FPS and never queues a backlog of stale frames.
The main loop also reads the current peer MTU directly so reconnects cannot leave
the media sender stuck at the default capacity when a fresh callback is absent.

Live streaming ends on disconnect or 2.5 seconds without completed frames **and**
received fragments. Hiding the browser page stops capture. GIF/built-in playback
continues through BLE disconnects; reboot starts normal face mode. Playback reads
one bounded frame from flash when its deadline arrives, loops, and limits catch-up
after a long stall. Upload pauses playback and temporarily returns to face mode.

Uploads are acknowledged per 4096-byte block. The temporary `/media/.upload` is
checked for length, FNV-1a, format, frame lengths and decoding before atomic
LittleFS rename to `/media/gifN.ptm`. Interrupted uploads cannot replace an existing
clip. Uploads require a 32 KiB free-space reserve **in addition to** the new file;
overwriting needs room for the complete temporary file. Flash filenames are fixed
by slot; optional original names (at most 96 UTF-8 bytes) live in `.name` sidecars.
Names are labels, never paths. A power loss between clip rename and label update can
leave an old label; it cannot expose a partial clip. FNV checks accidental corruption,
and is not an authentication mechanism. BLE access policy is unchanged.

## JSON commands

All media commands carry a uint32 `rid`. Reply:
`{"op":"media.reply","rid":1,"ok":true,"mode":"face",...}`.
Failures include `error`. `chunk_bytes` reflects the current negotiated link.

| Operation | Fields | Behavior |
| --- | --- | --- |
| `media.list` | — | `clips` (slot, name, bytes), builtins, free_bytes, slots, max_file_bytes |
| `media.status` | — | mode, frames, packet capacity and storage limits |
| `media.stream` | — | Black background, accept live frames |
| `media.stop` | — | Cancel upload, stop playback, restore face |
| `media.play` | slot **or** builtin | Loop stored GIF or `wave`/`hex`/`orb`/`badapple` |
| `media.delete` | slot | Remove clip and label; stop it if currently playing |
| `media.upload.begin` | slot, size, hash, optional name | Start temporary file |
| `media.upload.end` | — | Validate and atomically publish |
| `media.upload.cancel` | — | Remove temporary file, restore face |

JSON command writes must also fit negotiated ATT capacity. Notifications can split
UTF-8 characters: clients use streaming UTF-8 decoders before JSON reassembly.
The device queues media replies until an existing chunked JSON message finishes.

## Binary writes and file layout

All integers are little-endian. Each binary write has a 16-byte envelope:

| Offset | Value |
| --- | --- |
| 0..1 | ASCII `PM` |
| 2 | version 1 |
| 3 | 1 (live frame) / 2 (upload block) |
| 4..7 | uint32 transfer ID |
| 8..11 | uint32 fragment offset |
| 12..15 | uint32 total payload size |
| 16..end | contiguous fragment bytes |

Live payload: encoded frame followed by its uint32 FNV-1a. Upload payload: at most
4096 consecutive file bytes. Send one transfer and wait for
`{"op":"media.ack","id":2,"ok":true,"offset":4096}` before the next.
Upload `offset` is cumulative file bytes. Out-of-order fragments cannot publish a
frame; errors/timeouts require a fresh transfer or cancel/restart of an upload.

An encoded frame starts with codec 0/1 (RGB332 raw/RLE) or 2/3 (RGB565 raw/RLE).
Raw is exactly 2048 pixels. Each RLE item is uint8 count 1..255 plus one color
(one byte RGB332 or uint16 RGB565). Runs must sum to exactly 2048. Pixel order is
top-left, row-major. RGB332 is RRR/GGG/BB. MCU color expansion uses integer scaling
to 0..255. There is no cross-frame dependency, allowing immediate resynchronization.

PTM1 clip header: ASCII `PTM1`, uint8 width=64, height=32, uint16 frame count 1..1000,
uint32 reserved=0. Each frame: uint16 delay_ms >=20, uint16 encoded_length,
encoded bytes. Exact EOF is mandatory; trailing bytes are rejected.

## Built-in conversion and verification

`python tools/pack_builtin_media.py` regenerates `src/Media/BuiltinMedia.h` from
`src/Flash/ImageSequences/{Wave,Hex,Orb,BadApple}.h` with nearest sampling,
RGB332/RLE, 50 ms frames. Wave/Hex/Orb retain aspect-fit sampling. Bad Apple fills
the complete 64x32 landscape area and preserves all 300 frames of the repository's
supplied excerpt (15 seconds per loop; no audio). All 447 source frames are preserved.
The generated flash data is 152,109 bytes, including 72,196 bytes for Bad Apple,
rather than compiling the multi-megabyte source arrays.
The old Image/ImageSequence helpers are not used, avoiding their existing palette
bound/index and frame-selection problems.

Run from repository root:

```
node --test web-app/media-format.test.cjs web-app/hue-math.test.cjs web-app/fw-rotation-sim.test.cjs
host-tests\build_media.cmd
host-tests\build_and_compare.cmd
```

`web-app/media-browser.test.cjs` additionally requires Playwright and installed Edge.
It tests actual canvas/Worker GIF disposal, delays, capture via a browser test camera,
upload/play/delete, stop, English/Chinese, phone layout and disconnect. BLE is mocked
in this browser test. `tools/media_ble_test.py --address ADDRESS --port COM5`
requires bleak/pyserial and separately tests the **real** BLE radio plus UART RGB
snapshots, malformed/checksum rejection, upload, interrupted replacement, offline
playback, reconnect, deletion, built-ins and restoration. It uses an empty slot and
cleans it afterward; it never replaces an occupied user slot.

The Pages workflow runs codec/protocol and existing hue checks before publishing.
This machine has no Android SDK, so the supplemental native Android UI change
could not be APK-built here. The web remote and actual S3 firmware are the primary
tested clients for this feature.

## Hardware verification, 2026-10-04

RELEASE 1.3.0 was flashed to COM5 at application offset 0x10000, preserving the
filesystem. Real BLE testing negotiated 244-byte packets. RGB332 and RGB565 stream
frames matched the UART-dumped 6144-byte display buffer exactly. Bad checksums kept
the previous frame. Upload validation, filename persistence, interrupted replacement
retaining its original clip, offline GIF playback, reconnection/deletion, all three
built-ins advancing and restoration to face mode passed; final heap check was OK.

Windows/Bleak throughput over 60 sequential acknowledged frames per format:

| Frame type | Achieved FPS | Encoded throughput |
| --- | ---: | ---: |
| RGB565 raw (4097 B) | 10.03 | 40.1 KiB/s |
| RGB332 raw (2049 B) | 15.55 | 31.1 KiB/s |
| RGB332 RLE, uniform test frame (19 B) | 27.24 | 0.5 KiB/s |

These are this computer/radio's results, not a guarantee for a phone. The RLE result
uses a solid image; high-detail footage compresses less. GIF playback has no radio
bandwidth dependency after upload. Native browser camera/Worker/UI tests passed on
Edge at a 390px viewport. Four-codec host tests decoded all 147 built-in frames;
the existing direct/legacy rasterizer regression remained byte-identical (6144 B).
Both PROFILE and RELEASE builds passed; the final peer-MTU fix was additionally
compiled and exercised in RELEASE.

Flashed RELEASE SHA256:
`bff05a3450d6b8824f11469e7e745aaf377b9529565f3016c4fec25bde2288db`.

### RELEASE 1.3.1 follow-up

Flashed COM5 at application offset 0x10000, preserving the filesystem. Real BLE
regression passed again, including all four built-ins advancing, exact display
buffer matching, upload/play/delete, interrupted replacement, offline playback,
and restoration to face mode. Four-codec host checks decoded all 447 built-in
frames. Edge checks passed for the dropdown playlist, selection persistence,
built-in deletion protection, GIF processing and camera capture, plus exact 64x32
landscape output with all four rotations and mirroring.

Flashed RELEASE SHA256:
`5d52aa910f79d9a6af22bb1058ee4a4e77aa82e87e32a0ddfc99f03fa1434da2`.
