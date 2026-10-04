"""ProtoTracer serial snapshots and PROFILE diagnostics (pip install pyserial).

python tools/serial_debug.py --port COM5 stats
python tools/serial_debug.py --port COM5 dump rgb0 --output face.ppm
python tools/serial_debug.py --port COM5 dump vertices0 --output vertices.bin
"""
import argparse
import json
import struct
import time
from pathlib import Path

import serial


def fnv1a(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def receive(port, command, timeout=30):
    port.write((command + "\n").encode("ascii"))
    deadline = time.monotonic() + timeout
    metadata = None
    data = bytearray()
    records = []
    operation = command.split()[1]
    is_dump = operation == "dump"
    expected_record = {"help": "HELP", "list": "LIST", "stats": "TIMING",
                       "check": "CHECK", "cancel": "CANCEL", "profile": "PROFILE"}.get(operation)
    while time.monotonic() < deadline:
        line = port.readline().decode("ascii", errors="replace").strip()
        if operation == "profile" and line.startswith("[MATERIAL_VERIFY]"):
            records.append(line)
            continue
        if not line.startswith("@DBG "):
            continue
        records.append(line)
        fields = line.split()
        if fields[1] == "ERROR":
            raise RuntimeError(line)
        if not is_dump and fields[1] in ("BEGIN", "DATA", "END"):
            continue
        if fields[1] == "BEGIN":
            if metadata or len(fields) != 11:
                raise RuntimeError("Invalid BEGIN record")
            metadata = dict(id=int(fields[2]), name=fields[3], offset=int(fields[4]),
                            bytes=int(fields[5]), fnv=fields[6], format=fields[7],
                            width=int(fields[8]), height=int(fields[9]), frame=int(fields[10]))
            if not 0 < metadata["bytes"] <= 65536:
                raise RuntimeError("Invalid snapshot length")
        elif fields[1] == "DATA":
            if not metadata or int(fields[2]) != metadata["id"] or int(fields[3]) != len(data):
                raise RuntimeError("Out-of-order DATA record")
            data.extend(bytes.fromhex(fields[4]))
            if len(data) > metadata["bytes"]:
                raise RuntimeError("Snapshot exceeds declared size")
        elif fields[1] == "END":
            if (not metadata or int(fields[2]) != metadata["id"] or
                    int(fields[3]) != len(data) or len(data) != metadata["bytes"] or
                    fields[4] != metadata["fnv"] or fnv1a(data) != int(metadata["fnv"], 16)):
                raise RuntimeError("Snapshot length/checksum mismatch")
            return metadata, bytes(data)
        elif fields[1] == expected_record:
            return None, records
    raise TimeoutError("No complete debug response; wait for firmware startup and retry")


def save(path, metadata, data):
    if path.suffix.lower() == ".ppm":
        width, height = metadata["width"], metadata["height"]
        if metadata["offset"] or len(data) != width * height * (3 if metadata["format"] == "rgb888" else 2):
            raise ValueError("PPM requires a complete RGB image")
        if metadata["format"] == "rgb565le":
            rgb = bytearray()
            for (pixel,) in struct.iter_unpack("<H", data):
                r, g, b = pixel >> 11, (pixel >> 5) & 63, pixel & 31
                rgb.extend(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
            data = bytes(rgb)
        elif metadata["format"] != "rgb888":
            raise ValueError("PPM requires rgb888 or rgb565le")
        path.write_bytes(f"P6\n{width} {height}\n255\n".encode() + data)
    else:
        path.write_bytes(data)
    path.with_suffix(path.suffix + ".json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM5")
    parser.add_argument("--baud", type=int, default=115200)
    sub = parser.add_subparsers(dest="command", required=True)
    for op in ("stats", "check", "list", "help", "cancel"):
        sub.add_parser(op)
    profile = sub.add_parser("profile", help="PROFILE firmware only: temporary live shader override")
    profile.add_argument("material", choices=("normal", "spiral", "gradient", "solid", "verify"))
    dump = sub.add_parser("dump")
    dump.add_argument("name")
    dump.add_argument("--offset", type=int, default=0)
    dump.add_argument("--bytes", type=int)
    dump.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    command = f"dbg {args.command}"
    if args.command == "profile":
        command += f" {args.material}"
    if args.command == "dump":
        if not args.name.isalnum() or args.offset < 0 or (args.bytes is not None and args.bytes <= 0):
            parser.error("Invalid dump name/range")
        command += f" {args.name} {args.offset}"
        if args.bytes is not None:
            command += f" {args.bytes}"
    # Configure lines before opening: do not deliberately reset an active device.
    with serial.Serial(port=None, baudrate=args.baud, timeout=0.5, write_timeout=2) as port:
        port.dtr = False
        port.rts = False
        port.port = args.port
        port.open()
        if args.command != "cancel":
            receive(port, "dbg cancel")
        metadata, result = receive(port, command)
        if metadata:
            save(args.output, metadata, result)
            print(json.dumps(metadata))
            print(f"Verified snapshot saved to {args.output}")
        else:
            print("\n".join(result))


if __name__ == "__main__":
    main()
