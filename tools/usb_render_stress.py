"""Measure render progress with USB attached, unread CDC and reopen cycles."""
import argparse
import json
import time
from pathlib import Path

import serial
from serial_debug import receive


def open_port(name):
    port = serial.Serial(port=None, baudrate=115200, timeout=.1, write_timeout=2)
    port.dtr = True
    port.rts = False
    port.port = name
    port.open()
    return port


def resync(port):
    # Pressure can overflow RX and leave truncated commands, never configuration.
    port.reset_input_buffer()
    for _ in range(8):
        port.write(b'\n')
        try:
            return receive(port, 'dbg cancel', timeout=10)
        except RuntimeError as error:
            if '@DBG ERROR' not in str(error):
                raise
    raise RuntimeError('CDC command parser failed to recover')


def frame(port):
    return receive(port, 'dbg dump rgb0 0 3')[0]['frame']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM5')
    parser.add_argument('--seconds', type=float, default=60)
    parser.add_argument('--cycles', type=int, default=30)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 60 or not 1 <= args.cycles <= 100:
        parser.error('Use 1..60 seconds and 1..100 reopen cycles')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    results = []
    port = open_port(args.port)
    try:
        resync(port)
        deadline = time.monotonic() + 120
        while True:
            _, records = receive(port, 'dbg stats')
            if any('anim=0 ' not in line and line.startswith('@DBG STACK ') for line in records):
                break
            if time.monotonic() > deadline:
                raise TimeoutError('Animation worker has not started')
            time.sleep(.5)
        for mode in ['reading', 'port_closed', 'reading', 'unread_pressure', 'reading', 'reopen']:
            resync(port)
            _, before = receive(port, 'dbg stats')
            first, start = frame(port), time.monotonic()
            if mode == 'port_closed':
                port.close()
                time.sleep(args.seconds)
                port = open_port(args.port)
                resync(port)
            elif mode == 'unread_pressure':
                next_command = start
                while time.monotonic() - start < args.seconds:
                    port.write(b'dbg stats\n')
                    next_command += .05
                    time.sleep(max(0, next_command - time.monotonic()))
                resync(port)
            elif mode == 'reopen':
                for _ in range(args.cycles):
                    port.close()
                    time.sleep(.5)
                    port = open_port(args.port)
                    resync(port)
            else:
                while time.monotonic() - start < args.seconds:
                    line = port.readline().decode('ascii', errors='replace')
                    if any(marker in line for marker in ('Guru Meditation', 'panic', 'CORRUPT HEAP')):
                        raise RuntimeError(line.strip())
            last, end = frame(port), time.monotonic()
            _, after = receive(port, 'dbg stats')
            _, check = receive(port, 'dbg check')
            result = dict(mode=mode, seconds=end-start, frames=last-first,
                          wall_fps=(last-first)/(end-start), before=before, after=after, check=check)
            results.append(result)
            args.output.write_text(json.dumps(results, indent=2), encoding='utf-8')
            print(json.dumps(result), flush=True)
            if last <= first or result['wall_fps'] < 15 or not any('heap=ok' in line for line in check):
                raise RuntimeError('Render progress / heap gate failed')
    finally:
        if port.is_open:
            port.close()


if __name__ == '__main__':
    main()
