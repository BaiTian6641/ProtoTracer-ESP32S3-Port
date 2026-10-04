"""Real BLE + UART regression. Never replaces an occupied GIF slot."""
import argparse
import asyncio
import codecs
import json
import struct
import time
from pathlib import Path
import serial
from bleak import BleakClient, BleakScanner
from serial_debug import receive, fnv1a

SERVICE = '73cf57c7-6797-46e8-8202-dc5e7f956b57'

class Link:
    def __init__(self, device):
        self.client = BleakClient(device)
        self.messages = asyncio.Queue()
        self.text = ''
        self.decoder = codecs.getincrementaldecoder('utf8')()
        self.ident = 0
        self.chunk = 20

    def notification(self, _, data):
        self.text += self.decoder.decode(bytes(data))
        decoder = json.JSONDecoder()
        while self.text.strip():
            self.text = self.text.lstrip()
            try:
                obj, end = decoder.raw_decode(self.text)
            except json.JSONDecodeError:
                break
            self.text = self.text[end:]
            self.messages.put_nowait(obj)

    async def connect(self):
        await self.client.connect()
        service = self.client.services.get_service(SERVICE)
        self.rx = next(c for c in service.characteristics if 'write' in c.properties)
        tx = next(c for c in service.characteristics if 'notify' in c.properties)
        await self.client.start_notify(tx, self.notification)

    async def wait(self, predicate, seconds=30):
        async with asyncio.timeout(seconds):
            while True:
                obj = await self.messages.get()
                if predicate(obj):
                    return obj

    async def command(self, op, **fields):
        self.ident += 1
        rid = self.ident
        data = json.dumps(dict(op=op, rid=rid, **fields), separators=(',', ':')).encode()
        for offset in range(0, len(data), min(self.rx.max_write_without_response_size, 160)):
            await self.client.write_gatt_char(self.rx, data[offset:offset + min(self.rx.max_write_without_response_size, 160)], response=False)
        result = await self.wait(lambda o: o.get('op') == 'media.reply' and o.get('rid') == rid)
        self.chunk = min(result['chunk_bytes'], self.rx.max_write_without_response_size, 244)
        return result

    async def transfer(self, kind, data):
        self.ident += 1
        ident = self.ident
        for offset in range(0, len(data), self.chunk - 16):
            packet = b'PM' + bytes((1, kind)) + struct.pack('<III', ident, offset, len(data)) + data[offset:offset + self.chunk - 16]
            await self.client.write_gatt_char(self.rx, packet, response=False)
        return await self.wait(lambda o: o.get('op') == 'media.ack' and o.get('id') == ident)

def stream(frame):
    return frame + struct.pack('<I', fnv1a(frame))

def clip(frames):
    return b'PTM1' + bytes((64, 32)) + struct.pack('<HI', len(frames), 0) + b''.join(struct.pack('<HH', delay, len(data)) + data for delay, data in frames)

