"""Measure live PROFILE shader modes without modifying configuration or assets."""
import argparse
import json
import re
import statistics
import time
from pathlib import Path

import serial
from serial_debug import receive


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM5')
    parser.add_argument('--seconds', type=float, default=30)
    parser.add_argument('--modes', nargs='+', choices=['normal', 'spiral', 'gradient', 'solid'],
                        default=['normal', 'spiral', 'gradient', 'normal'])
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error('--seconds must be positive')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    port = serial.Serial(port=None, baudrate=115200, timeout=.3, write_timeout=2)
    port.dtr = True
    port.rts = False
    port.port = args.port
    port.open()
    results, logs = [], []
    try:
        receive(port, 'dbg cancel')
        startup_deadline = time.monotonic() + 120
        while True:
            _, records = receive(port, 'dbg stats')
            if any(re.search(r'@DBG STACK.* anim=[1-9]\d*', line) for line in records):
                break
            if time.monotonic() >= startup_deadline:
                raise TimeoutError('Animation worker has not started')
            next_check = time.monotonic() + 1
            while time.monotonic() < next_check:
                port.readline()
        for mode in args.modes:
            _, ack = receive(port, f'dbg profile {mode}')
            logs.extend(ack)
            # Allow OLED and animation state to settle and discard mixed windows.
            settle = time.monotonic() + 4
            while time.monotonic() < settle:
                port.readline()
            _, before = receive(port, 'dbg stats')
            first, _ = receive(port, 'dbg dump rgb0 0 3')
            start = time.monotonic()
            lines = []
            while time.monotonic() - start < args.seconds:
                line = port.readline().decode('ascii', errors='replace').strip()
                if line.startswith(('[FRAME]', 'anim=', '[RAST]', '[OLED]')):
                    lines.append(line)
                if any(marker in line for marker in ('Guru Meditation', 'assert failed', 'panic')):
                    raise RuntimeError(line)
            last, _ = receive(port, 'dbg dump rgb0 0 3')
            elapsed = time.monotonic() - start
            _, after = receive(port, 'dbg stats')
            _, check = receive(port, 'dbg check')
            logs.extend(before + lines + after + check)
            metrics = {}
            for line in lines:
                if not line.startswith(('[FRAME]', 'anim=')):
                    continue
                for key, value in re.findall(r'(fps|avg|max|menu|display|anim|render)=([\d.]+)', line):
                    metrics.setdefault(key, []).append(float(value))
            result = dict(mode=mode, seconds=elapsed, frames=last['frame'] - first['frame'],
                          wall_fps=(last['frame'] - first['frame']) / elapsed,
                          before=before, after=after, check=check,
                          metrics={key: dict(samples=len(values), median=statistics.median(values),
                                             min=min(values), max=max(values))
                                   for key, values in metrics.items()})
            results.append(result)
            print(json.dumps(result), flush=True)
    finally:
        try:
            _, ack = receive(port, 'dbg cancel')
            logs.extend(ack)
            _, ack = receive(port, 'dbg profile normal')
            logs.extend(ack)
        finally:
            port.close()
            args.output.write_text(json.dumps(results, indent=2), encoding='utf-8')
            args.output.with_suffix('.log').write_text('\n'.join(logs) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
