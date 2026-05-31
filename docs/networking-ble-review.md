# Networking & BLE Review — ProtoTracer ESP32-S3

> **Date**: 2026-05-31 | **Key Files**: `Menu/ESPMenu.h`, `Network/*`, `main.cpp`

## 1. Architecture Overview

```
┌──────────────────────────────────────────────────┐
│  main loop() [Core 1 - Arduino Task]             │
│    animation.UpdateTime() → controller.Render()  │
│    → delay(1) → controller.Display()             │
│    → yield()                                     │
├──────────────────────────────────────────────────┤
│  Background Tasks (FreeRTOS):                    │
│    - WiFi/LWIP stack (separate task)             │
│    - BLE stack (NimBLE, separate task)            │
│    - AsyncTCP worker (separate task)             │
│    - AsyncWebServer (event-driven, same task)    │
└──────────────────────────────────────────────────┘
```

**Key Insight**: Everything is single-threaded on the Arduino `loop()`. BLE, WiFi, and TCP run in their own FreeRTOS tasks. This means BLE callbacks (`onWrite`, `onConnect`) execute in the BLE task context, NOT in the main loop context.

## 2. BLE System (ESPMenu.h) — CRITICAL REVIEW

### 2.1 Shared State Without Adequate Synchronization ⚠️

```cpp
// Static globals (file scope in ESPMenu.h):
static BLEServer *bleServer = nullptr;
static BLECharacteristic *bleTxCharacteristic = nullptr;
static bool bleDeviceConnected = false;       // ⚠️ accessed from BLE task AND main loop
static bool bleOldDeviceConnected = false;    // ⚠️ same issue
static String blePendingJsonPayload;          // ⚠️ protected by spinlock
static bool bleJsonPayloadPending = false;    // ⚠️ protected by spinlock
static String bleRxJsonBuffer;                // ⚠️ accessed from BLE callback AND main loop
static bool bleManifestRequested = false;     // ⚠️ no synchronization
static String bleCachedManifestJson;          // ⚠️ no synchronization
```

### ⚠️ Race Condition #1: `bleRxJsonBuffer`
```cpp
// BLE callback (BLE task context):
void onWrite(BLECharacteristic *characteristic) {
    std::string rxValue(...);
    ApplyBleJsonWrite(rxValue);  // modifies bleRxJsonBuffer
}

// ApplyBleJsonWrite (in BLE callback context):
bool ApplyBleJsonWrite(const std::string &rxValue) {
    bleRxJsonBuffer += String(rxValue.c_str());  // ⚠️ String += allocates!
    // ... parsing ...
    bleRxJsonBuffer = String("");  // reset
}
```

**Problems**:
1. `String::operator+=` allocates memory from the heap — doing this inside a BLE callback is dangerous (BLE stack may be in a critical section)
2. `bleRxJsonBuffer` is also accessed from `Menu::Update()` (main loop) via `ClearQueuedLegacyCommands()` on disconnect
3. No mutex/critical section protection for `bleRxJsonBuffer`

### ⚠️ Race Condition #2: `bleDeviceConnected` / `bleOldDeviceConnected`
```cpp
// BLE callback (BLE task):
void onConnect(BLEServer *server) {
    bleDeviceConnected = true;     // ⚠️ non-atomic write
    bleRxJsonBuffer = String("");  // ⚠️ String allocation in BLE context
}

// Main loop:
void Update() {
    if (!bleDeviceConnected && bleOldDeviceConnected) { ... }  // ⚠️ non-atomic read
    else if (bleDeviceConnected && !bleOldDeviceConnected) { ... }
}
```

On ESP32-S3, `bool` reads/writes are NOT guaranteed atomic (they may be byte operations that the compiler could optimize across). While unlikely to tear on a 32-bit aligned bool, it's undefined behavior.

### ⚠️ Race Condition #3: `bleManifestRequested` / `bleCachedManifestJson`
```cpp
// BLE callback:
bleManifestRequested = true;  // ⚠️ no barrier

// Main loop:
if (bleManifestRequested) {
    bleManifestRequested = false;
    bleCachedManifestJson = BuildRemoteControllerManifestJson(); // ⚠️ 65KB DynamicJsonDocument!
    QueueBleJsonPayload(bleCachedManifestJson);
}
```

`BuildRemoteControllerManifestJson()` opens LittleFS, reads a JSON file, creates a `DynamicJsonDocument(65536)` (65 KB!), parses, builds a response doc (6144 bytes), serializes — ALL in the main loop. This blocks rendering for potentially hundreds of milliseconds.

### 2.2 BLE Notify Blocking ⚠️

```cpp
void NotifyBleJsonPayload(const String &payload) {
    for (size_t offset = 0; offset < payload.length(); offset += kBleJsonChunkBytes) {
        bleTxCharacteristic->setValue(...);
        bleTxCharacteristic->notify();
        delay(8);  // ⚠️ BLOCKS MAIN LOOP for 8ms per chunk!
    }
}
```

For a typical manifest JSON of ~3KB, that's ~19 chunks × 8ms = ~152ms of blocking delay in the main loop. During this time:
- Animation freezes
- HUB75 panel shows last frame (no Display() call)
- Watchdog is NOT fed (no `yield()` inside the loop)

### 2.3 BLE JSON Parsing Memory