async def main(args):
    device = None
    for attempt in range(6):
        devices = await BleakScanner.discover(timeout=8, return_adv=True)
        device = next((d for d, adv in devices.values() if d.address.lower() == args.address.lower() and SERVICE in adv.service_uuids), None)
        if device is not None: break
    if device is None: raise RuntimeError('Target BLE device not advertising after 48 seconds')
    link = Link(device)
    output = []
    def log(text):
        output.append(text)
        print(text, flush=True)
    uart = serial.Serial(port=None, baudrate=115200, timeout=.3, write_timeout=2)
    uart.dtr = True; uart.rts = False; uart.port = args.port; uart.open()
    async def dump():
        _, data = await asyncio.to_thread(receive, uart, 'dbg dump rgb0 0 6144', 15)
        return data
    slot = None
    try:
        await link.connect()
        info = await link.command('media.list')
        assert info['ok'], info
        log(f"Connected {device.name}; packet bytes={link.chunk}; free flash={info['free_bytes']}")
        slot = next((i for i in range(info['slots']) if i not in {c['slot'] for c in info['clips']}), None)
        assert (await link.command('media.stream'))['ok']
        red = bytes((0,)) + bytes((224,)) * 2048
        assert (await link.transfer(1, stream(red)))['ok']
        assert await dump() == bytes((255, 0, 0)) * 2048
        log('PASS RGB332 stream / UART pixel equality')
        gradient = b'\x02' + b''.join(struct.pack('<H', ((i % 64) >> 1) << 11 | (i // 64 * 2) << 5 | (i % 32)) for i in range(2048))
        assert (await link.transfer(1, stream(gradient)))['ok']
        expected = b''.join(bytes(((c >> 11) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)) for c, in struct.iter_unpack('<H', gradient[1:]))
        assert await dump() == expected
        log('PASS RGB565 stream / UART pixel equality')
        bad = bytearray(stream(red)); bad[-1] ^= 1
        assert not (await link.transfer(1, bad))['ok']
        assert await dump() == expected
        log('PASS bad checksum keeps previous frame')
        start = time.monotonic()
        for i in range(60):
            assert (await link.transfer(1, stream(gradient)))['ok']
        elapsed = time.monotonic() - start
        log(f'RGB565 raw BLE 60 frames: {60 / elapsed:.2f} FPS / {len(gradient) * 60 / elapsed / 1024:.1f} KiB/s')
        for label, frame in [('RGB332 raw', red), ('RGB332 RLE', b'\x01' + b'\xff\xe0' * 8 + b'\x08\xe0')]:
            start = time.monotonic()
            for i in range(60): assert (await link.transfer(1, stream(frame)))['ok']
            elapsed = time.monotonic() - start
            log(f'{label} BLE 60 frames: {60 / elapsed:.2f} FPS / {len(frame) * 60 / elapsed / 1024:.1f} KiB/s')
        await asyncio.sleep(3)
        assert (await link.command('media.status'))['mode'] == 'face'
        log('PASS idle stream restores face')
        if slot is not None:
            await link.command('media.upload.begin', slot=slot, size=100, hash=0)
            assert (await link.transfer(2, b'x' * 100))['ok']
            assert not (await link.command('media.upload.end'))['ok']
            assert slot not in {c['slot'] for c in (await link.command('media.list'))['clips']}
            log('PASS invalid upload not published')
            # Both frames identical, so every playback-time dump must match.
            movie = clip([(500, gradient), (1000, gradient)])
            result = await link.command('media.upload.begin', slot=slot, name='test-动画.gif', size=len(movie), hash=fnv1a(movie))
            assert result['ok'], result
            for off in range(0, len(movie), 4096):
                result = await link.transfer(2, movie[off:off + 4096]); assert result['ok'] and result['offset'] == min(off + 4096, len(movie))
            assert (await link.command('media.upload.end'))['ok']
            saved = next(c for c in (await link.command('media.list'))['clips'] if c['slot'] == slot)
            assert saved['name'] == 'test-动画.gif'
            # Interrupted replacement leaves the already validated clip intact.
            assert (await link.command('media.upload.begin', slot=slot, size=100, hash=0))['ok']
            assert (await link.transfer(2, b'x' * 50))['ok']
            await link.client.disconnect(); await asyncio.sleep(2); await link.connect()
            saved = next(c for c in (await link.command('media.list'))['clips'] if c['slot'] == slot)
            assert saved['bytes'] == len(movie) and saved['name'] == 'test-动画.gif'
            log('PASS interrupted replacement retains original GIF')
            assert (await link.command('media.play', slot=slot))['ok']
            await asyncio.sleep(.2)
            assert await dump() == expected
            log('PASS flash upload / playback / exact frame')
            await link.client.disconnect(); await asyncio.sleep(2)
            assert await dump() == expected
            await link.connect()
            assert (await link.command('media.status'))['mode'] == 'gif'
            assert (await link.command('media.delete', slot=slot))['ok']; slot = None
            log('PASS offline GIF / reconnect / delete')
        for name in ('wave', 'hex', 'orb', 'badapple'):
            assert (await link.command('media.play', builtin=name))['ok']
            await asyncio.sleep(.25)
            first = await dump(); await asyncio.sleep(.3); second = await dump()
            assert first != second
            log(f'PASS built-in {name}: frames advance')
        assert (await link.command('media.stop'))['mode'] == 'face'
        _, records = await asyncio.to_thread(receive, uart, 'dbg check', 15)
        assert any('heap=ok' in r for r in records), records
        log('PASS restored face / renderer health')
    finally:
        if link.client.is_connected:
            if slot is not None:
                await link.command('media.upload.cancel')
                await link.command('media.delete', slot=slot)
            await link.command('media.stop')
            await link.client.disconnect()
        uart.close()
        Path('build/media_ble_test.txt').write_text('\n'.join(output) + '\n')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--address', required=True)
    parser.add_argument('--port', default='COM5')
    asyncio.run(main(parser.parse_args()))
