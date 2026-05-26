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
    voiceEnabled:      true,
    displayMode:       0,
    connected:         false,
  };

  // --- Initialization ---
  function init() {
    I18N.applyToDOM(document.body);
    updateLangToggleLabel();
    checkBLESupport();

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
      state.hue = msg.hue_shift;
      dom.hueSlider.value = Math.round(msg.hue_shift);
      dom.hueValue.textContent = Math.round(msg.hue_shift) + '\u00B0';
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

  function onHueInput() {
    dom.hueValue.textContent = dom.hueSlider.value + '\u00B0';
  }

  async function onHueChange() {
    const degrees = parseInt(dom.hueSlider.value, 10);
    state.hue = degrees;
    dom.hueValue.textContent = degrees + '\u00B0';

    $$('.hue-btn').forEach(b => b.classList.remove('active'));
    const preset = document.querySelector('.hue-btn[data-hue="' + degrees + '"]');
    if (preset) preset.classList.add('active');

    if (state.connected) {
      try {
        await ProtoTracerBLE.controlSet({ hue_shift: degrees });
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
