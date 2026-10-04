const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const M = require('./js/media-format.js');
test('raw/RLE codecs preserve quantized pixels, exact sizes and malformed bounds', () => {
  for (const quality of ['332', '565']) for (const solid of [false, true]) {
    const rgba = Uint8ClampedArray.from({ length: 8192 }, (_, i) => i % 4 === 3 ? 255 : solid ? 128 : (i * 7919 + (i >> 8)) & 255);
    const bytes = M.encode(rgba, quality), decoded = M.decode(bytes);
    assert.equal(bytes[0], (quality === '565' ? 2 : 0) + (solid ? 1 : 0));
    for (let i = 0; i < 2048; i++) {
      const r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2];
      assert.deepEqual(Array.from(decoded.subarray(i * 4, i * 4 + 4)), quality === '332'
        ? [Math.floor((r >> 5) * 255 / 7), Math.floor((g >> 5) * 255 / 7), (b >> 6) * 85, 255]
        : [Math.floor((r >> 3) * 255 / 31), Math.floor((g >> 2) * 255 / 63), Math.floor((b >> 3) * 255 / 31), 255]);
    }
    const stream = M.frameTransfer(bytes); assert.equal(new DataView(stream.buffer).getUint32(bytes.length, true), M.hash(bytes));
    const clip = M.clip([{ bytes, delay: 50 }, { bytes, delay: 200 }]);
    assert.equal(clip.length, 20 + bytes.length * 2); assert.equal(new DataView(clip.buffer).getUint16(6, true), 2);
  }
  for (const bad of [[4], [0, 1], [1, 0, 0], [3, 255, 1]]) assert.throws(() => M.decode(Uint8Array.from(bad)));
  assert.throws(() => M.clip([]));
});
test('binary envelope works with both default MTU23 and negotiated MTU', () => {
  const bytes = Uint8Array.from({ length: 4101 }, (_, i) => i & 255);
  for (const chunk of [20, 160, 244]) {
    const assembled = new Uint8Array(bytes.length);
    for (let offset = 0; offset < bytes.length; offset += chunk - 16) {
      const p = M.fragment(1, 123, bytes.length, offset, bytes.subarray(offset, offset + chunk - 16));
      assert.ok(p.length <= chunk); const v = new DataView(p.buffer);
      assert.equal(v.getUint32(4, true), 123); assert.equal(v.getUint32(12, true), bytes.length);
      assembled.set(p.subarray(16), v.getUint32(8, true));
    }
    assert.deepEqual(assembled, bytes);
  }
});
test('BLE media waiters handle reply before write completion and disconnect', async () => {
  let context, text = '', ack = true;
  const rx = { properties: { writeWithoutResponse: true }, async writeValueWithoutResponse(bytes) {
    if (bytes[0] === 123 || text) {
      text += new TextDecoder().decode(bytes);
      let cmd; try { cmd = JSON.parse(text); } catch { return; }
      text = '';
      context.handleMessage({ op: 'media.reply', rid: cmd.rid, ok: true, chunk_bytes: 160 });
    } else if (ack) {
      const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      if (v.getUint32(8, true) + bytes.length - 16 === v.getUint32(12, true)) context.handleMessage({ op: 'media.ack', id: v.getUint32(4, true), ok: true });
    }
  } };
  const code = fs.readFileSync('web-app/js/ble.js', 'utf8').replace(/\/\/ --- Exports ---/, 'globalThis.handleMessage = handleMessage; globalThis.forceDisconnect = onDisconnected; connected = true; rxCharacteristic = RX; // --- Exports ---') + '\nglobalThis.BLE = ProtoTracerBLE;';
  context = vm.createContext({ console, TextEncoder, TextDecoder, setTimeout, clearTimeout, RX: rx, MediaFormat: M, I18N: { t: x => x } });
  vm.runInContext(code, context);
  await context.BLE.mediaRequest('media.stream');
  await context.BLE.mediaTransfer(1, new Uint8Array(500));
  ack = false;
  const pending = context.BLE.mediaTransfer(1, new Uint8Array(500));
  context.forceDisconnect(); await assert.rejects(pending, /disconnected|notConnected/i);
});
test('notify JSON preserves UTF-8 split into individual bytes', () => {
  const code = fs.readFileSync('web-app/js/ble.js', 'utf8').replace(/\/\/ --- Exports ---/, 'globalThis.notify = onTxValueChanged; // --- Exports ---') + '\nglobalThis.BLE = ProtoTracerBLE;';
  const context = vm.createContext({ console, TextEncoder, TextDecoder, setTimeout, clearTimeout, I18N: { t: x => x } });
  vm.runInContext(code, context);
  const messages = [];
  context.BLE.on('received', msg => messages.push(msg));
  const original = { op: 'media.reply', rid: 7, ok: true, clips: [{ name: '我的动画😀.gif' }] };
  for (const byte of new TextEncoder().encode(JSON.stringify(original))) context.notify({ target: { value: Uint8Array.of(byte) } });
  assert.equal(JSON.stringify(messages), JSON.stringify([original]));
});
