/* Real Chromium canvas/worker/UI tests. The BLE radio is mocked here;
 * tools/media_ble_test.py separately exercises the actual board over BLE. */
const fs = require('node:fs'), http = require('node:http'), path = require('node:path');
const assert = require('node:assert/strict');
const { GifWriter } = require('./js/vendor/omggif.js');
const M = require('./js/media-format.js');
const { chromium } = require('playwright');
async function main() {
  const tmp = Buffer.alloc(4096), writer = new GifWriter(tmp, 8, 4, { palette: [0, 0xff0000, 0x00ff00, 0x0000ff], loop: 0 });
  writer.addFrame(0, 0, 8, 4, new Uint8Array(32).fill(1), { delay: 5, disposal: 1 });
  writer.addFrame(0, 0, 4, 4, new Uint8Array(16).fill(2), { delay: 10, disposal: 3 });
  writer.addFrame(4, 0, 4, 4, new Uint8Array(16).fill(3), { delay: 20, disposal: 2 });
  writer.addFrame(0, 0, 1, 1, new Uint8Array([2]), { delay: 2, disposal: 1 });
  const gif = tmp.subarray(0, writer.end());
  const root = path.resolve('web-app');
  const server = http.createServer((req, res) => {
    const file = path.join(root, decodeURIComponent(req.url.split('?')[0] === '/' ? '/index.html' : req.url.split('?')[0]));
    if (!file.startsWith(root + path.sep)) { res.writeHead(403).end(); return; }
    try {
      res.setHeader('Content-Type', file.endsWith('.js') ? 'text/javascript' : file.endsWith('.css') ? 'text/css' : 'text/html');
      res.end(fs.readFileSync(file));
    } catch { res.writeHead(404).end(); }
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const url = `http://127.0.0.1:${server.address().port}/`;
  let browser;
  try {
    browser = await chromium.launch({ channel: 'msedge', headless: true, args: ['--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream'] });
    const page = await browser.newPage({ viewport: { width: 390, height: 844 } });
    const errors = []; page.on('pageerror', e => errors.push(e.message));
    await page.route('**/js/ble.js', route => route.fulfill({ contentType: 'text/javascript', body: `
      const ProtoTracerBLE = (() => {
        const listeners = {}, clips = []; let mode = 'face', expected = 0, received = 0;
        const emit = (op, data) => (listeners[op] || []).forEach(fn => fn(data));
        globalThis.fakeConnect = () => { emit('connected', {name:'Test'}); emit('manifest', {device:{display_name:'Test'}, visual:{expression_names:['Normal','Happy'],expression_count:2}, media:{version:1}}); };
        globalThis.fakeDisconnect = () => emit('disconnected', {});
        globalThis.sentMedia = [];
        globalThis.streamCount = 0;
        return { on(op,fn){ (listeners[op] ||= []).push(fn); }, isSupported:()=>true, isConnected:()=>true,
          getManifest:()=>null, sendCommand:async()=>{}, controlSet:async()=>{},
          mediaRequest:async(op,fields={})=> {
            sentMedia.push({op,...fields});
            if(op==='media.upload.begin'){ expected=fields.size;received=0; }
            if(op==='media.upload.end'){ if(received!==expected)throw Error('size');clips.push({slot:0,bytes:received}); }
            if(op==='media.delete')clips.splice(0);
            if(op==='media.stop')mode='face';
            if(op==='media.play')mode=fields.builtin?'builtin':'gif';
            return {ok:true,slots:8,mode,free_bytes:1000000,builtins:['wave','hex','orb'],clips};
          }, mediaTransfer:async(kind,bytes)=> {if(kind===1)streamCount++;else received+=bytes.length;return{ok:true,offset:received};}
        };
      })();` }));
    await page.goto(url);
    await page.evaluate(() => { localStorage.setItem('prototracer-lang', 'zh'); });
    await page.reload();
    await page.evaluate(() => fakeConnect());
    await page.waitForFunction(() => document.querySelectorAll('#mediaLibrary > div').length === 3);
    const result = await page.evaluate(async buffer => {
      const worker = new Worker('js/gif-worker.js');
      const bytes = Uint8Array.from(buffer);
      const message = await new Promise((resolve, reject) => {
        worker.onmessage = ({data}) => { if(data.error)reject(Error(data.error));else if(data.buffer)resolve(data); };
        worker.postMessage({buffer:bytes.buffer,quality:'332',fit:'contain'}, [bytes.buffer]);
      });
      worker.terminate();
      const clip = new Uint8Array(message.buffer), view = new DataView(clip.buffer);
      const colors = [], delays = []; let offset = 12;
      for(let i=0;i<view.getUint16(6,true);i++) {
        const n = view.getUint16(offset+2,true), rgba = MediaFormat.decode(clip.subarray(offset+4,offset+4+n));
        colors.push([Array.from(rgba.subarray((16*64+8)*4,(16*64+8)*4+3)), Array.from(rgba.subarray((16*64+56)*4,(16*64+56)*4+3))]);
        delays.push(view.getUint16(offset,true)); offset+=4+n;
      }
      return { colors, delays };
    }, Array.from(gif));
    assert.deepEqual(result.colors, [ [[255,0,0],[255,0,0]], [[0,255,0],[255,0,0]], [[255,0,0],[0,0,255]], [[255,0,0],[0,0,0]] ]);
    assert.deepEqual(result.delays, [50,100,200,20]);
    await page.locator('#mediaCamera').click();
    await page.waitForFunction(() => streamCount >= 3);
    await page.locator('#mediaStop').click();
    await page.waitForFunction(() => document.querySelector('#mediaStatus').textContent.includes('恢复'));
    await page.locator('#mediaGIF').setInputFiles({ name: 'disposal.gif', mimeType: 'image/gif', buffer: gif });
    await page.waitForFunction(() => !document.querySelector('#mediaUpload').disabled);
    await page.locator('#mediaUpload').click();
    await page.waitForFunction(() => document.querySelectorAll('#mediaLibrary > div').length === 4 && !document.querySelector('#mediaRefresh').disabled);
    await page.locator('#mediaLibrary > div').last().getByRole('button', { name: '播放' }).click();
    await page.waitForFunction(() => sentMedia.some(m=>m.op==='media.play' && m.slot===0));
    await page.locator('#mediaStop').click();
    await page.waitForFunction(() => document.querySelector('#mediaStatus').textContent.includes('恢复'));
    await page.locator('#mediaLibrary > div').last().getByRole('button', { name: '删除' }).click();
    await page.waitForFunction(() => document.querySelectorAll('#mediaLibrary > div').length===3);
    await page.locator('#btnLangToggle').click();
    await page.waitForFunction(() => document.querySelector('#mediaStop').textContent==='Stop / restore face');
    assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false);
    fs.mkdirSync('build/media-browser', {recursive:true});
    await page.screenshot({path:'build/media-browser/phone.png',fullPage:true});
    await page.evaluate(() => fakeDisconnect());
    assert.equal(await page.locator('#mediaControls').isVisible(), false);
    assert.deepEqual(errors, []);
    console.log('PASS Chromium: camera capture/stream, GIF disposal 1/2/3, frame timing, worker conversion, upload/play/delete, stop, languages, phone layout, disconnect');
  } finally { if (browser) await browser.close(); server.close(); }
}
main().catch(error => { console.error(error); process.exitCode=1; });
