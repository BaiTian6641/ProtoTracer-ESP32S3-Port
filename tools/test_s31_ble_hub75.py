"""Verify S31 BLE media frames, USB snapshots and the eight connected HUB75 signals."""
import argparse
import asyncio
import json
from pathlib import Path
import serial
from bleak import BleakScanner
from media_ble_test import Link, SERVICE, stream
from serial_debug import receive
from slogic_combo8_capture import capture
from analyze_s31_hub75 import analyze
import numpy as np


async def run(args):
    devices = await BleakScanner.discover(timeout=10, return_adv=True)
    matches = [device for device, advertisement in devices.values()
               if SERVICE in advertisement.service_uuids and
               (device.address.lower() == args.address.lower() if args.address else device.name == 'ProtoTracer-S31')]
    if len(matches) != 1: raise RuntimeError(f'Expected one S31 BLE device, found {len(matches)}')
    link = Link(matches[0])
    uart = serial.Serial(port=None, baudrate=115200, timeout=.3, write_timeout=2)
    uart.dtr = True; uart.rts = False; uart.port = args.port; uart.open()
    args.output.mkdir(parents=True, exist_ok=True)
    results = {'device':matches[0].address, 'patterns':{}}
    try:
        await link.connect()
        assert (await link.command('media.list'))['ok']
        assert (await link.command('media.stream'))['ok']
        for name, packed, rgb, expected_mask in [
                ('red',224,(255,0,0),8), ('green',28,(0,255,0),16),
                ('blue',3,(0,0,255),32), ('black',0,(0,0,0),0)]:
            payload = stream(bytes((0,)) + bytes((packed,)) * 2048)
            assert (await link.transfer(1, payload))['ok']
            await asyncio.sleep(.25)
            _, pixels = await asyncio.to_thread(receive, uart, 'dbg dump rgb0 0 6144', 15)
            assert pixels == bytes(rgb) * 2048, f'{name}: USB pixel snapshot differs'
            path = args.output / f'{name}.bin'
            acquisition = asyncio.create_task(asyncio.to_thread(capture, path, args.rate_mhz, .05))
            # Keep stream mode alive while USB acquisition runs; one BLE caller.
            while not acquisition.done():
                assert (await link.transfer(1, payload))['ok']
                await asyncio.sleep(.2)
            await acquisition
            summary = analyze(path)
            raw = np.frombuffer(path.read_bytes(), dtype=np.uint8)
            rises = np.flatnonzero((raw[:-1] & 1) == 0)
            rises = rises[(raw[rises + 1] & 1) != 0] + 1
            values = raw[rises] & 0x38
            assert not np.any(values & (0x38 ^ expected_mask)), f'{name}: wrong RGB1 channel'
            if expected_mask: assert np.any(values & expected_mask), f'{name}: RGB1 absent'
            assert set(summary.get('complete_clock_bursts', {})) == {'128'}, summary
            assert summary['oe_enabled_clock_edges'] == summary['oe_enabled_at_latch'] == 0, summary
            assert summary['lat_high_clock_edges'] == 0, summary
            assert summary['latch_after_final_clock_ns']['min'] > 0, summary
            assert summary['ab_progression_mod4'] and summary['complete_ab_latch_runs'] == [8], summary
            results['patterns'][name] = summary
            print(f'PASS {name}: BLE frame, exact USB pixels, captured RGB1 and 128-clock rows', flush=True)
        assert (await link.command('media.stop'))['ok']
        _, stats = await asyncio.to_thread(receive, uart, 'dbg stats', 15)
        results['stats'] = stats
        print('\n'.join(stats), flush=True)
    finally:
        if link.client.is_connected:
            try: await link.command('media.stop')
            finally: await link.client.disconnect()
        uart.close()
        (args.output / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--address', default='')
    parser.add_argument('--port', default='COM6')
    parser.add_argument('--rate-mhz', type=int, default=40)
    parser.add_argument('--output', type=Path, default=Path('build/s31-ble-hub75'))
    asyncio.run(run(parser.parse_args()))
