"""Exercise snapshots while rendering, then watch heap/frame telemetry.

python tools/serial_debug_soak.py --port COM5 --seconds 600 --dumps 60
Requires PROFILE firmware and pyserial. Does not reset or change configuration.
"""
import argparse
import json
import math
import re
import statistics
import struct
import time
from pathlib import Path

import serial
from serial_debug import receive, save


class Monitor:
    def __init__(self, port, log):
        self.port, self.log = port, log
        self.frames, self.heaps, self.verify = [], [], []
        self.faults = []
        self.last_main_progress = None

    def write(self, data):
        return self.port.write(data)

    def readline(self):
        line = self.port.readline()
        text = line.decode("ascii", errors="replace").strip()
        if text and not text.startswith("@DBG DATA "):
            self.log.write(text + "\n")
            self.log.flush()
        match = re.search(r"\[FRAME\] fps=([\d.]+).*avg=([\d.]+).*display=([\d.]+)", text)
        if match:
            self.frames.append(tuple(map(float, match.groups())))
            self.last_main_progress = time.monotonic()
        if text.startswith(('@DBG BEGIN ', '@DBG DATA ', '@DBG END ', '@DBG TIMING ')):
            self.last_main_progress = time.monotonic()
        match = re.search(r"pipe=1 intFree=(\d+) largestBlk=(\d+).*micDrops=(\d+)", text)
        if match:
            self.heaps.append(tuple(map(int, match.groups())))
        match = re.search(r"\[HUBVERIFY\] checked=(\d+) diffs=(\d+)", text)
        if match:
            self.verify.append(tuple(map(int, match.groups())))
        if re.search(r"panic|CORRUPT HEAP|Guru Meditation|watchdog|brownout|\[FATAL\]", text, re.I):
            self.faults.append(text)
        if self.last_main_progress is not None and time.monotonic() - self.last_main_progress > 15:
            raise RuntimeError('Main-loop telemetry/debug transport stalled for 15 seconds')
        return line


def validate(metadata, data, vertex_count=None):
    if metadata["format"] in ("f32le", "xyzf32le"):
        if not all(math.isfinite(v) for (v,) in struct.iter_unpack("<f", data)):
            raise RuntimeError("Non-finite published array")
    if metadata["name"] == "indices0" and vertex_count is not None:
        if max(struct.unpack("<" + "H" * (len(data) // 2), data)) >= vertex_count:
            raise RuntimeError("Face index exceeds published vertex count")
    if metadata["format"] == "dma16le":
        words = struct.unpack("<" + "H" * (len(data) // 2), data)
        width = metadata["width"]
        for plane in range(metadata["height"]):
            for x in range(width // 2):
                if (words[plane * width + x] & 63) != (words[plane * width + width - 1 - x] & 63):
                    raise RuntimeError("Chained-panel DMA RGB bits are not mirrored")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM5")
    parser.add_argument("--seconds", type=int, default=600)
    parser.add_argument("--dumps", type=int, default=60)
    parser.add_argument("--directory", type=Path, default=Path(".pio/build/debug-soak"))
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=True)
    with serial.Serial(port=None, baudrate=115200, timeout=0.3, write_timeout=2) as port:
        port.dtr = False
        port.rts = False
        port.port = args.port
        open_deadline = time.monotonic() + 15
        while True:
            try:
                port.open()
                break
            except serial.SerialException:
                if time.monotonic() >= open_deadline:
                    raise
                time.sleep(0.2)
        with (args.directory / "serial.log").open("w", encoding="utf-8") as log:
            monitor = Monitor(port, log)
            start = time.monotonic()
            # Startup may spend time downloading or initializing BLE.
            while time.monotonic() - start < 120 and not monitor.heaps:
                monitor.readline()
            receive(monitor, "dbg cancel")
            for command in ("dbg help", "dbg list", "dbg stats", "dbg check"):
                print(receive(monitor, command)[1], flush=True)
            partial, data = receive(monitor, "dbg dump rgb0 6130 14")
            if partial["offset"] != 6130 or len(data) != 14:
                raise RuntimeError("Partial range not honored")
            targets = ["rgb0", "scratch0", "hud", "vertices0", "indices0", "samples", "fft", "filtered",
                       "dmafront0", "dmaback0"]
            transferred = 0
            vertex_count = None
            hashes = set()
            for i in range(args.dumps):
                name = targets[i % len(targets)]
                metadata, data = receive(monitor, "dbg dump " + name)
                if name == "vertices0":
                    vertex_count = metadata["width"]
                validate(metadata, data, vertex_count)
                transferred += len(data)
                if name == "rgb0":
                    hashes.add(metadata["fnv"])
                    save(args.directory / "rgb0.ppm", metadata, data)
                if i % len(targets) == len(targets) - 1:
                    receive(monitor, "dbg check")
                    print(f"snapshots={i + 1} bytes={transferred}", flush=True)
            # Bounds and long lines must return errors without crashing.
            for command in ("dbg dump rgb0 6144", "dbg dump rgb0 0 70000", "dbg dump vertices999",
                            "dbg dump rgb0 -1", "dbg dump rgb0 0 1 extra", "dbg dump dmafront16",
                            "dbg " + "x" * 120):
                try:
                    receive(monitor, command)
                except RuntimeError as error:
                    if "@DBG ERROR" not in str(error):
                        raise
                else:
                    raise RuntimeError("Malformed command accepted: " + command)
            last_report = time.monotonic()
            while time.monotonic() - start < args.seconds:
                monitor.readline()
                if time.monotonic() - last_report >= 45:
                    print(f"elapsed={time.monotonic() - start:.0f}s windows={len(monitor.frames)} faults={len(monitor.faults)}", flush=True)
                    last_report = time.monotonic()
            check = receive(monitor, "dbg check")[1]
            stats = receive(monitor, "dbg stats")[1]
            summary = dict(seconds=round(time.monotonic() - start, 1), snapshots=args.dumps,
                           bytes=transferred, distinct_rgb_hashes=len(hashes), windows=len(monitor.frames),
                           median_fps=statistics.median(f[0] for f in monitor.frames),
                           median_display_ms=statistics.median(f[2] for f in monitor.frames),
                           last_heap=monitor.heaps[-1] if monitor.heaps else None,
                           heap_range=[min(h[0] for h in monitor.heaps), max(h[0] for h in monitor.heaps)],
                           max_mic_drops=max(h[2] for h in monitor.heaps),
                           pack_verify=monitor.verify, faults=monitor.faults, final_check=check, final_stats=stats)
            (args.directory / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
            print(json.dumps(summary), flush=True)
            if (monitor.faults or any(diffs for _, diffs in monitor.verify) or
                    not any("heap=ok" in line for line in check)):
                raise RuntimeError("Stability gate failed; inspect log")


if __name__ == "__main__":
    main()
