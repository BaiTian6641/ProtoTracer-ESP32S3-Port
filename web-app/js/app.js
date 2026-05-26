/**
 * ProtoTracer Remote - Application Controller
 * 
 * Manages UI state, user interactions, and bridges BLE events to the DOM.
 */

(function () {
  'use strict';

  // --- DOM References ---
  const $ = (sel) => document.querySelector(sel);
  const $$ = (sel) => document.querySelectorAll(sel);

  const dom = {
    statusDot:       $('#statusDot'),
    statusText:      $('#statusText'),
    deviceName:      $('#deviceName'),
    batteryBadge:    $('#batteryBadge'),
    connectPanel:    $('#connectPanel'),
    controlPanel:    $('#controlPanel'),
    btnScan:         $('#btnScan'),
    btnManualConnect:$('#btnManualConnect'),
    manualDeviceName:$('#manualDeviceName'),
    scanStatus:      $('#scanStatus'),
    deviceList:      $('#deviceList'),
    bleSupportInfo:  $('#bleSupportInfo'),
    expressionGrid:  $('#expressionGrid'),
    exprCount:       $('#exprCount'),
    brightnessSlider:$('#brightnessSlider'),
    brightnessValue: $('#brightnessValue'),
    hueSlider:       $('#hueSlider'),
    hueValue:        $('#hueValue'),
    voiceToggle:     $('#voiceToggle'),
    displayModeToggle:$('#displayModeToggle'),
    btnPing:         $('#btnPing'),
    btnRefreshManifest:$('#btnRefreshManifest'),
    btnDisconnect:   $('#btnDisconnect'),
    btnClearLog:     $('#btnClearLog'),
    logOutput:       $('#logOutput'),
    toastContainer:  $('#toastContainer'),
  };

  // --- Application State ---
  let state = {
    expressions: [],      // [{name, index}]
    currentExpression: 0,
    brightness: 105,
    hue: 0.0,
    voiceEnabled: true,
    displayMode: 0,
    connected: false,
  };

  // --- Initialization ---
  function init() {
    checkBLESupport();

    // BLE events
    ProtoTracerBLE.on('status', onStatusChange);
    ProtoTracerBLE.on('connected', onConnected);
    ProtoTracerBLE.on('disconnected', onDisconnected);
    ProtoTracerBLE.on('manifest', onManifestReceived);
    ProtoTracerBLE.on('controlState', onControlState);
    ProtoTracerBLE.on('pong', onPongReceived);
    ProtoTracerBLE.on('sent', onCommandSent);
    ProtoTracerBLE.on('received', onDataReceived);

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

    // Hue preset buttons
    $$('.hue-btn').forEach(btn => {
      btn.addEventListener('click', () => {
        const hue = parseInt(btn.dataset.hue, 10);
        dom.hueSlider.value = hue;
        onHueChange();
      });
    });

    // Enable scan button if BLE is supported
    if (ProtoTracerBLE.isSupported()) {
      dom.btnScan.disabled = false;
    }

    addLog('info', 'ProtoTracer Remote ready.');
  }

  // --- BLE Support Check ---
  function checkBLESupport() {
    if (ProtoTracerBLE.isSupported()) {
      dom.bleSupportInfo.className = 'compat-info supported';
      dom.bleSupportInfo.textContent = '✅ Web Bluetooth is supported in this browser (Chrome / Edge).';
      dom.btnScan.disabled = false;
      dom.btnManualConnect.disabled = false;
    } else {
      dom.bleSupportInfo.className = 'compat-info unsupported';
      dom.bleSupportInfo.innerHTML = '⚠️ Web Bluetooth is not supported in this browser.<br>'
        + 'Please use <strong>Chrome</strong> or <strong>Edge</strong> on desktop or Android.';
    }
  }

  // --- Event Handlers: BLE ---

  function onStatusChange(data) {
    dom.statusText.textContent = data.message;
    dom.statusDot.className = 'dot ' + data.state;

    switch (data.state) {
      case 'scanning':
        dom.scanStatus.textContent = 'Scanning... select your ProtoTracer from the dialog.';
        break;
      case 'connecting':
        dom.scanStatus.textContent = 'Connecting...';
        break;
      case 'connected':
        dom.scanStatus.textContent = '';
        break;
      case 'disconnected':
      case 'error':
        dom.scanStatus.textContent = '';
        break;
    }
  }

  function onConnected(data) {
    state.connected = true;
    dom.deviceName.textContent = data.name || 'ProtoTracer';
    dom.deviceName.parentElement.classList.remove('hidden');
    dom.connectPanel.classList.add('hidden');
    dom.controlPanel.classList.remove('hidden');
    addLog('success', `Connected to ${data.name || 'ProtoTracer'}`);
    showToast(`Connected to ${data.name || 'ProtoTracer'}`, 'success');
  }

  function onDisconnected(data) {
    state.connected = false;
    dom.deviceName.parentElement.classList.add('hidden');
    dom.controlPanel.classList.add('hidden');
    dom.connectPanel.classList.remove('hidden');
    addLog('warn', data.unexpected ? 'Device disconnected unexpectedly.' : 'Disconnected.');
    if (data.unexpected) showToast('Device disconnected', 'warning');
  }

  function onManifestReceived(manifest) {
    addLog('success', 'Manifest received.');

    // Parse visual / expression info
    const visual = manifest.visual || {};
    const deviceInfo = manifest.device || {};

    if (visual.expression_names && visual.expression_names.length > 0) {
      state.expressions = visual.expression_names.map((name, i) => ({ name, index: i }));
    } else if (visual.expression_count) {
      // Fallback: generate numbered expressions
      state.expressions = Array.from({ length: visual.expression_count }, (_, i) => ({
        name: `Expression ${i + 1}`,
        index: i,
      }));
    }

    dom.exprCount.textContent = `${state.expressions.length} exprs`;
    renderExpressionGrid();

    if (deviceInfo.display_name) {
      dom.deviceName.textContent = deviceInfo.display_name;
    }

    addLog('info', `Animation: ${visual.animation_name || 'Unknown'}, ${state.expressions.length} expressions`);
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
      dom.hueValue.textContent = `${Math.round(msg.hue_shift)}°`;
    }

    // Update expression grid highlight
    highlightExpression(state.currentExpression);
  }

  function onPongReceived(msg) {
    addLog('success', `Pong from ${msg.name || 'device'}`);
    showToast('Device is alive! 📡', 'success');
  }

  function onCommandSent(data) {
    addLog('data', `→ Sent: ${data.op} (${data.payload.length} bytes)`);
  }

  function onDataReceived(data) {
    if (data.op === 'control.state' || data.op === 'pong') return; // logged separately
    const preview = JSON.stringify(data).substring(0, 120);
    addLog('data', `← Recv: ${preview}${preview.length >= 120 ? '...' : ''}`);
  }

  // --- Event Handlers: UI ---

  async function onScanClick() {
    try {
      const dev = await ProtoTracerBLE.scan();
      if (dev) {
        await ProtoTracerBLE.connect();
      }
    } catch (err) {
      addLog('error', `Scan failed: ${err.message}`);
      showToast(`Scan error: ${err.message}`, 'error');
    }
  }

  async function onManualConnectClick() {
    const nameFilter = dom.manualDeviceName.value.trim();
    if (!nameFilter) {
      showToast('Enter a device name first.', 'warning');
      return;
    }

    try {
      const btDevice = await navigator.bluetooth.requestDevice({
        filters: [{ namePrefix: nameFilter }],
        optionalServices: [ProtoTracerBLE.SERVICE_UUID],
      });

      if (btDevice) {
        addLog('info', `Found: ${btDevice.name || nameFilter}, connecting...`);
        await ProtoTracerBLE.connectToDevice(btDevice);
      }
    } catch (err) {
      addLog('error', `Manual connect failed: ${err.message}`);
      showToast(`Connection failed: ${err.message}`, 'error');
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
        addLog('error', `Brightness set failed: ${e.message}`);
      }
    }
  }

  function onHueInput() {
    dom.hueValue.textContent = `${dom.hueSlider.value}°`;
  }

  async function onHueChange() {
    const degrees = parseInt(dom.hueSlider.value, 10);
    state.hue = degrees;
    dom.hueValue.textContent = `${degrees}°`;

    // Highlight active preset
    $$('.hue-btn').forEach(b => b.classList.remove('active'));
    const preset = document.querySelector(`.hue-btn[data-hue="${degrees}"]`);
    if (preset) preset.classList.add('active');

    if (state.connected) {
      try {
        await ProtoTracerBLE.controlSet({ hue_shift: degrees });
      } catch (e) {
        addLog('error', `Hue set failed: ${e.message}`);
      }
    }
  }

  async function onVoiceToggle() {
    state.voiceEnabled = dom.voiceToggle.checked;
    if (state.connected) {
      try {
        await ProtoTracerBLE.controlSet({ voice_enabled: state.voiceEnabled });
        addLog('info', `Voice ${state.voiceEnabled ? 'enabled' : 'disabled'}`);
      } catch (e) {
        addLog('error', `Voice toggle failed: ${e.message}`);
      }
    }
  }

  async function onDisplayModeToggle() {
    state.displayMode = dom.displayModeToggle.checked ? 1 : 0;
    if (state.connected) {
      try {
        await ProtoTracerBLE.controlSet({ display_mode: state.displayMode });
        addLog('info', `Display mode ${state.displayMode ? 'on' : 'off'}`);
      } catch (e) {
        addLog('error', `Display mode toggle failed: ${e.message}`);
      }
    }
  }

  async function onPingClick() {
    if (!state.connected) return;
    try {
      await ProtoTracerBLE.ping();
      addLog('info', 'Ping sent...');
    } catch (e) {
      addLog('error', `Ping failed: ${e.message}`);
    }
  }

  async function onRefreshManifestClick() {
    if (!state.connected) return;
    try {
      await ProtoTracerBLE.requestManifest();
      addLog('info', 'Manifest refresh requested...');
    } catch (e) {
      addLog('error', `Manifest refresh failed: ${e.message}`);
    }
  }

  async function onDisconnectClick() {
    try {
      await ProtoTracerBLE.disconnect();
    } catch (e) {
      addLog('error', `Disconnect failed: ${e.message}`);
    }
  }

  // --- Expression Grid ---

  function renderExpressionGrid() {
    dom.expressionGrid.innerHTML = '';

    state.expressions.forEach((expr, i) => {
      const btn = document.createElement('button');
      btn.className = 'expr-btn';
      btn.textContent = expr.name;
      btn.title = `${expr.name} (index ${i})`;
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
        .then(() => addLog('info', `Expression set: ${state.expressions[index]?.name || index}`))
        .catch(e => addLog('error', `Expression set failed: ${e.message}`));
    }
  }

  function highlightExpression(index) {
    const buttons = dom.expressionGrid.querySelectorAll('.expr-btn');
    buttons.forEach((btn, i) => {
      btn.classList.toggle('active', i === index);
    });
  }

  // --- Toast Notifications ---

  function showToast(message, type = 'info') {
    const toast = document.createElement('div');
    toast.className = `toast ${type}`;
    toast.textContent = message;
    dom.toastContainer.appendChild(toast);

    // Auto-remove after animation
    setTimeout(() => {
      if (toast.parentNode) toast.parentNode.removeChild(toast);
    }, 3200);
  }

  // --- Logging ---

  function addLog(level, message) {
    const now = new Date();
    const ts = now.toLocaleTimeString('en-US', { hour12: false });
    const entry = document.createElement('div');
    entry.className = `log-entry ${level}`;
    entry.innerHTML = `<span class="ts">${ts}</span>${escapeHtml(message)}`;
    dom.logOutput.appendChild(entry);
    dom.logOutput.scrollTop = dom.logOutput.scrollHeight;

    // Cap log entries
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
