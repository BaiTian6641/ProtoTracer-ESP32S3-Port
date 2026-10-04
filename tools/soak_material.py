"""Run the existing snapshot/heap soak with a temporary PROFILE shader override."""
import argparse
import sys

import serial
from serial_debug import receive
import serial_debug_soak


def select(port_name, material):
    with serial.Serial(port=None, baudrate=115200, timeout=.3, write_timeout=2) as port:
        port.dtr = True
        port.rts = False
        port.port = port_name
        port.open()
        receive(port, 'dbg cancel')
        print(receive(port, 'dbg profile ' + material)[1], flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM5')
    parser.add_argument('--material', choices=['spiral', 'gradient', 'solid'], default='spiral')
    args, remaining = parser.parse_known_args()
    original_argv = sys.argv
    try:
        select(args.port, args.material)
        sys.argv = ['serial_debug_soak.py', '--port', args.port] + remaining
        serial_debug_soak.main()
    finally:
        sys.argv = original_argv
        select(args.port, 'normal')


if __name__ == '__main__':
    main()
