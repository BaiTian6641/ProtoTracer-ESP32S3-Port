/**
 * ProtoTracer BLE Protocol Module
 * 
 * Implements the ProtoTracer BLE JSON remote control protocol:
 * - Service UUID: 73cf57c7-6797-46e8-8202-dc5e7f956b57
 * - RX characteristic: device-specific (write from app → device)
 * - TX characteristic: device-specific (notify from device → app)
 * 
 * Protocol operations:
 *   config.get / pair.discover / pair.info  → manifest response (notify)
 *   control.set / control.patch             → command, acks with control.state
 *   ping                                    → pong response
 */

const ProtoTracerBLE = (() => {
  // --- Constants ---
  const SERVICE_UUID        = '73cf57c7-6797-46e8-8202-dc5e7f956b57';
  const BLE_CHUNK_BYTES     = 160;   // Max bytes per BLE notification chunk
  const BLE_RX_BUFFER_BYTES = 1024;  // Device-side RX buffer
  const SCAN_TIMEOUT_MS     = 10000; // BLE scan duration

  // --- Internal State ---
  let device          = null;
  let server          = null;
  let service         = null;
  let rxCharacteristic = null; // Write (app→device)
  let txCharacteristic = null; // Notify (device→app)
  let connected       = false;
  let cachedManifest  = null;
  let txBuffer        = '';     // Accumulator for incoming chunked JSON
  let eventCallbacks  = {};

  // --- Public API ---

  /** Register an event listener */
  function on(event, callback) {
    if (!eventCallbacks[event]) eventCallbacks[event] = [];
    eventCallbacks[event].push(callback);
  }

  /** Emit an event to all listeners */
  function emit(event, data) {
    const cbs = eventCallbacks[event] || [];
    cbs.forEach(cb => { try { cb(data); } catch (e) { console.error('[BLE] event handler error:', e); } });
  }

  /** Check if Web Bluetooth API is available */
  function isSupported() {
    return typeof navigator !== 'undefined' && !!navigator.bluetooth;
  }

  /** Get connection state */
  function isConnected() {
    return connected;
  }

  /** Get cached device manifest */
  function getManifest() {
    return cachedManifest;
  }

  /**
   * Scan for ProtoTracer devices advertising the service UUID.
   * Opens the browser's device picker dialog filtered by the service UUID.
   */
  async function scan(timeoutMs = SCAN_TIMEOUT_MS) {
    emit('status', { state: 'scanning', message: I18N.t('ble.scanning') });

    try {
      device = await navigator.bluetooth.requestDevice({
        filters: [{ services: [SERVICE_UUID] }],
        optionalServices: [SERVICE_UUID],
        acceptAllDevices: false,
      });
    } catch (err) {
      if (err.name === 'NotFoundError') {
        emit('status', { state: 'error', message: I18N.t('ble.noDeviceSelected') });
        return null;
      }
      throw err;
    }

    if (!device) {
      emit('status', { state: 'error', message: I18N.t('ble.noDevice') });
      return null;
    }

    emit('status', { state: 'found', message: I18N.t('ble.found', { name: device.name || 'Unknown' }) });
    return device;
  }

  /**
   * Set an externally-obtained BluetoothDevice and run the full connect flow.
   * For manual connection (name filter, etc.) where the app gets the device.
   */
  async function connectToDevice(bluetoothDevice) {
    device = bluetoothDevice;
    return connectInternal();
  }

  /**
   * Connect to the currently set device.
   * Public wrapper — calls the internal retry-capable implementation.
   */
  async function connect() {
    if (!device) throw new Error(I18N.t('err.noDevice'));
    return connectInternal();
  }

  /**
   * Internal connect implementation with retry logic.
   *
   * Web Bluetooth on Chrome/Edge sometimes leaves the GATT server in a
   * zombie state after requestDevice() performs its own brief service-
   * discovery connection.  We force-disconnect any stale session first,
   * then retry the full service-discovery handshake up to 3 times.
   */
  async function connectInternal(retries = 3) {
    if (!device) throw new Error(I18N.t('err.noDevice'));

    emit('status', { state: 'connecting', message: I18N.t('ble.connecting', { name: device.name || 'device' }) });

    // Remove any stale disconnect listener from a previous attempt.
    device.removeEventListener('gattserverdisconnected', onDisconnected);
    device.addEventListener('gattserverdisconnected', onDisconnected);

    let lastError = null;

    for (let attempt = 1; attempt <= retries; attempt++) {
      try {
        // If the browser thinks we're already connected (stale session),
        // force a clean disconnect before reconnecting.
        // Temporarily remove the disconnect listener so the forced
        // disconnect doesn't trigger onDisconnected and null out `device`.
        device.removeEventListener('gattserverdisconnected', onDisconnected);
        if (device.gatt && device.gatt.connected) {
          try { device.gatt.disconnect(); } catch (_) { /* ignore */ }
          await sleep(300);
        }
        device.addEventListener('gattserverdisconnected', onDisconnected);

        // Connect (or re-connect) to the GATT server.
        server = await device.gatt.connect();

        // Small settle delay — some ESP32 BLE stacks need a moment
        // after the GATT connection before service discovery works.
        await sleep(200);

        // Discover the primary service.
        service = await server.getPrimaryService(SERVICE_UUID);

        // Discover characteristics.
        const characteristics = await service.getCharacteristics();

        // Reset previous characteristic references.
        if (txCharacteristic) {
          try { await txCharacteristic.stopNotifications(); } catch (_) { /* ignore */ }
        }
        txCharacteristic = null;
        rxCharacteristic = null;

        for (const char of characteristics) {
          const props = char.properties;

          // TX = notify (device → app)
          if (props.notify && !txCharacteristic) {
            txCharacteristic = char;
            await txCharacteristic.startNotifications();
            txCharacteristic.addEventListener('characteristicvaluechanged', onTxValueChanged);
          }

          // RX = write / writeWithoutResponse (app → device)
          if ((props.write || props.writeWithoutResponse) && !rxCharacteristic) {
            rxCharacteristic = char;
          }
        }

        if (!rxCharacteristic || !txCharacteristic) {
          throw new Error(I18N.t('err.noCharacteristics'));
        }

        // Success — finalise state.
        connected = true;
        txBuffer = '';
        emit('status', { state: 'connected', message: I18N.t('ble.connected', { name: device.name || 'ProtoTracer' }) });
        emit('connected', { name: device.name || 'ProtoTracer', id: device.id });

        // Request the manifest (animations, expressions, etc.)
        await requestManifest();
        return; // Done.

      } catch (err) {
        lastError = err;
        const msg = (err.message || '').toLowerCase();

        // If the GATT server disconnected underneath us, retry.
        if (msg.includes('disconnected') || msg.includes('gatt') || msg.includes('retrieve services')) {
          if (attempt < retries) {
            const delay = 400 * attempt;
            emit('status', { state: 'connecting', message: I18N.t('ble.retry', { n: attempt, total: retries, ms: delay }) });
            await sleep(delay);

            // Ensure server is fully torn down before next attempt.
            try { if (server && server.connected) server.disconnect(); } catch (_) { /* ignore */ }
            server = null;
            service = null;
            txCharacteristic = null;
            rxCharacteristic = null;
            continue;
          }
        }

        // Non-retryable error — fail immediately.
        break;
      }
    }

    // All retries exhausted — report the failure.
    resetState();
    emit('status', { state: 'error', message: I18N.t('err.connectionFailed', { msg: lastError ? lastError.message : 'Unknown error' }) });
    throw lastError || new Error(I18N.t('err.connectionRetries'));
  }

  /** Disconnect from the device */
  async function disconnect() {
    // Remove the listener first so intentional disconnect doesn't fire it.
    if (device) {
      device.removeEventListener('gattserverdisconnected', onDisconnected);
    }
    if (txCharacteristic) {
      try { await txCharacteristic.stopNotifications(); } catch (e) { /* ignore */ }
    }
    if (server && server.connected) {
      try { server.disconnect(); } catch (e) { /* ignore */ }
    }
    resetState();
    emit('status', { state: 'disconnected', message: I18N.t('ble.disconnected') });
    emit('disconnected', {});
  }

  /** Reset connection state (preserves device reference for reconnection) */
  function resetState() {
    connected = false;
    // device is intentionally preserved — the browser owns its lifecycle
    // and we may need it for reconnection without re-scanning.
    server = null;
    service = null;
    rxCharacteristic = null;
    txCharacteristic = null;
    txBuffer = '';
  }

  /**
   * Send a JSON command to the device.
   * Automatically chunks writes > BLE_CHUNK_BYTES.
   */
  async function sendCommand(jsonObj) {
    if (!connected || !rxCharacteristic) {
      throw new Error(I18N.t('err.notConnected'));
    }

    const payload = JSON.stringify(jsonObj);
    const encoder = new TextEncoder();
    const bytes = encoder.encode(payload);

    if (bytes.length <= BLE_CHUNK_BYTES) {
      await rxCharacteristic.writeValueWithoutResponse(bytes);
    } else {
      // Chunked send
      for (let offset = 0; offset < bytes.length; offset += BLE_CHUNK_BYTES) {
        const chunk = bytes.slice(offset, Math.min(offset + BLE_CHUNK_BYTES, bytes.length));
        await rxCharacteristic.writeValueWithoutResponse(chunk);
        // Small delay between chunks to let device process
        await sleep(10);
      }
    }

    emit('sent', { op: jsonObj.op, payload });
  }

  // --- Protocol Commands ---

  /** Request device manifest (config.get) */
  async function requestManifest() {
    emit('status', { state: 'loading', message: I18N.t('ble.manifestLoading') });
    await sendCommand({ op: 'config.get' });
  }

  /** Send control.set with optional fields */
  async function controlSet(fields) {
    const cmd = { op: 'control.set' };
    if (fields.expression !== undefined) cmd.expression = fields.expression;
    if (fields.brightness !== undefined) cmd.brightness = fields.brightness;
    if (fields.voice_enabled !== undefined) cmd.voice_enabled = fields.voice_enabled;
    if (fields.display_mode !== undefined) cmd.display_mode = fields.display_mode;
    if (fields.hue_shift !== undefined) cmd.hue_shift = fields.hue_shift;
    await sendCommand(cmd);
  }

  /** Ping the device */
  async function ping() {
    await sendCommand({ op: 'ping' });
  }

  // --- Internal Handlers ---

  /** Handle incoming TX characteristic notifications (device → app) */
  function onTxValueChanged(event) {
    const value = event.target.value;
    if (!value) return;

    const decoder = new TextDecoder();
    const chunk = decoder.decode(value);

    txBuffer += chunk;

    // Try to extract complete JSON objects from the buffer
    while (true) {
      const start = txBuffer.indexOf('{');
      if (start === -1) {
        // No JSON start found; if there's garbage, clear it
        if (txBuffer.trim().length > 0 && start === -1) {
          txBuffer = '';
        }
        break;
      }

      // Find matching closing brace
      let depth = 0;
      let inString = false;
      let escaped = false;
      let end = -1;

      for (let i = start; i < txBuffer.length; i++) {
        const c = txBuffer[i];
        if (escaped) { escaped = false; continue; }
        if (c === '\\') { escaped = inString; continue; }
        if (c === '"') { inString = !inString; continue; }
        if (inString) continue;
        if (c === '{') depth++;
        else if (c === '}') {
          depth--;
          if (depth === 0) { end = i + 1; break; }
        }
      }

      if (end === -1) break; // Incomplete JSON, wait for more data

      const jsonStr = txBuffer.substring(start, end);
      txBuffer = txBuffer.substring(end);

      try {
        const msg = JSON.parse(jsonStr);
        handleMessage(msg);
      } catch (e) {
        console.warn('[BLE] Failed to parse JSON:', jsonStr.substring(0, 80));
      }
    }
  }

  /** Route incoming JSON messages */
  function handleMessage(msg) {
    const op = msg.op || '';
    emit('received', msg);

    switch (op) {
      case 'control.state':
        emit('controlState', msg);
        break;
      case 'pong':
        emit('pong', msg);
        break;
      default:
        // Treat any non-control/pong JSON containing 'visual' or 'pairing' as manifest
        if (msg.visual || msg.pairing || msg.device) {
          cachedManifest = msg;
          emit('manifest', msg);
          emit('status', { state: 'ready', message: I18N.t('ble.manifestLoaded') });
        } else {
          emit('unknown', msg);
        }
        break;
    }
  }

  /** Handle device disconnect (may fire from intentional or unexpected disconnects) */
  function onDisconnected() {
    const wasConnected = connected;

    // Clean up characteristic references but do NOT null `device` here —
    // it is still owned by the browser and may be reused for reconnection.
    connected = false;
    server = null;
    service = null;
    rxCharacteristic = null;
    txCharacteristic = null;
    txBuffer = '';

    if (wasConnected) {
      emit('status', { state: 'disconnected', message: I18N.t('ble.deviceDisconnected') });
      emit('disconnected', { unexpected: true });
    }
  }

  /** Utility: promise-based sleep */
  function sleep(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
  }

  // --- Exports ---
  return {
    on,
    isSupported,
    isConnected,
    getManifest,
    scan,
    connectToDevice,
    connect,
    disconnect,
    sendCommand,
    requestManifest,
    controlSet,
    ping,
    SERVICE_UUID,
  };
})();
