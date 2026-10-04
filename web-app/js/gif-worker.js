/* omggif is MIT, vendored with its original license. All GIF work stays on phone. */
importScripts('vendor/omggif.js', 'media-format.js');
self.onmessage = ({ data }) => {
  try {
    const bytes = new Uint8Array(data.buffer);
    if (bytes.length > 16 * 1024 * 1024) throw Error('GIF input exceeds 16 MiB');
    const gif = new GifReader(bytes);
    if (gif.width * gif.height > 4 * 1024 * 1024 || gif.numFrames() > 1000) throw Error('GIF is too large (4M pixels / 1000 frames maximum)');
    if (gif.width * gif.height * gif.numFrames() > 128 * 1024 * 1024) throw Error('GIF exceeds phone decoding budget; shorten it or reduce source resolution');
    const rgba = new Uint8ClampedArray(gif.width * gif.height * 4);
    const source = new OffscreenCanvas(gif.width, gif.height), target = new OffscreenCanvas(64, 32);
    const ctx = source.getContext('2d'), out = target.getContext('2d', { willReadFrequently: true });
    const frames = [];
    let previous = null, saved = null, size = 12;
    // GIF disposal 2 restores the logical background; transparent GIFs use black.
    let bg = [0, 0, 0, 0];
    const first = gif.frameInfo(0), bgIndex = bytes[11];
    if ((bytes[10] & 128) && first.transparent_index !== bgIndex) {
      const pos = 13 + bgIndex * 3;
      bg = [bytes[pos], bytes[pos + 1], bytes[pos + 2], 255];
    }
    for (let i = 0; i < rgba.length; i += 4) rgba.set(bg, i);
    for (let i = 0; i < gif.numFrames(); i++) {
      const frame = gif.frameInfo(i);
      if (frame.x + frame.width > gif.width || frame.y + frame.height > gif.height) throw Error('GIF frame exceeds canvas');
      if (previous?.disposal === 2) {
        for (let y = previous.y; y < previous.y + previous.height; y++) {
          for (let x = previous.x; x < previous.x + previous.width; x++) rgba.set(bg, (y * gif.width + x) * 4);
        }
      } else if (previous?.disposal === 3 && saved) rgba.set(saved);
      saved = frame.disposal === 3 ? rgba.slice() : null;
      gif.decodeAndBlitFrameRGBA(i, rgba);
      ctx.putImageData(new ImageData(rgba, gif.width, gif.height), 0, 0);
      MediaFormat.draw(out, source, gif.width, gif.height, data.fit);
      const encoded = MediaFormat.encode(out.getImageData(0, 0, 64, 32).data, data.quality);
      frames.push({ delay: frame.delay ? frame.delay * 10 : 100, bytes: encoded });
      size += 4 + encoded.length;
      if (size > 1048576) throw Error('Compressed GIF exceeds 1 MiB; shorten it or choose RGB332');
      previous = frame;
      if (i % 10 === 0) self.postMessage({ progress: (i + 1) / gif.numFrames() });
    }
    const clip = MediaFormat.clip(frames);
    self.postMessage({ buffer: clip.buffer, count: frames.length }, [clip.buffer]);
  } catch (error) { self.postMessage({ error: error.message }); }
};
