# First-boot device registration display

Startup checks for `/user_config.json` **before** `EnsureUserConfig()` can create
the default file. When that file was absent, the auxiliary display shows a QR
code containing the complete chip-derived 32-character hexadecimal device ID.
It appears immediately after display setup and the file-existence check, before
HUB75 boot tests, asset checks and the factory indicator blink. The missing-file
path skips the ordinary "Starting..." and device-ID text delays.
The payload has no URL, label, or case conversion, and uses the same
`ReadUniqueDeviceId()` identity as the configuration/registration flow.

The registration area remains **120x42**, centered using the detected display's
dimensions. On a 128x64 display it starts at **(4,11)**. With the password shown,
a square **42x42** QR tile starts at **(4,11)** on the left; the right pane starts
at **(50,11)** and shows `WiFi PW` plus the actual setup AP password. Password
text wraps automatically; long passwords use a compact font to fit all 63 ASCII
characters. The font/text style is restored after drawing. The password comes
from the same trimming/fallback resolver used by NetWizard. Startup without a
config uses that resolver's default; saved home WiFi credentials are not shown.
The QR-only helper still centers its tile when no password is supplied. The
remaining background is black to reduce OLED exposure. The code has solid
black/white pixels, no text overlay or
dithering. Version 2 fits the complete ID. Its 25x25 module pattern has the
largest uniform integer scaling possible at this height: **1 pixel/module**.
Two pixels/module with the four-module quiet zone would require **66x66**.
An attempted 30px pattern with uneven module widths failed all 576 software
decode cases and was discarded. Smaller screens are clamped to their actual
size and must have at least 33 pixels on each axis.

Registration is held when the original user configuration, the selected face
JSON, or the selected animation JSON is absent. Device-specific paths and the
existing `/universal_face.json` and `/example_animation.json` fallbacks count
as available assets. Writing a default `user_config.json` does not dismiss the
QR while either asset is missing, including after another boot.

The foreground retains the QR and setup AP password during WiFi connection,
captive portal completion, remote checks, failed downloads, and offline waiting.
Startup progress receives no display while registration is held, and text
scrolling is disabled. Missing files no longer show the scrolling
`Offline boot blocked` page or cause the missing-face restart loop. OLED
protection still shifts the pattern inside its quiet zone and dims it on idle;
registration never sleeps the panel.

Synchronous startup downloads animation before validating both assets and
initializing the renderer. The foreground is released only after config, face
and animation files are present. Offline waiting checks every ten seconds and
restarts once all required files become available. This is a file-presence check,
not a JSON validity check. A timed-out captive portal keeps the QR visible;
retry provisioning after restarting the device.

OLED protection applies to both **M5UnitOLED** (SH1107) and **GLASS2** (SSD1309).
[GLASS2 is also an OLED, with a transparent background](https://docs.m5stack.com/en/unit/Glass2%20Unit).
Registration contrast is **255**, including GLASS2, which previously stayed at
45. The contrast command changes emission strength without adjusting precharge
or VCOM settings. A dark backing behind transparent GLASS2 improves optical
contrast; firmware cannot turn its transparent, unlit pixels into opaque black.

## OLED protection policy

| Mode | Initial contrast | Inactivity | Pixel shifting | Display off |
| --- | --- | --- | --- | --- |
| Registration | 255 | 90s: 64; 5min: 32 | Every 30s, +/-1px inside the existing tile | Never during registration |
| Normal HUD | 96 | 60s: 24 | Every 30s, 0..2px X and 0..1px Y | After 5min |

Registration keeps a minimum seven-pixel quiet zone during all nine shift
positions, with the tile inside the required 120x42 region. The HUD clears old
pixels when shifting or waking so text/icons do not leave trails. BLE activity,
queued remote commands, a proximity rising edge, or the OTA button wake the HUD.
AP client count changes, portal state changes and the OTA button restore full
registration contrast. Continuous animation and microphone updates do not count
as interaction. The OLED RAM image is retained while display output is off.

NetWizard runs its existing state machine in an application-owned waiting loop
on OLED devices so protection continues throughout the portal wait, including
timeouts and the existing HTTP exit grace. No extra task, timer, framebuffer or
per-frame allocation is used. The unsigned timers handle `millis()` rollover;
BLE callbacks request wake atomically and do not issue I2C operations. LCDs keep
their original behavior. HUB75 render/output settings are unaffected.

These measures reduce static pixel exposure; actual panel lifetime depends on
the hardware and environment and is not measured by the software tests.

## Validation

Run `host-tests/build_oled.cmd` to compile the actual QR renderer/encoder and
OLED protection policy with a mock panel. It tests idle timing, rollover,
sleep/wake, constant-state I2C suppression, LCD isolation, and exports 288 QR
fixtures. With Pillow and zxing-cpp available, run
`python host-tests/check_qr_decode.py` for 576 full-ID decode checks across 32
IDs, nine positions and two orientations. The software fixtures resolve each
screen pixel without interpolation; phone optics and transparent backgrounds
still require physical scanning.

The updated firmware passed PROFILE/RELEASE compilation, the policy/panel host
checks and all 576 decode cases. PROFILE was flashed to COM5 at 0x10000 with
esptool hash verification, preserving configuration/filesystem contents. The
COM5 portal-wait logs confirm the normal-mode contrast transition to 24 and
the five-minute idle display-off event (`asleep=1`). Hardware wake and
registration-mode brightness remain to be checked during physical interaction. The
static internal data footprint is 82,880 bytes, 48 bytes above the preceding
82,832-byte build. Physical phone scanning, UnitOLED hardware, and panel aging
are not established by these checks.

On this USB-Serial-JTAG connection, the serial recovery that produced app logs
released DTR/IO0 before a 200ms RTS reset pulse, then enabled DTR for monitoring
after 500ms. The older recipe with DTR asserted during reset produced no app
output on COM5. No Flash or configuration data is changed by these reset pulses.

The QR/password side-by-side layout also passes all 576 decode cases. Host
checks reconstruct the full displayed default-length and 63-character passwords,
verify font restoration and keep illuminated pixels inside the 120x42 area.
Both PROFILE and RELEASE compile with the early-startup/password changes;
PROFILE is flashed to COM5 with hash verification. Existing Flash data is
preserved, so the missing-config branch is not forced on an already-configured
device during this update.

### Persistent foreground validation (2026-10-04)

PROFILE and RELEASE both compile with the persistent registration guard; the
OLED host checks and all 576 QR decodes pass. PROFILE reports 82,880 bytes of
static RAM, unchanged from the preceding build. The merged 8 MB image's NVS
and filesystem partitions were checked to contain only erased `0xFF` padding.
COM5 then received a successful whole-chip erase and this firmware image with
esptool hash verification; no filesystem asset image was uploaded.

During this boot, the user confirmed the QR and AP password remained visible.
Serial observations showed `registration=1 contrast=64 asleep=0` at 84.7s and
`registration=1 contrast=255 asleep=0` at 121.9s. At 165.8s, after provisioning,
the file-presence guard logged `Assets ready; starting renderer` and switched
to normal OLED mode. The new configuration/assets were preserved. The test
therefore covered registration display and handoff to rendering; this boot did
not reach an unattended portal timeout or a second boot with missing assets.
An additional read-only serial sample after handoff reported steady frames at
50.5-51.6 FPS, `intFree=49128`, `largestBlk=31732`, and `micDrops=0` (12-second
sample, not a long-duration stability test). The first sample included a
startup stall; later samples returned to the normal frame rate.
