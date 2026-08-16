/**
 * ProtoTracer Remote - Application Controller
 *
 * Manages UI state, user interactions, and bridges BLE events to the DOM.
 * All user-visible strings go through I18N.t().
 */

(function () {
  'use strict';

  // --- DOM References ---
  const $ = (sel) => document.querySelector(sel);
  const $$ = (sel) => document.querySelectorAll(sel);

  const dom = {
    statusDot:        $('#statusDot'),
    statusText:       $('#statusText'),
    deviceName:       $('#deviceName'),
    batteryBadge:     $('#batteryBadge'),
    btnLangToggle:    $('#btnLangToggle'),
    connectPanel:     $('#connectPanel'),
    controlPanel:     $('#controlPanel'),
    btnScan:          $('#btnScan'),
    btnManualConnect: $('#btnManualConnect'),
    manualDeviceName: $('#manualDeviceName'),
    scanStatus:       $('#scanStatus'),
    deviceList:       $('#deviceList'),
    bleSupportInfo:   $('#bleSupportInfo'),
    expressionGrid:   $('#expressionGrid'),
    exprCount:        $('#exprCount'),
    brightnessSlider: $('#brightnessSlider'),
    brightnessValue:  $('#brightnessValue'),
    hueSlider:        $('#hueSlider'),
    hueValue:         $('#hueValue'),
    voiceToggle:      $('#voiceToggle'),
    displayModeToggle:$('#displayModeToggle'),
    btnPing:          $('#btnPing'),
    btnRefreshManifest:$('#btnRefreshManifest'),
    btnDisconnect:    $('#btnDisconnect'),
    btnClearLog:      $('#btnClearLog'),
    logOutput:        $('#logOutput'),
    toastContainer:   $('#toastContainer'),
  };

  // --- Application State ---
  let state = {
    expressions:       [],
    currentExpression: 0,
    brightness:        105,
    hue:               0.0,
    baseColor:         null, // { r, g, b, hue } from manifest visual.red/green/blue; null = legacy firmware
    hueEchoReceived:   false, // true once a control.state hue_shift echo has arrived this session
    voiceEnabled:      true,
    displayMode:       0,
    connected:         false,
  };

  // --- Initialization ---
  function init() {
    I18N.applyToDOM(document.body);
    updateLangToggleLabel();
    checkBLESupport();
    updateHueBadge();

    // BLE events
    ProtoTracerBLE.on('status',       onStatusChange);
    ProtoTracerBLE.on('connected',    onConnected);
    ProtoTracerBLE.on('disconnected', onDisconnected);
    ProtoTracerBLE.on('manifest',     onManifestReceived);
    ProtoTracerBLE.on('controlState', onControlState);
    ProtoTracerBLE.on('pong',         onPongReceived);
    ProtoTracerBLE.on('sent',         onCommandSent);
    ProtoTracerBLE.on('received',     onDataReceived);

    // UI events
    dom.btnScan.addEventListener('click', onScanClick);
    dom.btnManualConnect.addEventListener('click', onManualConnectClick);
    dom.brightnessSlider.addEventListener('input', onBrightnessInput);
    dom.brightnessSlider.addEventListener('change', onBrightnessChange);
    dom.hueSlider.addEventListener('input', onHueInput);
    dom.hueSlider.addEventListener('change', onHueChange);
    dom.voiceToggle.addEventListener('change', onVoiceToggle);
    dom.displayModeToggle.addEventListener('change', onDisplayModeToggle);
    dom.btnPing.addEventListener('click', onPingClick);
    dom.btnRefreshManifest.addEventListener('click', onRefreshManifestClick);
    dom.btnDisconnect.addEventListener('click', onDisconnectClick);
    dom.btnClearLog.addEventListener('click', () => { dom.logOutput.innerHTML = ''; });
    dom.btnLangToggle.addEventListener('click', onLangToggle);

    // Hue preset buttons
    $$('.hue-btn').forEach(btn => {
      btn.addEventListener('click', () => {
        const hue = parseInt(btn.dataset.hue, 10);
        dom.hueSlider.value = hue;
        onHueChange();
      });
    });

    if (ProtoTracerBLE.isSupported()) {
      dom.btnScan.disabled = false;
    }

    addLog('info', I18N.t('log.ready'));
  }

  // --- Language Toggle ---
  function onLangToggle() {
    const next = I18N.getLang() === 'en' ? 'zh' : 'en';
    I18N.setLang(next);
    I18N.applyToDOM(document.body);
    updateLangToggleLabel();

    // Refresh dynamic strings
    if (state.expressions.length > 0) {
      dom.exprCount.textContent = I18N.t('ctrl.exprCount', { n: state.expressions.length });
    }
    dom.statusText.textContent = state.connected
      ? I18N.t('status.connected')
      : I18N.t('status.disconnected');
  }

  function updateLangToggleLabel() {
    dom.btnLangToggle.textContent = I18N.getLang() === 'en' ? '中' : 'EN';
  }

  // --- BLE Support Check ---
  function checkBLESupport() {
    if (ProtoTracerBLE.isSupported()) {
      dom.bleSupportInfo.className = 'compat-info supported';
      dom.bleSupportInfo.textContent = I18N.t('ble.supported');
      dom.btnScan.disabled = false;
      dom.btnManualConnect.disabled = false;
    } else {
      dom.bleSupportInfo.className = 'compat-info unsupported';
      dom.bleSupportInfo.textContent = I18N.t('ble.unsupported');
    }
  }

  // --- Event Handlers: BLE ---

  function onStatusChange(data) {
    dom.statusText.textContent = data.message || '';
    dom.statusDot.className = 'dot ' + (data.state || '');

    if (data.state === 'scanning') {
      dom.scanStatus.textContent = I18N.t('connect.scanHint');
    } else {
      dom.scanStatus.textContent = '';
    }
  }

  function onConnected(data) {
    state.connected = true;
    dom.deviceName.textContent = data.name || 'ProtoTracer';
    dom.deviceName.parentElement.classList.remove('hidden');
    dom.connectPanel.classList.add('hidden');
    dom.controlPanel.classList.remove('hidden');
    addLog('success', I18N.t('log.connected', { name: data.name || 'ProtoTracer' }));
    showToast(I18N.t('ble.connectedToast', { name: data.name || 'ProtoTracer' }), 'success');
  }

  function onDisconnected(data) {
    state.connected = false;
    dom.deviceName.parentElement.classList.add('hidden');
    dom.controlPanel.classList.add('hidden');
    dom.connectPanel.classList.remove('hidden');
    addLog('warn', data.unexpected ? I18N.t('log.disconnectedUnexp') : I18N.t('log.disconnected'));
    if (data.unexpected) showToast(I18N.t('log.disconnectedToast'), 'warning');
  }

  function onManifestReceived(manifest) {
    addLog('success', I18N.t('log.manifestRcvd'));

    const visual = manifest.visual || {};
    const deviceInfo = manifest.device || {};

    if (visual.expression_names && visual.expression_names.length > 0) {
      state.expressions = visual.expression_names.map((name, i) => ({ name, index: i }));
    } else if (visual.expression_count) {
      state.expressions = Array.from({ length: visual.expression_count }, (_, i) => ({
        name: I18N.t('log.exprFallback', { n: i + 1 }),
        index: i,
      }));
    }

    dom.exprCount.textContent = I18N.t('ctrl.exprCount', { n: state.expressions.length });
    renderExpressionGrid();

    if (deviceInfo.display_name) {
      dom.deviceName.textContent = deviceInfo.display_name;
    }

    addLog('info', I18N.t('log.animation', { name: visual.animation_name || 'Unknown', n: state.expressions.length }));

    // Device base color: hue controls become TARGET hue relative to this base.
    if (Number.isFinite(visual.red) && Number.isFinite(visual.green) && Number.isFinite(visual.blue)) {
      const r = Math.min(255, Math.max(0, visual.red));
      const g = Math.min(255, Math.max(0, visual.green));
      const b = Math.min(255, Math.max(0, visual.blue));
      state.baseColor = { r: r, g: g, b: b, hue: rgbToHueDeg(r, g, b) };
      addLog('info', I18N.t('log.baseColor', { r: r, g: g, b: b, h: Math.round(state.baseColor.hue) }));
      if (!state.hueEchoReceived) {
        // Firmware hue shift boots at 0 (not persisted), so the face currently shows
        // the base color: point the wheel/badge at it until the first echo says otherwise.
        state.hue = norm360(state.baseColor.hue);
        dom.hueSlider.value = Math.round(state.hue);
        dom.hueValue.textContent = Math.round(state.hue) + '\u00B0';
      }
    } else {
      state.baseColor = null; // older firmware: no base color, raw hue_shift semantics
    }
    updateHueBadge(); // refresh tint only; never resend anything to the device
  }

  function onControlState(msg) {
    if (msg.expression !== undefined) state.currentExpression = msg.expression;
    if (msg.brightness !== undefined) {
      state.brightness = msg.brightness;
      dom.brightnessSlider.value = msg.brightness;
      dom.brightnessValue.textContent = msg.brightness;
    }
    if (msg.voice_enabled !== undefined) {
      state.voiceEnabled = msg.voice_enabled;
      dom.voiceToggle.checked = msg.voice_enabled;
    }
    if (msg.display_mode !== undefined) {
      state.displayMode = msg.display_mode;
      dom.displayModeToggle.checked = msg.display_mode !== 0;
    }
    if (msg.hue_shift !== undefined) {
      // Echo S is the RAW shift; show the target hue it produces (S + base hue).
      state.hueEchoReceived = true;
      const displayed = state.baseColor ? norm360(msg.hue_shift + state.baseColor.hue) : msg.hue_shift;
      state.hue = displayed;
      dom.hueSlider.value = Math.round(displayed);
      dom.hueValue.textContent = Math.round(displayed) + '\u00B0';
      updateHueBadge();
    }

    highlightExpression(state.currentExpression);
  }

  function onPongReceived(msg) {
    addLog('success', I18N.t('log.pong', { name: msg.name || 'device' }));
    showToast(I18N.t('log.pongToast'), 'success');
  }

  function onCommandSent(data) {
    addLog('data', I18N.t('log.sent', { op: data.op, n: data.payload.length }));
  }

  function onDataReceived(data) {
    if (data.op === 'control.state' || data.op === 'pong') return;
    const preview = JSON.stringify(data).substring(0, 120);
    addLog('data', I18N.t('log.recv', { preview: preview + (preview.length >= 120 ? '\u2026' : '') }));
  }

  // --- Event Handlers: UI ---

  async function onScanClick() {
    try {
      const dev = await ProtoTracerBLE.scan();
      if (dev) { await ProtoTracerBLE.connect(); }
    } catch (err) {
      addLog('error', I18N.t('err.scanFailed', { msg: err.message }));
      showToast(I18N.t('err.scanToast', { msg: err.message }), 'error');
    }
  }

  async function onManualConnectClick() {
    const nameFilter = dom.manualDeviceName.value.trim();
    if (!nameFilter) {
      showToast(I18N.t('err.noDeviceName'), 'warning');
      return;
    }

    try {
      const btDevice = await navigator.bluetooth.requestDevice({
        filters: [{ namePrefix: nameFilter }],
        optionalServices: [ProtoTracerBLE.SERVICE_UUID],
      });

      if (btDevice) {
        addLog('info', I18N.t('log.found', { name: btDevice.name || nameFilter }));
        await ProtoTracerBLE.connectToDevice(btDevice);
      }
    } catch (err) {
      addLog('error', I18N.t('err.manualFailed', { msg: err.message }));
      showToast(I18N.t('err.connectToast', { msg: err.message }), 'error');
    }
  }

  function onBrightnessInput() {
    dom.brightnessValue.textContent = dom.brightnessSlider.value;
  }

  async function onBrightnessChange() {
    const val = parseInt(dom.brightnessSlider.value, 10);
    state.brightness = val;
    dom.brightnessValue.textContent = val;
    if (state.connected) {
      try {
        await ProtoTracerBLE.controlSet({ brightness: val });
      } catch (e) {
        addLog('error', I18N.t('err.brightness', { msg: e.message }));
      }
    }
  }

  // --- Hue helpers (target hue relative to device base color) ---

  function norm360(x) {
    return ((x % 360) + 360) % 360;
  }

  // Standard RGB->HSV hue in degrees (0-360). Grayscale input returns 0.
  function rgbToHueDeg(r, g, b) {
    const rn = r / 255, gn = g / 255, bn = b / 255;
    const max = Math.max(rn, gn, bn), min = Math.min(rn, gn, bn);
    const d = max - min;
    if (d === 0) return 0;
    let h;
    if (max === rn)      h = ((gn - bn) / d) % 6;
    else if (max === gn) h = (bn - rn) / d + 2;
    else                 h = (rn - gn) / d + 4;
    return norm360(h * 60);
  }

  // Simple RGB->HSL, returns { h: 0-360, s: 0-100, l: 0-100 }.
  function rgbToHsl(r, g, b) {
    const rn = r / 255, gn = g / 255, bn = b / 255;
    const max = Math.max(rn, gn, bn), min = Math.min(rn, gn, bn);
    const l = (max + min) / 2;
    const d = max - min;
    const s = d === 0 ? 0 : d / (1 - Math.abs(2 * l - 1));
    return { h: rgbToHueDeg(r, g, b), s: s * 100, l: l * 100 };
  }

  // h/s/l in degrees / 0-1 / 0-1, returns { r, g, b } as 0-255 integers.
  function hslToRgb(h, s, l) {
    const c = (1 - Math.abs(2 * l - 1)) * s;
    const hp = norm360(h) / 60;
    const x = c * (1 - Math.abs(hp % 2 - 1));
    let r = 0, g = 0, b = 0;
    if      (hp < 1) { r = c; g = x; }
    else if (hp < 2) { r = x; g = c; }
    else if (hp < 3) { g = c; b = x; }
    else if (hp < 4) { g = x; b = c; }
    else if (hp < 5) { r = x; b = c; }
    else             { r = c; b = x; }
    const m = l - c / 2;
    return { r: Math.round((r + m) * 255), g: Math.round((g + m) * 255), b: Math.round((b + m) * 255) };
  }

  // Tint the hue badge with the effective color of the displayed target hue.
  // Display-only: never sends anything to the device.
  function updateHueBadge() {
    const displayed = norm360(Number(state.hue) || 0);
    let rgb;
    if (state.baseColor) {
      const hsl = rgbToHsl(state.baseColor.r, state.baseColor.g, state.baseColor.b);
      rgb = hslToRgb(displayed, hsl.s / 100, hsl.l / 100);
    } else {
      rgb = hslToRgb(displayed, 1, 0.5);
    }
    const luma = 0.2126 * rgb.r + 0.7152 * rgb.g + 0.0722 * rgb.b;
    dom.hueValue.style.background = 'rgb(' + rgb.r + ', ' + rgb.g + ', ' + rgb.b + ')';
    dom.hueValue.style.color = luma > 128 ? '#000' : '#fff';
  }

  function onHueInput() {
    dom.hueValue.textContent = dom.hueSlider.value + '\u00B0';
  }

  async function onHueChange() {
    const target = parseInt(dom.hueSlider.value, 10); // slider value = TARGET hue
    state.hue = target;
    dom.hueValue.textContent = target + '\u00B0';
    updateHueBadge();

    $$('.hue-btn').forEach(b => b.classList.remove('active'));
    const preset = document.querySelector('.hue-btn[data-hue="' + target + '"]');
    if (preset) preset.classList.add('active');

    if (state.connected) {
      // Firmware hue_shift is an ABSOLUTE rotation of the base color: send (T - B).
      const shift = state.baseColor ? Math.round(norm360(target - state.baseColor.hue)) : target;
      try {
        await ProtoTracerBLE.controlSet({ hue_shift: shift });
      } catch (e) {
        addLog('error', I18N.t('err.hue', { msg: e.message }));
      }
    }
  }

  async function onVoiceToggle() {
    state.voiceEnabled = dom.voiceToggle.checked;
    if (state.connected) {
      try {
        await ProtoTracerBLE.controlSet({ voice_enabled: state.voiceEnabled });
        addLog('info', state.voiceEnabled ? I18N.t('log.voiceOn') : I18N.t('log.voiceOff'));
      } catch (e) {
        addLog('error', I18N.t('err.voice', { msg: e.message }));
      }
    }
  }

  async function onDisplayModeToggle() {
    state.displayMode = dom.displayModeToggle.checked ? 1 : 0;
    if (state.connected) {
      try {
        await ProtoTracerBLE.controlSet({ display_mode: state.displayMode });
        addLog('info', state.displayMode ? I18N.t('log.dispOn') : I18N.t('log.dispOff'));
      } catch (e) {
        addLog('error', I18N.t('err.displayMode', { msg: e.message }));
      }
    }
  }

  async function onPingClick() {
    if (!state.connected) return;
    try {
      await ProtoTracerBLE.ping();
      addLog('info', I18N.t('log.pingSent'));
    } catch (e) {
      addLog('error', I18N.t('err.ping', { msg: e.message }));
    }
  }

  async function onRefreshManifestClick() {
    if (!state.connected) return;
    try {
      await ProtoTracerBLE.requestManifest();
      addLog('info', I18N.t('log.manifestRefresh'));
    } catch (e) {
      addLog('error', I18N.t('err.manifestRefresh', { msg: e.message }));
    }
  }

  async function onDisconnectClick() {
    try {
      await ProtoTracerBLE.disconnect();
    } catch (e) {
      addLog('error', I18N.t('err.disconnect', { msg: e.message }));
    }
  }

  // --- Expression Grid ---

  function renderExpressionGrid() {
    dom.expressionGrid.innerHTML = '';

    state.expressions.forEach((expr, i) => {
      const btn = document.createElement('button');
      btn.className = 'expr-btn';
      btn.textContent = expr.name;
      btn.title = expr.name;
      btn.addEventListener('click', () => selectExpression(i));
      dom.expressionGrid.appendChild(btn);
    });

    highlightExpression(state.currentExpression);
  }

  function selectExpression(index) {
    if (index === state.currentExpression) return;
    state.currentExpression = index;
    highlightExpression(index);

    if (state.connected) {
      ProtoTracerBLE.controlSet({ expression: index })
        .then(() => addLog('info', I18N.t('log.exprSet', { name: state.expressions[index]?.name || index })))
        .catch(e => addLog('error', I18N.t('err.exprSet', { msg: e.message })));
    }
  }

  function highlightExpression(index) {
    const buttons = dom.expressionGrid.querySelectorAll('.expr-btn');
    buttons.forEach((btn, i) => {
      btn.classList.toggle('active', i === index);
    });
  }

  // --- Toast Notifications ---

  function showToast(message, type) {
    type = type || 'info';
    const toast = document.createElement('div');
    toast.className = 'toast ' + type;
    toast.textContent = message;
    dom.toastContainer.appendChild(toast);

    setTimeout(() => {
      if (toast.parentNode) toast.parentNode.removeChild(toast);
    }, 3200);
  }

  // --- Logging ---

  function addLog(level, message) {
    const now = new Date();
    const ts = now.toLocaleTimeString('en-US', { hour12: false });
    const entry = document.createElement('div');
    entry.className = 'log-entry ' + level;
    entry.innerHTML = '<span class="ts">' + ts + '</span>' + escapeHtml(message);
    dom.logOutput.appendChild(entry);
    dom.logOutput.scrollTop = dom.logOutput.scrollHeight;

    while (dom.logOutput.children.length > 200) {
      dom.logOutput.firstChild.remove();
    }
  }

  function escapeHtml(text) {
    const div = document.createElement('div');
    div.textContent = text;
    return div.innerHTML;
  }

  // --- Boot ---
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();
