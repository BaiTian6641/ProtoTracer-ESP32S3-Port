#!/usr/bin/env python3
"""Bounded SLogic Combo8 native USB capture; no device firmware writes.

Protocol derived from Sipeed's GPL-3.0-or-later libslogic Combo8 driver:
https://github.com/sipeed/libsigrok/tree/slogic-dev/src/hardware/sipeed-slogic-analyzer/slogic
Copyright (C) 2023-2025 Shenzhen Sipeed Technology Co., Ltd.
This adapter is distributed under GPL-3.0-or-later.
Requires pyusb and libusb-package in the selected workspace Python environment.
"""
import argparse
import json
from pathlib import Path
import struct
import time
from datetime import datetime, timezone
import usb.core
import usb.util
import libusb_package


def capture(output, rate, duration):
    if rate not in [1, 2, 4, 5, 8, 10, 16, 20, 32, 40]:
        raise ValueError("Eight-channel Combo8 rate must be a supported value at most 40 MHz")
    if not 0 < duration <= 0.25:
        raise ValueError("Capture duration must be bounded to 0.25 seconds")
    dev = usb.core.find(idVendor=0x359f, idProduct=0x0300,
                        backend=libusb_package.get_libusb1_backend())
    if dev is None: raise RuntimeError("SLogic Combo8 not found")
    usb.util.claim_interface(dev, 0)
    data = bytearray()
    started = datetime.now(timezone.utc).isoformat()
    target = int(rate * 1_000_000 * duration) + 4
    try:
        # Official CMD_START carries MHz:u16, enabled-channel-count:u8, zero.
        dev.ctrl_transfer(0x40, 0xb1, 0, 0, struct.pack("<HBB", rate, 8, 0), timeout=500)
        deadline = time.monotonic() + 5
        while len(data) < target:
            if time.monotonic() > deadline: raise TimeoutError("Capture deadline expired")
            # Queue a large USB transfer so Python scheduling gaps cannot
            # overflow Combo8's streaming FIFO between small reads.
            block = min(10 * 1024 * 1024, ((target - len(data) + 511) // 512) * 512)
            data.extend(dev.read(0x81, block, timeout=1000))
        # Sipeed documents a four-byte artifact at the start of a new acquisition.
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(data[4:target])
        output.with_suffix(".json").write_text(json.dumps({
            "model":"SLogic Combo8", "vid":"359f", "pid":"0300",
            "sample_rate_hz":rate * 1_000_000, "channels":8,
            "samples":target - 4, "format":"one byte per sample, D0 is bit 0",
            "started_utc":started,
            "initial_bytes_dropped":4,
            "assumed_gpio_order":[35,36,37,38,39,40,42,43]
        }, indent=2), encoding="utf-8")
    finally:
        # Combo8 has no reliable STOP command; bounded endpoint drain and close
        # match the vendor model. Do not send firmware/register-write requests.
        for _ in range(4):
            try: dev.read(0x81, 32768, timeout=30)
            except usb.core.USBError: break
        usb.util.release_interface(dev, 0)
        usb.util.dispose_resources(dev)
    print(f"Captured {target - 4} samples at {rate} MHz to {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rate-mhz", type=int, default=20)
    parser.add_argument("--duration", type=float, default=0.01)
    args = parser.parse_args()
    capture(args.output, args.rate_mhz, args.duration)
