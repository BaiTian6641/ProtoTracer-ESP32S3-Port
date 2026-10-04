/* Capture/encode/upload stays in the browser; firmware consumes tiny PTM frames. */
(() => {
  const $ = id => document.getElementById(id);
  const canvas = $('mediaPreview'), ctx = canvas.getContext('2d', { willReadFrequently: true });
  const words = {
    title: ['Projection & GIF · 64×32', '投屏与 GIF · 64×32'], color: ['Color', '色彩'], fit: ['Fit', '缩放'],
    stretch: ['Fill landscape 64×32', '填满横屏 64×32'], rotation: ['Rotation', '旋转'],
    contain: ['Full image', '完整画面'], cover: ['Fill / crop', '填满裁切'], mirror: ['Mirror', '镜像'],
    camera: ['Cast camera', '摄像头投屏'], screen: ['Share screen', '共享屏幕'], video: ['Cast image / video', '图片 / 视频投屏'],
    stop: ['Stop / restore face', '停止并恢复表情'], slot: ['GIF slot', 'GIF 槽位'], gif: ['Choose GIF', '选择 GIF'],
    upload: ['Upload to device', '上传到设备'], refresh: ['Refresh clips', '刷新动画列表'],
    playlist: ['Animation playlist', '动画播放列表'], play: ['Play selected', '播放所选动画'], delete: ['Delete selected GIF', '删除所选 GIF'],
    hint: ['Camera, images and video are processed on your phone. Screen sharing depends on browser support. Keep this page in the foreground.', '摄像头、图片和视频在手机上处理；共享屏幕取决于浏览器支持。保持此页面在前台。']
  };
  const say = (en, zh) => I18N.getLang() === 'zh' ? zh : en;
  let supported = false, busy = false, uploading = false, cancelUpload = false;
  let generation = 0, streamTask = null, capture = null, source = null, objectURL = null, worker = null;
  let prepared = null, preparedName = '', slots = 8, list = null;
  function status(text) { $('mediaStatus').textContent = text; }
  function language() {
    document.querySelectorAll('[data-media-text]').forEach(el => { el.textContent = words[el.dataset.mediaText][I18N.getLang() === 'zh' ? 1 : 0]; });
    $('mediaSupport').textContent = supported ? say('Media firmware v1 connected.', '媒体固件 v1 已连接。')
      : say('Connect a device with firmware 1.3.0 or later to use media.', '连接固件 1.3.0 或以上的设备后可使用。');
    if (list) renderLibrary(list);
  }
  new MutationObserver(language).observe(document.documentElement, { attributes: true, attributeFilter: ['lang'] });
  function setBusy(value) {
    busy = value;
    for (const id of ['mediaCamera', 'mediaScreen', 'mediaSourceFile', 'mediaRefresh', 'mediaGIF', 'mediaSlot', 'mediaQuality', 'mediaFit', 'mediaRotation', 'mediaPlaylist']) $(id).disabled = value;
    $('mediaUpload').disabled = value || !prepared;
    updatePlaylistActions();
  }
  function releaseSource() {
    if (capture) capture.getTracks().forEach(t => t.stop());
    capture = null;
    if (source instanceof HTMLVideoElement) { source.pause(); source.srcObject = null; source.removeAttribute('src'); source.load(); }
    source = null;
    if (objectURL) URL.revokeObjectURL(objectURL);
    objectURL = null;
  }
  async function endLocal() {
    ++generation; releaseSource();
    const task = streamTask; streamTask = null;
    if (task) await task.catch(() => {});
  }
  async function action(fn) {
    if (busy || !supported || !ProtoTracerBLE.isConnected()) return;
    setBusy(true);
    try { await fn(); } catch (error) { status(error.message); }
    finally { setBusy(false); }
  }
  async function refresh() {
    list = await ProtoTracerBLE.mediaRequest('media.list');
    slots = list.slots;
    const selected = $('mediaSlot').value;
    $('mediaSlot').replaceChildren(...Array.from({ length: slots }, (_, i) => new Option(`GIF ${i + 1}`, i)));
    if (selected !== '') $('mediaSlot').value = selected;
    renderLibrary(list);
  }
  function renderLibrary(data) {
    const select = $('mediaPlaylist'), selected = select.value;
    select.replaceChildren();
    const defaults = document.createElement('optgroup'); defaults.label = say('Built-in clips', '内置动画');
    for (const name of data.builtins || []) {
      defaults.append(new Option(name === 'badapple' ? 'Bad Apple' : name, 'builtin:' + name));
    }
    if (defaults.children.length) select.append(defaults);
    const uploaded = document.createElement('optgroup'); uploaded.label = say('Uploaded GIFs', '已上传 GIF');
    for (const clip of data.clips || []) {
      const label = `GIF ${clip.slot + 1}${clip.name ? ' · ' + clip.name : ''} · ${(clip.bytes / 1024).toFixed(1)} KB`;
      uploaded.append(new Option(label, 'gif:' + clip.slot));
    }
    if (uploaded.children.length) select.append(uploaded);
    if (Array.from(select.options).some(o => o.value === selected)) select.value = selected;
    updatePlaylistActions();
    status(`${say('Flash available', 'Flash 可用')} ${(data.free_bytes / 1024).toFixed(0)} KB · ${say('mode', '模式')}: ${data.mode}`);
  }
  function updatePlaylistActions() {
    const value = $('mediaPlaylist').value;
    $('mediaPlay').disabled = busy || !value;
    $('mediaDelete').disabled = busy || !value.startsWith('gif:');
  }
  $('mediaPlaylist').onchange = updatePlaylistActions;
  $('mediaPlay').onclick = () => action(async () => {
    const [type, value] = $('mediaPlaylist').value.split(':');
    if (!value) return;
    await endLocal();
    await ProtoTracerBLE.mediaRequest('media.play', type === 'builtin' ? { builtin: value } : { slot: Number(value) });
    status(say('Playing on device. Bluetooth can disconnect.', '设备正在播放，可断开蓝牙。'));
  });
  $('mediaDelete').onclick = () => action(async () => {
    const [type, value] = $('mediaPlaylist').value.split(':');
    if (type !== 'gif') return;
    await ProtoTracerBLE.mediaRequest('media.delete', { slot: Number(value) }); await refresh();
  });
  async function videoReady(video) {
    if (!video.videoWidth) await new Promise((resolve, reject) => {
      video.onloadedmetadata = resolve; video.onerror = () => reject(Error('Cannot open video'));
    });
    video.muted = true; video.playsInline = true; await video.play();
  }
  async function startCast(nextSource) {
    source = nextSource;
    await ProtoTracerBLE.mediaRequest('media.stream');
    const token = ++generation;
    streamTask = (async () => {
      let count = 0, sent = 0, started = performance.now();
      while (generation === token && source && ProtoTracerBLE.isConnected()) {
        if (document.hidden) break;
        const began = performance.now();
        const width = source.videoWidth || source.naturalWidth, height = source.videoHeight || source.naturalHeight;
        if (!width || !height) throw Error('Capture has no image');
        MediaFormat.draw(ctx, source, width, height, $('mediaFit').value, $('mediaMirror').checked, $('mediaRotation').value);
        const frame = MediaFormat.encode(ctx.getImageData(0, 0, 64, 32).data, $('mediaQuality').value);
        // Show the exact quantized image sent to the panels.
        ctx.putImageData(new ImageData(MediaFormat.decode(frame), 64, 32), 0, 0);
        await ProtoTracerBLE.mediaTransfer(1, MediaFormat.frameTransfer(frame));
        count++; sent += frame.length;
        const elapsed = performance.now() - started;
        if (elapsed >= 1000) {
          status(`${(count * 1000 / elapsed).toFixed(1)} FPS · ${(sent / elapsed).toFixed(1)} KB/s · ${say('face hidden', '表情已隐藏')}`);
          count = sent = 0; started = performance.now();
        }
        await new Promise(resolve => setTimeout(resolve, Math.max(0, 1000 / Number($('mediaFPS').value) - (performance.now() - began))));
      }
    })().catch(error => { status(error.message); }).finally(() => {
      if (token === generation) { ++generation; releaseSource(); }
    });
  }
  $('mediaCamera').onclick = () => action(async () => {
    await endLocal();
    capture = await navigator.mediaDevices.getUserMedia({ video: { width: 320, height: 240, facingMode: 'environment' }, audio: false });
    const v = document.createElement('video'); v.srcObject = capture; source = v;
    try { await videoReady(v); await startCast(v); } catch (error) { releaseSource(); throw error; }
  });
  $('mediaScreen').onclick = () => {
    if (busy || !supported) return;
    if (!navigator.mediaDevices?.getDisplayMedia) { status(say('This browser cannot share the screen. Use camera / video, or desktop Chrome.', '此浏览器无法共享屏幕，请使用摄像头、视频或桌面 Chrome。')); return; }
    // Must be invoked directly from the user's click, before any await.
    const request = navigator.mediaDevices.getDisplayMedia({ video: true, audio: false });
    request.catch(() => {});
    action(async () => {
      await endLocal(); capture = await request;
      const v = document.createElement('video'); v.srcObject = capture; source = v;
      capture.getVideoTracks()[0].onended = () => $('mediaStop').click();
      try { await videoReady(v); await startCast(v); } catch (error) { releaseSource(); throw error; }
    });
  };
  $('mediaSourceFile').onchange = event => {
    const file = event.target.files[0]; event.target.value = '';
    if (!file) return;
    action(async () => {
      await endLocal(); objectURL = URL.createObjectURL(file);
      try {
        if (file.type.startsWith('video/')) {
          const v = document.createElement('video'); source = v; v.src = objectURL; v.loop = true;
          await videoReady(v); await startCast(v);
        } else {
          const image = new Image(); source = image; image.src = objectURL; await image.decode(); await startCast(image);
        }
      } catch (error) { releaseSource(); throw error; }
    });
  };
  $('mediaStop').onclick = async () => {
    cancelUpload = true;
    await endLocal();
    if (!uploading && ProtoTracerBLE.isConnected()) {
      try { await ProtoTracerBLE.mediaRequest('media.stop'); status(say('Face restored.', '已恢复表情。')); }
      catch (error) { status(error.message); }
    }
  };
  $('mediaRefresh').onclick = () => action(refresh);
  $('mediaGIF').onchange = event => {
    const file = event.target.files[0]; event.target.value = '';
    if (!file) return;
    action(async () => {
      await endLocal(); await ProtoTracerBLE.mediaRequest('media.stop');
      if (file.size > 16 * 1024 * 1024) throw Error(say('GIF input exceeds 16 MiB.', 'GIF 原文件超过 16 MiB。'));
      prepared = null; $('mediaGIFInfo').textContent = file.name;
      if (worker) worker.terminate();
      worker = new Worker('js/gif-worker.js');
      $('mediaProgress').hidden = false; $('mediaProgress').value = 0;
      try {
        const buffer = await file.arrayBuffer();
        const result = await new Promise((resolve, reject) => {
          worker.onmessage = ({ data }) => {
            if (data.error) reject(Error(data.error));
            else if (data.buffer) resolve(data);
            else $('mediaProgress').value = data.progress;
          };
          worker.onerror = e => reject(Error(e.message));
          worker.postMessage({ buffer, quality: $('mediaQuality').value, fit: $('mediaFit').value, rotation: Number($('mediaRotation').value) }, [buffer]);
        });
        prepared = new Uint8Array(result.buffer);
        preparedName = file.name;
        const firstSize = new DataView(prepared.buffer).getUint16(14, true);
        ctx.putImageData(new ImageData(MediaFormat.decode(prepared.subarray(16, 16 + firstSize)), 64, 32), 0, 0);
        $('mediaGIFInfo').textContent = `${file.name} · ${result.count} ${say('frames', '帧')} · ${(prepared.length / 1024).toFixed(1)} KB · 64×32`;
      } finally { worker.terminate(); worker = null; $('mediaProgress').hidden = true; }
    });
  };
  $('mediaUpload').onclick = () => action(async () => {
    await endLocal();
    uploading = true; cancelUpload = false;
    const bytes = prepared;
    $('mediaProgress').hidden = false;
    let finished = false;
    try {
      await ProtoTracerBLE.mediaRequest('media.upload.begin', { slot: Number($('mediaSlot').value), name: preparedName, size: bytes.length, hash: MediaFormat.hash(bytes) });
      for (let offset = 0; offset < bytes.length;) {
        if (cancelUpload) throw Error(say('Upload cancelled.', '上传已取消。'));
        const block = bytes.subarray(offset, offset + 4096);
        const ack = await ProtoTracerBLE.mediaTransfer(2, block);
        offset += block.length;
        if (ack.offset !== offset) throw Error('Upload offset mismatch');
        $('mediaProgress').value = offset / bytes.length;
        status(`${say('Uploading', '上传中')} ${Math.round(100 * offset / bytes.length)}%`);
      }
      if (cancelUpload) throw Error('Upload cancelled');
      await ProtoTracerBLE.mediaRequest('media.upload.end'); finished = true; await refresh();
      $('mediaPlaylist').value = 'gif:' + $('mediaSlot').value;
      updatePlaylistActions();
    } finally {
      if (!finished && ProtoTracerBLE.isConnected()) await ProtoTracerBLE.mediaRequest('media.upload.cancel').catch(() => {});
      uploading = false; $('mediaProgress').hidden = true;
    }
  });
  ProtoTracerBLE.on('manifest', manifest => {
    supported = manifest.media?.version === 1; $('mediaControls').hidden = !supported;
    language(); if (supported) action(refresh);
  });
  ProtoTracerBLE.on('disconnected', () => {
    supported = false; cancelUpload = true; endLocal(); $('mediaControls').hidden = true; language();
  });
  document.addEventListener('visibilitychange', () => { if (document.hidden && source) $('mediaStop').click(); });
  window.addEventListener('pagehide', () => { ++generation; releaseSource(); if (worker) worker.terminate(); });
  language();
})();
