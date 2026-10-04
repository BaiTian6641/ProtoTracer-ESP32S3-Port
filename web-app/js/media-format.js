/* PTM1 / PM v1 format. Shared by browser, GIF worker and Node tests. */
const MediaFormat = (() => {
  const WIDTH = 64, HEIGHT = 32, PIXELS = WIDTH * HEIGHT;
  function hash(bytes, h = 2166136261) {
    for (const b of bytes) h = Math.imul(h ^ b, 16777619) >>> 0;
    return h;
  }
  function encode(rgba, quality = '332') {
    if (rgba.length !== PIXELS * 4) throw Error('Expected 64×32 RGBA');
    const stride = quality === '565' ? 2 : 1;
    const raw = new Uint8Array(PIXELS * stride);
    for (let i = 0; i < PIXELS; i++) {
      // Composite alpha over black; canvas sources are already composited.
      const a = rgba[i * 4 + 3] / 255;
      const r = Math.round(rgba[i * 4] * a), g = Math.round(rgba[i * 4 + 1] * a), b = Math.round(rgba[i * 4 + 2] * a);
      const c = stride === 1 ? (r & 224) | ((g >> 3) & 28) | (b >> 6)
        : ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
      raw[i * stride] = c; if (stride === 2) raw[i * stride + 1] = c >> 8;
    }
    const rle = [];
    for (let i = 0; i < PIXELS;) {
      let run = 1;
      while (run < 255 && i + run < PIXELS && raw[(i + run) * stride] === raw[i * stride]
        && (stride === 1 || raw[(i + run) * stride + 1] === raw[i * stride + 1])) run++;
      rle.push(run, raw[i * stride]); if (stride === 2) rle.push(raw[i * stride + 1]);
      i += run;
    }
    const compressed = rle.length < raw.length;
    const out = new Uint8Array(1 + (compressed ? rle.length : raw.length));
    out[0] = (stride === 2 ? 2 : 0) + (compressed ? 1 : 0);
    out.set(compressed ? rle : raw, 1);
    return out;
  }
  function decode(bytes) {
    if (!bytes.length || bytes[0] > 3) throw Error('Invalid codec');
    const stride = bytes[0] < 2 ? 1 : 2, rle = bytes[0] & 1;
    const rgba = new Uint8ClampedArray(PIXELS * 4);
    let p = 1, i = 0;
    while (p < bytes.length) {
      const n = rle ? bytes[p++] : 1;
      if (!n || p + stride > bytes.length || i + n > PIXELS) throw Error('Invalid frame');
      const c = bytes[p++] | (stride === 2 ? bytes[p++] << 8 : 0);
      const r = stride === 1 ? Math.floor((c >> 5) * 255 / 7) : Math.floor((c >> 11) * 255 / 31);
      const g = stride === 1 ? Math.floor(((c >> 2) & 7) * 255 / 7) : Math.floor(((c >> 5) & 63) * 255 / 63);
      const b = stride === 1 ? (c & 3) * 85 : Math.floor((c & 31) * 255 / 31);
      for (let j = 0; j < n; j++, i++) rgba.set([r, g, b, 255], i * 4);
    }
    if (i !== PIXELS) throw Error('Incomplete frame');
    return rgba;
  }
  function clip(frames) {
    if (!frames.length || frames.length > 1000) throw Error('GIF needs 1–1000 frames');
    const out = new Uint8Array(12 + frames.reduce((s, f) => s + 4 + f.bytes.length, 0));
    if (out.length > 1048576) throw Error('Compressed GIF exceeds 1 MiB; shorten it or use RGB332');
    out.set([80, 84, 77, 49, 64, 32]);
    const view = new DataView(out.buffer); view.setUint16(6, frames.length, true);
    let offset = 12;
    for (const f of frames) {
      view.setUint16(offset, Math.max(20, Math.min(65535, f.delay)), true);
      view.setUint16(offset + 2, f.bytes.length, true);
      out.set(f.bytes, offset + 4); offset += f.bytes.length + 4;
    }
    return out;
  }
  function frameTransfer(frame) {
    const out = new Uint8Array(frame.length + 4); out.set(frame);
    new DataView(out.buffer).setUint32(frame.length, hash(frame), true); return out;
  }
  function fragment(kind, id, total, offset, payload) {
    const out = new Uint8Array(16 + payload.length);
    out.set([80, 77, 1, kind]); const v = new DataView(out.buffer);
    v.setUint32(4, id, true); v.setUint32(8, offset, true); v.setUint32(12, total, true);
    out.set(payload, 16); return out;
  }
  function draw(ctx, source, width, height, fit = 'stretch', mirror = false, rotation = 0) {
    const angle = Number(rotation);
    if (![0, 90, 180, 270].includes(angle) || width <= 0 || height <= 0) throw Error('Invalid source orientation');
    const sideways = angle === 90 || angle === 270;
    const orientedWidth = sideways ? height : width, orientedHeight = sideways ? width : height;
    const scale = fit === 'cover' ? Math.max(WIDTH / orientedWidth, HEIGHT / orientedHeight) : Math.min(WIDTH / orientedWidth, HEIGHT / orientedHeight);
    ctx.fillStyle = '#000'; ctx.fillRect(0, 0, WIDTH, HEIGHT); ctx.save();
    ctx.translate(WIDTH / 2, HEIGHT / 2);
    if (mirror) ctx.scale(-1, 1);
    ctx.scale(fit === 'stretch' ? WIDTH / orientedWidth : scale, fit === 'stretch' ? HEIGHT / orientedHeight : scale);
    ctx.rotate(angle * Math.PI / 180);
    ctx.drawImage(source, -width / 2, -height / 2, width, height);
    ctx.restore();
  }
  return { WIDTH, HEIGHT, PIXELS, hash, encode, decode, clip, frameTransfer, fragment, draw };
})();
if (typeof module !== 'undefined') module.exports = MediaFormat;
