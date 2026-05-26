/**
 * ProtoTracer Remote — i18n Module
 *
 * Lightweight localization with English (en) and 简体中文 (zh) support.
 * Detects browser language on load; allows manual toggle via setLang().
 *
 * Usage:
 *   I18N.t('status.disconnected')           // → "Disconnected" / "未连接"
 *   I18N.t('log.connected', {name:'Fox'})   // → "Connected to Fox" / "已连接 Fox"
 *   I18N.setLang('zh');                     // switch to Chinese
 *   I18N.applyToDOM(document.body);         // scan [data-i18n] attrs
 */

const I18N = (() => {
  // ── Translations ──────────────────────────────────────────────
  const DICT = {
    en: {
      // ── Status bar ──
      'status.disconnected':    'Disconnected',
      'status.connecting':      'Connecting…',
      'status.connected':       'Connected',
      'status.error':           'Error',
      'status.scanning':        'Scanning…',
      'status.loading':         'Loading…',
      'status.ready':           'Ready',
      'status.found':           'Device found',

      // ── Connect panel ──
      'hero.title':             'ProtoTracer Remote',
      'hero.subtitle':          'Web Bluetooth Remote Control',
      'connect.title':          'Connect to Device',
      'connect.desc':           'Make sure your ProtoTracer is powered on and BLE advertising is active.',
      'connect.scanBtn':        '🔍 Scan for ProtoTracer',
      'connect.scanHint':       'Scanning… select your ProtoTracer from the dialog.',
      'connect.manualTitle':    'Manual Connection',
      'connect.manualDesc':     'If auto-scan doesn\'t work, enter your device name or BLE address.',
      'connect.placeholder':    'Device name (e.g. Protogen-01)',
      'connect.manualBtn':      'Connect',
      'connect.compatTitle':    'Browser Compatibility',

      // ── BLE support ──
      'ble.supported':          '✅ Web Bluetooth is supported in this browser (Chrome / Edge).',
      'ble.unsupported':        '⚠️ Web Bluetooth is not supported in this browser. Please use Chrome or Edge on desktop or Android.',

      // ── Control panel ──
      'ctrl.expressions':       'Expressions',
      'ctrl.exprCount':         '{n} exprs',
      'ctrl.brightness':        'Brightness',
      'ctrl.hue':               'Color Hue Shift',
      'ctrl.options':           'Options',
      'ctrl.lipSync':           '🎤 Lip Sync',
      'ctrl.displayMode':       '🖥️ Display Mode',
      'ctrl.quickActions':      'Quick Actions',
      'ctrl.ping':              '📡 Ping Device',
      'ctrl.refreshManifest':   '🔄 Refresh Manifest',
      'ctrl.disconnect':        '🔌 Disconnect',
      'ctrl.log':               'Event Log',
      'ctrl.clearLog':          'Clear',

      // ── Log messages ──
      'log.ready':              'ProtoTracer Remote ready.',
      'log.connecting':         'Connecting…',
      'log.connected':          'Connected to {name}',
      'log.disconnected':       'Disconnected.',
      'log.disconnectedUnexp':  'Device disconnected unexpectedly.',
      'log.disconnectedToast':  'Device disconnected',
      'log.manifestRcvd':       'Manifest received.',
      'log.animation':          'Animation: {name}, {n} expressions',
      'log.exprFallback':       'Expression {n}',
      'log.pong':               'Pong from {name}',
      'log.pongToast':          'Device is alive! 📡',
      'log.sent':               '→ Sent: {op} ({n} bytes)',
      'log.recv':               '← Recv: {preview}',
      'log.exprSet':            'Expression set: {name}',
      'log.voiceOn':            'Voice enabled',
      'log.voiceOff':           'Voice disabled',
      'log.dispOn':             'Display mode on',
      'log.dispOff':            'Display mode off',
      'log.pingSent':           'Ping sent…',
      'log.manifestRefresh':    'Manifest refresh requested…',
      'log.found':              'Found: {name}, connecting…',

      // ── Errors ──
      'err.scanFailed':         'Scan failed: {msg}',
      'err.scanToast':          'Scan error: {msg}',
      'err.manualFailed':       'Manual connect failed: {msg}',
      'err.connectFailed':      'Connection failed: {msg}',
      'err.connectToast':       'Connection failed: {msg}',
      'err.brightness':         'Brightness set failed: {msg}',
      'err.hue':                'Hue set failed: {msg}',
      'err.voice':              'Voice toggle failed: {msg}',
      'err.displayMode':        'Display mode toggle failed: {msg}',
      'err.ping':               'Ping failed: {msg}',
      'err.manifestRefresh':    'Manifest refresh failed: {msg}',
      'err.disconnect':         'Disconnect failed: {msg}',
      'err.exprSet':            'Expression set failed: {msg}',
      'err.noDeviceName':       'Enter a device name first.',
      'err.noDevice':           'No device to connect to. Scan first.',
      'err.notConnected':       'Not connected to device.',
      'err.noCharacteristics':  'Could not find required BLE characteristics.',
      'err.connectionFailed':   'Connection failed: {msg}',
      'err.connectionRetries':  'Connection failed after retries.',

      // ── BLE internal status (ble.js → app.js) ──
      'ble.scanning':           'Scanning for ProtoTracer devices…',
      'ble.noDeviceSelected':   'No ProtoTracer device selected.',
      'ble.noDevice':           'No device selected.',
      'ble.found':              'Found: {name}',
      'ble.connecting':         'Connecting to {name}…',
      'ble.retry':              'Retry {n}/{total} in {ms}ms…',
      'ble.connected':          'Connected to {name}',
      'ble.manifestLoading':    'Requesting device manifest…',
      'ble.manifestLoaded':     'Manifest loaded',
      'ble.deviceDisconnected': 'Device disconnected',
      'ble.disconnected':       'Disconnected',
      'ble.connectedToast':     'Connected to {name}',
    },

    zh: {
      // ── 状态栏 ──
      'status.disconnected':    '未连接',
      'status.connecting':      '连接中…',
      'status.connected':       '已连接',
      'status.error':           '错误',
      'status.scanning':        '扫描中…',
      'status.loading':         '加载中…',
      'status.ready':           '就绪',
      'status.found':           '已发现设备',

      // ── 连接面板 ──
      'hero.title':             'ProtoTracer 遥控器',
      'hero.subtitle':          'Web 蓝牙远程控制',
      'connect.title':          '连接设备',
      'connect.desc':           '请确保 ProtoTracer 已开机且 BLE 正在广播。',
      'connect.scanBtn':        '🔍 扫描 ProtoTracer',
      'connect.scanHint':       '正在扫描… 请在对话框中选择您的 ProtoTracer。',
      'connect.manualTitle':    '手动连接',
      'connect.manualDesc':     '如果自动扫描失败，请输入设备名称或 BLE 地址。',
      'connect.placeholder':    '设备名称（例如 Protogen-01）',
      'connect.manualBtn':      '连接',
      'connect.compatTitle':    '浏览器兼容性',

      // ── BLE 支持 ──
      'ble.supported':          '✅ 此浏览器支持 Web Bluetooth（Chrome / Edge）。',
      'ble.unsupported':        '⚠️ 此浏览器不支持 Web Bluetooth。请在桌面端或安卓端使用 Chrome 或 Edge。',

      // ── 控制面板 ──
      'ctrl.expressions':       '表情',
      'ctrl.exprCount':         '{n} 个表情',
      'ctrl.brightness':        '亮度',
      'ctrl.hue':               '色相偏移',
      'ctrl.options':           '选项',
      'ctrl.lipSync':           '🎤 唇音同步',
      'ctrl.displayMode':       '🖥️ 显示模式',
      'ctrl.quickActions':      '快捷操作',
      'ctrl.ping':              '📡 检测连接',
      'ctrl.refreshManifest':   '🔄 刷新配置',
      'ctrl.disconnect':        '🔌 断开连接',
      'ctrl.log':               '事件日志',
      'ctrl.clearLog':          '清空',

      // ── 日志消息 ──
      'log.ready':              'ProtoTracer 遥控器已就绪。',
      'log.connecting':         '连接中…',
      'log.connected':          '已连接 {name}',
      'log.disconnected':       '已断开连接。',
      'log.disconnectedUnexp':  '设备意外断开。',
      'log.disconnectedToast':  '设备已断开',
      'log.manifestRcvd':       '已接收配置清单。',
      'log.animation':          '动画：{name}，{n} 个表情',
      'log.exprFallback':       '表情 {n}',
      'log.pong':               '来自 {name} 的响应',
      'log.pongToast':          '设备在线！📡',
      'log.sent':               '→ 发送：{op}（{n} 字节）',
      'log.recv':               '← 接收：{preview}',
      'log.exprSet':            '表情已切换：{name}',
      'log.voiceOn':            '唇音同步已开启',
      'log.voiceOff':           '唇音同步已关闭',
      'log.dispOn':             '显示模式已开启',
      'log.dispOff':            '显示模式已关闭',
      'log.pingSent':           'Ping 已发送…',
      'log.manifestRefresh':    '配置刷新请求已发送…',
      'log.found':              '已发现：{name}，正在连接…',

      // ── 错误 ──
      'err.scanFailed':         '扫描失败：{msg}',
      'err.scanToast':          '扫描错误：{msg}',
      'err.manualFailed':       '手动连接失败：{msg}',
      'err.connectFailed':      '连接失败：{msg}',
      'err.connectToast':       '连接失败：{msg}',
      'err.brightness':         '亮度设置失败：{msg}',
      'err.hue':                '色相设置失败：{msg}',
      'err.voice':              '唇音同步切换失败：{msg}',
      'err.displayMode':        '显示模式切换失败：{msg}',
      'err.ping':               'Ping 失败：{msg}',
      'err.manifestRefresh':    '配置刷新失败：{msg}',
      'err.disconnect':         '断开连接失败：{msg}',
      'err.exprSet':            '表情设置失败：{msg}',
      'err.noDeviceName':       '请先输入设备名称。',
      'err.noDevice':           '没有可连接的设备。请先扫描。',
      'err.notConnected':       '未连接到设备。',
      'err.noCharacteristics':  '未找到所需的 BLE 特征值。',
      'err.connectionFailed':   '连接失败：{msg}',
      'err.connectionRetries':  '多次重试后连接失败。',

      // ── BLE 内部状态 ──
      'ble.scanning':           '正在扫描 ProtoTracer 设备…',
      'ble.noDeviceSelected':   '未选择 ProtoTracer 设备。',
      'ble.noDevice':           '未选择设备。',
      'ble.found':              '已发现：{name}',
      'ble.connecting':         '正在连接 {name}…',
      'ble.retry':              '重试 {n}/{total}，{ms}ms 后…',
      'ble.connected':          '已连接 {name}',
      'ble.manifestLoading':    '正在请求设备配置…',
      'ble.manifestLoaded':     '配置清单已加载',
      'ble.deviceDisconnected': '设备已断开',
      'ble.disconnected':       '已断开',
      'ble.connectedToast':     '已连接 {name}',
    },
  };

  // ── State ─────────────────────────────────────────────────────
  let currentLang = 'en';

  // ── Language detection ────────────────────────────────────────
  function detectLang() {
    try {
      // Check localStorage first
      const stored = localStorage.getItem('prototracer-lang');
      if (stored === 'en' || stored === 'zh') return stored;

      // Fall back to browser preference
      const navLang = (navigator.language || navigator.userLanguage || '').toLowerCase();
      if (navLang.startsWith('zh')) return 'zh';
    } catch (_) { /* localStorage may be blocked */ }
    return 'en';
  }

  // ── Public API ────────────────────────────────────────────────

  /**
   * Translate a key with optional placeholder substitutions.
   * Falls back to the key itself if no translation is found.
   *
   *   I18N.t('log.connected', { name: 'Fox' })
   *   // en → "Connected to Fox"
   *   // zh → "已连接 Fox"
   */
  function t(key, params) {
    const dict = DICT[currentLang] || DICT.en;
    let text = dict[key];
    if (text === undefined) {
      text = DICT.en[key];
    }
    if (text === undefined) {
      return key; // ultimate fallback
    }
    if (params) {
      Object.keys(params).forEach(k => {
        text = text.replace(`{${k}}`, params[k]);
      });
    }
    return text;
  }

  /** Get current language code */
  function getLang() {
    return currentLang;
  }

  /** Set language and persist preference */
  function setLang(lang) {
    if (lang !== 'en' && lang !== 'zh') return;
    currentLang = lang;
    try { localStorage.setItem('prototracer-lang', lang); } catch (_) {}
  }

  /**
   * Walk the DOM and apply translations to elements with [data-i18n]
   * or [data-i18n-placeholder] attributes.
   */
  function applyToDOM(root) {
    // Text content
    root.querySelectorAll('[data-i18n]').forEach(el => {
      const key = el.getAttribute('data-i18n');
      if (key) el.textContent = t(key);
    });

    // Input placeholders
    root.querySelectorAll('[data-i18n-placeholder]').forEach(el => {
      const key = el.getAttribute('data-i18n-placeholder');
      if (key) el.setAttribute('placeholder', t(key));
    });

    // Also set <html lang="">
    document.documentElement.lang = currentLang;
    document.title = t('hero.title');
  }

  // ── Init ──────────────────────────────────────────────────────
  currentLang = detectLang();

  return { t, getLang, setLang, applyToDOM };
})();