`DynamicJsonDocument doc(65536)` in `LoadAnimationManifestMetadata()` — that's 65 KB allocated from internal DRAM (due to `heap_caps_malloc_extmem_enable(0)`). If this allocation fails, the function returns an empty metadata struct silently, and the BLE manifest will have no expression names.

### 2.4 Legacy Command Queue — Adequate

```cpp
static portMUX_TYPE gCommandQueueMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t remoteCommandQueue[8];
```

The spinlock-protected ring buffer for legacy numeric commands is properly implemented. `EnqueueLegacyCommand` and `DequeueLegacyCommand` use correct `portENTER_CRITICAL`/`portEXIT_CRITICAL` patterns.

## 3. WiFi & HTTP Networking

### 3.1 AsyncWebServer on Port 80

```cpp
AsyncWebServer server(80);
server.begin();  // starts in setup()
```

The server runs in AsyncTCP's task context. Routes handle:
- `/` → redirect to ElegantOTA
- `/update` → ElegantOTA firmware upload
- `/api/relay/esp32c6/*` → relay server for companion ESP32-C6 remote

### 3.2 Remote File Downloads During Startup ⚠️

The startup sequence downloads multiple files from GitHub/Gitee:
1. `user_config.json` (via `DownloadUserConfigFromSources`)
2. Firmware manifest + binary (via `FirmwareUpdater::CheckAndUpdate`)
3. Face model JSON (via `EnsureFaceModelJson`)
4. Animation JSON (via `AnimationDownloader::Download`)

Each download:
- Uses blocking HTTP client (not async)
- Creates temporary `String` objects for URL, headers, response
- Verifies MD5 (which reads the entire file + computes hash)

**Risk**: If any download stalls (network timeout), the entire startup hangs. The face model download has a 10-second timeout, but if it fails, `ESP.restart()` is called — which reboots the device, potentially creating an infinite boot loop if the remote file is persistently unavailable.

### 3.3 `RefreshRelayAsset` Called from HTTP Handler

```cpp
server.on("/api/relay/esp32c6/manifest", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!RefreshRelayAsset(kRemoteC6ManifestFilename, kRelayManifestCachePath)) {
        request->send(502, ...);
        return;
    }
    request->send(LittleFS, kRelayManifestCachePath, "application/json", false);
});
```

`RefreshRelayAsset` calls `RemoteFileSync::SyncAny` which does blocking HTTP + MD5 computation. This runs INSIDE the AsyncTCP request handler, blocking the async task. For small files this is acceptable, but if the remote is slow, it could stall other HTTP requests.

## 4. FreeRTOS Task Considerations

### 4.1 Arduino Loop Task Priority
The main `loop()` runs on the Arduino task (normally priority 1 on core 1). The BLE task (NimBLE) typically runs at higher priority to meet Bluetooth timing requirements.

### 4.2 Watchdog Feeding
`yield()` is called between animation, render, and display steps. This feeds the task watchdog BUT also allows the BLE and WiFi tasks to run. However:

- Between `animation.UpdateTime()` and `controller.Render()`, there's only one `yield()`
- Between `controller.Render()` and `controller.Display()`, there's a `delay(1)` + `yield()`
- Inside `NotifyBleJsonPayload`, there are `delay(8)` calls but NO `yield()` — watchdog may trigger

### 4.3 Core Pinning
From the board JSON:
```json
"-DARDUINO_RUNNING_CORE=1",
"-DARDUINO_EVENT_RUNNING_CORE=1"
```
Arduino runs on Core 1. This leaves Core 0 free for WiFi/BLE stack, which is good. But rendering is also on Core 1 — if it blocks for too long, Arduino events (like `loop()`) can't process.

## 5. Memory Allocation in Network Paths

### 5.1 `ConnectWifiWithNetWizard`
Uses NetWizard library which internally creates WiFi scan results, HTML pages, etc. All from internal DRAM.

### 5.2 `RemoteFileSync::Sync`
- Creates `HTTPClient` (stack)
- Creates `WiFiClient` (stack + internal buffers)
- Computes MD5 using `mbedtls` (stack + internal buffers)
- Creates `File` objects for LittleFS read/write

### 5.3 ElegantOTA
- Creates AsyncWebServer routes
- Handles firmware binary upload to LittleFS staging
- The staging file is on LittleFS (flash), not RAM — good

## 6. Critical Questions

1. **Q: Does the freeze happen when a BLE device connects and requests the manifest?** This would trigger `BuildRemoteControllerManifestJson()` with a 65KB JSON allocation + `NotifyBleJsonPayload()` with blocking delays.

2. **Q: Has `bleRxJsonBuffer` been observed growing without bound?** If BLE chunks arrive faster than they can be parsed, or if a malformed JSON object never completes, the buffer accumulates indefinitely in internal DRAM.

3. **Q: How reliable is the WiFi connection at the deployment site?** If WiFi disconnects/reconnects frequently, the AsyncWebServer + AsyncTCP reallocates socket buffers each time, contributing to fragmentation.

4. **Q: Is the ESP32-C6 relay actually used in production?** If not, the `RefreshRelayAsset` calls and their associated memory could be removed.

5. **Q: Does the device connect to both WiFi STA and run in AP mode simultaneously?** `WiFi.mode(WIFI_AP_STA)` enables both. Each mode consumes separate buffers.

6. **Q: What BLE MTU size is negotiated?** Larger MTU means fewer chunks but also larger internal buffers. The `kBleJsonChunkBytes = 160` suggests a conservative size; actual MTU may be 23-517 bytes.
