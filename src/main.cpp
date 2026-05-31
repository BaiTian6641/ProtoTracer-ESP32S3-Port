// Select target controller via build flag: -DTASESP32S3 or -DTASESP32P4.
// Default to ESP32-S3 when none specified.
#if !defined(TASESP32S3) && !defined(TASESP32P4)
#define TASESP32S3
#endif
#define ELEGANTOTA_USE_ASYNC_WEBSERVER 1
#define NEW_GESTURE
// #define NEW_HUB75
//  #define PRINTINFO
#define VERBOSE_STARTUP
// #define LANG_CN

#include <Arduino.h>
#include <string>

uint8_t maxBrightness = 50;
uint8_t maxAccentBrightness = 100;

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#if defined(TASESP32S3)
#include "Controllers/TasESP32S3KitV1.h"
#elif defined(TASESP32P4)
#include "Controllers/TasESP32P4KitV0.h"
#endif
#include "Animation/JsonDrivenProtogenAnimation.h"
#include <Adafruit_NeoPixel.h>
#if defined(__has_include)
  #if __has_include(<ElegantOTAPro.h>)
    #include <ElegantOTAPro.h>
    #define USING_ELEGANTOTA_PRO 1
  #elif __has_include(<ElegantOTA.h>)
    #include <ElegantOTA.h>
    #define USING_ELEGANTOTA 1
  #else
    #warning "Neither ElegantOTAPro nor ElegantOTA found — OTA portal functions will be disabled"
  #endif
#else
  /* Fallback for compilers without __has_include: assume ElegantOTAPro is available. */
  #include <ElegantOTAPro.h>
  #define USING_ELEGANTOTA_PRO 1
#endif
#include <M5Unified.h>
#include <M5UnitGLASS2.h>
#include <Wire.h>
#include <LittleFS.h>
#include <ProtoGC.h>
#include "Network/FaceModelUpdater.h"
#include "Network/FirmwareUpdater.h"
#include "Network/UserConfigManager.h"

#ifndef ANIM_RENDER_PIPELINE
#define ANIM_RENDER_PIPELINE 1
#endif

#if ANIM_RENDER_PIPELINE
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#ifndef ANIM_TASK_CORE
#define ANIM_TASK_CORE 0
#endif

#ifndef ANIM_TASK_PRIORITY
#define ANIM_TASK_PRIORITY 1
#endif

#ifndef ANIM_TASK_STACK_BYTES
#define ANIM_TASK_STACK_BYTES 6144
#endif
#endif

#ifdef LANG_CN
#define TXT(en, cn) cn
#else
#define TXT(en, cn) en
#endif

#ifdef USE_TOKEN_AUTH
#include "Auth/TassAuthToken.h"
#else
#include "Auth/AuthToken.h"
#endif

UserConfig userConfig;
FaceUpdateConfig faceUpdateConfig;

std::string user_name;
std::string BLE_TX2_UUID;
std::string BLE_RX2_UUID;

uint8_t User_R;
uint8_t User_G;
uint8_t User_B;

#ifdef VERBOSE_STARTUP
constexpr bool kVerboseStartup = true;
#else
constexpr bool kVerboseStartup = false;
#endif

#ifndef PROTOTRACER_FW_VERSION
#define PROTOTRACER_FW_VERSION "1.1.1"
#endif

#ifndef PROTOTRACER_FW_MANIFEST
  #if defined(TASESP32P4)
    #define PROTOTRACER_FW_MANIFEST "esp32p4.json"
  #else
    #define PROTOTRACER_FW_MANIFEST "esp32s3.json"
  #endif
#endif

constexpr const char *kFirmwareVersion = PROTOTRACER_FW_VERSION;
constexpr const char *kFirmwareManifest = PROTOTRACER_FW_MANIFEST;

static uint32_t gOtaButtonDownAtMs = 0;
static uint32_t gOtaButtonLastTriggerMs = 0;

#ifdef TASESP32S3
extern VirtualMatrixPanel *virtualDisp;
#endif

AsyncWebServer server(80);

// Controller selection per target
#if defined(TASESP32S3)
M5UnitGLASS2 display = M5UnitGLASS2(41, 42, 400000); // SDA, SCL, FREQ
TasESP32S3KitV1 controller = TasESP32S3KitV1(maxBrightness);
Adafruit_NeoPixel nowpixels(1, 45, NEO_GRB + NEO_KHZ800);
#elif defined(TASESP32P4)
M5UnitGLASS2 display = M5UnitGLASS2(47, 48, 400000); // SDA, SCL, FREQ
TasESP32P4KitV0 controller = TasESP32P4KitV0(maxBrightness);
Adafruit_NeoPixel nowpixels(1, 20, NEO_GRB + NEO_KHZ800);
#endif

JsonDrivenProtogenAnimation animation = JsonDrivenProtogenAnimation();

// Copy completed animation vertices from all scene objects to their render buffers.
// Defined unconditionally so both the pipeline task and the single-core fallback
// publish the animated geometry before Camera reads GetRenderTriangleGroup().
static void PublishSceneVertices(Scene* scene) {
    if (!scene) return;
    Object3D** objs = scene->GetObjects();
    const unsigned int count = scene->GetObjectCount();
    for (unsigned int i = 0; i < count; i++) {
        if (objs[i]) {
            objs[i]->PublishVertices();
        }
    }
}

#if ANIM_RENDER_PIPELINE
static SemaphoreHandle_t gAnimDoneSemaphore = nullptr;
static SemaphoreHandle_t gRenderDoneSemaphore = nullptr;
static TaskHandle_t gAnimTaskHandle = nullptr;
static volatile float gAnimRatio = 0.0f;
static volatile bool gAnimTaskStop = false;
static volatile bool gPipelineActive = false;

// Animation worker: runs UpdateTime() on ANIM_TASK_CORE, publishes the finished
// geometry snapshot, then signals the render core. Render waits for this signal
// so material/effect state is not raced by an unsafe whole-scene overlap.
static void AnimationTask(void*) {
    for (;;) {
        // Wait for the main loop to request the next animation frame.
        if (xSemaphoreTake(gRenderDoneSemaphore, portMAX_DELAY) != pdTRUE) continue;

        if (gAnimTaskStop) {
            xSemaphoreGive(gAnimDoneSemaphore);
            break;
        }

        const float ratio = gAnimRatio;
        animation.UpdateTime(ratio);

        // Publish stable vertex snapshots for the render core
        PublishSceneVertices(animation.GetScene());

        xSemaphoreGive(gAnimDoneSemaphore);
    }
    vTaskDelete(nullptr);
}
#endif

// Face model updater config is defined here for clarity; logic lives in FaceModelUpdater.*

const char *index_html = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <title>Ruby Protogen Web OTA Portal</title>
  <meta http-equiv="refresh" content="7; url='http://192.168.4.1/update/'" />
</head>
<body>
<h2>You will be redirect to firmware upload page shortly!</h2>
<p>Please access http://192.168.4.1/update/ to update firmware.</p>
</body>
</html>
)rawliteral";

constexpr const char *kRemoteC6ManifestFilename = "remote-esp32c6.json";
constexpr const char *kRemoteC6FirmwareFilename = "remote-firmware.bin";
constexpr const char *kRelayManifestCachePath = "/relay_remote-esp32c6.json";
constexpr const char *kRelayFirmwareCachePath = "/relay_remote-firmware.bin";

bool gRelayRoutesRegistered = false;
bool gRuntimeServerStarted = false;
bool gControllerInitialized = false;
static bool gRelayRefreshPending = false;

// Forward declaration — defined below
bool RefreshRelayAsset(const char *remoteFilename, const char *localPath);

// Non-blocking relay asset serve: returns cached copy immediately if available,
// defers remote refresh to main loop via gRelayRefreshPending flag.
bool ServeRelayAssetCached(AsyncWebServerRequest *request, const char *remoteFilename, const char *localPath, const char *contentType)
{
  // Serve cached copy immediately if it exists (fast path, no HTTP blocking)
  if (LittleFS.exists(localPath))
  {
    request->send(LittleFS, localPath, contentType, false);
    // Defer background refresh to main loop
    gRelayRefreshPending = true;
    return true;
  }

  // No cache — must fetch now (blocking, but unavoidable on first request)
  if (RefreshRelayAsset(remoteFilename, localPath))
  {
    request->send(LittleFS, localPath, contentType, false);
    return true;
  }

  request->send(502, "application/json", "{\"error\":\"Relay asset unavailable\"}");
  return false;
}

// Called from main loop to refresh stale relay assets in background
void RefreshRelayAssetsIfPending()
{
  if (!gRelayRefreshPending) return;
  gRelayRefreshPending = false;

  // Best-effort background refresh — failures are non-fatal (next request retries)
  RefreshRelayAsset(kRemoteC6ManifestFilename, kRelayManifestCachePath);
  RefreshRelayAsset(kRemoteC6FirmwareFilename, kRelayFirmwareCachePath);
}

bool RefreshRelayAsset(const char *remoteFilename, const char *localPath)
{
  if (remoteFilename == nullptr || localPath == nullptr)
  {
    return false;
  }

  RemoteFileSource sources[2];
  sources[0].baseUrl = user_config_gitee_base_url;
  sources[0].token = user_config_gitee_token;
  sources[0].authScheme = "Bearer ";
  sources[0].acceptHeader = gitee_accept_header;
  sources[0].name = "Gitee";
  sources[1].baseUrl = user_config_base_url;
  sources[1].token = user_config_github_token;
  sources[1].authScheme = "token ";
  sources[1].acceptHeader = nullptr;
  sources[1].name = "GitHub";

  RemoteFileSyncOptions options;
  options.verbose = kVerboseStartup;
  options.keepExistingWhenRemoteMd5Unavailable = false;

  int usedSourceIndex = -1;
  const bool synced = RemoteFileSync::SyncAny(
      sources,
      2,
      String(remoteFilename),
      String(localPath),
      options,
      &usedSourceIndex);

  if (synced)
  {
    Serial.printf("[INFO] Refreshed relay asset %s via source %d\n", remoteFilename, usedSourceIndex);
    return true;
  }

  return RemoteFileSync::EnsureFsMounted() && LittleFS.exists(localPath);
}

void RegisterC6RelayRoutes()
{
  if (gRelayRoutesRegistered)
  {
    return;
  }

  server.on("/api/relay/esp32c6/healthz", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "application/json", "{\"status\":\"ok\"}"); });

  server.on("/api/relay/esp32c6/manifest", HTTP_GET, [](AsyncWebServerRequest *request)
            {
              ServeRelayAssetCached(request, kRemoteC6ManifestFilename, kRelayManifestCachePath, "application/json"); });

  server.on("/api/relay/esp32c6/remote-firmware.bin", HTTP_GET, [](AsyncWebServerRequest *request)
            {
              ServeRelayAssetCached(request, kRemoteC6FirmwareFilename, kRelayFirmwareCachePath, "application/octet-stream"); });

  gRelayRoutesRegistered = true;
}

void EnsureRuntimeServerStarted()
{
  if (gRuntimeServerStarted)
  {
    return;
  }

  server.begin();
  gRuntimeServerStarted = true;
  Serial.println("[INFO] Runtime HTTP server started");
}

void EnsureControllerInitialized()
{
  if (gControllerInitialized)
  {
    return;
  }

  controller.Initialize();
  gControllerInitialized = true;
}

float FreeMem()
{
  uint32_t stackT;
  uint32_t heapT;

  // current position of the stack.
  stackT = (uint32_t)&stackT;

  void *heapPos = malloc(1);
  heapT = (uint32_t)heapPos;
  free(heapPos);

  float temp = stackT - heapT;

  return temp / 1000000.0f;
}

// Real heap telemetry: use these in logs instead of FreeMem().
inline size_t GetFreeInternalDRAM() {
  return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}
inline size_t GetLargestFreeInternalBlock() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
}
inline size_t GetFreePSRAM() {
  return heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

static void ProtoGcWarnHandler(protogc::HeapGuard::Level, size_t freeBytes, size_t largestBlock) {
  Serial.printf("[ProtoGC] WARN intFree=%u largestBlk=%u; running light collection\n",
                static_cast<unsigned>(freeBytes),
                static_cast<unsigned>(largestBlock));
  protogc::ProtoGC::collectLight("heap-warn");
}

static void ProtoGcCriticalHandler(protogc::HeapGuard::Level, size_t freeBytes, size_t largestBlock) {
  Serial.printf("[ProtoGC] CRITICAL intFree=%u largestBlk=%u; running emergency collection\n",
                static_cast<unsigned>(freeBytes),
                static_cast<unsigned>(largestBlock));
  protogc::ProtoGC::emergency("heap-critical");
}

void setup()
{
  // ProtoGC: cooperative dual-heap strategy.
  // - Internal DRAM stays explicit for DMA/ESP-DSP, with ProtoGC managing non-DMA app SRAM.
  // - PSRAM handles cold app allocations via managed segments, pools, and arenas.
  // Phase 1: boot allows both heaps during WiFi/downloads.
  heap_caps_malloc_extmem_enable(64);
  protogc::ProtoGC::begin();

  pinMode(OTA_BTN, INPUT_PULLUP);
  Serial.begin(115200);
  protogc::HeapGuard::onWarning(ProtoGcWarnHandler);
  protogc::HeapGuard::onCritical(ProtoGcCriticalHandler);
  Serial.println("/nStarting...");
  //Wire.begin(41, 42);
  Wire.begin(47, 48);
  delay(100);
  display.begin();
  display.setColorDepth(1);
  display.setEpdMode(epd_mode_t::epd_fastest);
  display.setRotation(1);
  display.setBrightness(45);

  nowpixels.begin();
  nowpixels.clear();
  nowpixels.show();
  delay(100);

  display.setFont(&fonts::efontCN_12);
  display.setTextSize(1);
  display.setTextScroll(true);
  display.setTextColor(TFT_WHITE);

  display.fillScreen(TFT_BLACK);
  // display.pushImage(22,4,84,40,FusionOpen_startup_logo);
  // display.progressBar(14,50,100,8,75);
  // display.display();
  // delay(2000);
  display.println(TXT("Starting...", "启动中..."));
  delay(1000);

#ifdef TASESP32S3
  // Reserve HUB75 DMA internal SRAM before WiFi/BLE/animation allocate from the same heap.
  EnsureControllerInitialized();
#endif

  if (!EnsureUserConfig(userConfig))
  {
    display.println(TXT("Config load fail", "配置加载失败"));
    display.display();
  }

  // Factory flash indicator: blink internal WS2812 red/blue if face model or animation config is missing.
  // Note: LittleFS already mounted by EnsureUserConfig() via RemoteFileSync::EnsureFsMounted()
  if (true)
  {
    const char *markerPath = "/factory_blink_done";

    String deviceFacePath = "/" + userConfig.device_id + String("_face.json");
    bool faceExists = LittleFS.exists(deviceFacePath) || LittleFS.exists("/universal_face.json");

    String animFilename = userConfig.user_animation.length() > 0 ? userConfig.user_animation : (userConfig.device_id + String("_animation.json"));
    String animPath = "/" + animFilename;
    bool animExists = LittleFS.exists(animPath) || LittleFS.exists("/example_animation.json");

    if ((!faceExists || !animExists) && !LittleFS.exists(markerPath))
    {
      // Blink red/blue a few times so factory staff can see firmware ran.
      nowpixels.setBrightness(150);
      const uint32_t red = nowpixels.Color(255, 0, 0);
      const uint32_t blue = nowpixels.Color(0, 0, 255);
      const int cycles = 6;
      for (int i = 0; i < cycles; ++i)
      {
        nowpixels.setPixelColor(0, (i % 2 == 0) ? red : blue);
        nowpixels.show();
        delay(300);
        yield(); // Feed watchdog during factory indicator blink
      }
      nowpixels.setPixelColor(0, 0);
      nowpixels.show();

      File m = LittleFS.open(markerPath, "w");
      if (m)
      {
        m.print("1");
        m.close();
      }
      nowpixels.setBrightness(45);
      delay(200);
    }
  }

  user_name = userConfig.username.c_str();

  // Show unique device ID for registration
  display.println(TXT("Device ID:", "设备ID:"));
  display.println(userConfig.device_id);
  display.display();
  delay(1500);

  if (digitalRead(OTA_BTN) == LOW)
  {
    EnsureControllerInitialized();
#ifdef TASESP32S3
    if (virtualDisp) {
      virtualDisp->clearScreen();
      virtualDisp->fillScreenRGB888(255, 255, 255);
    }
#endif
    WiFi.mode(WIFI_AP);
    WiFi.softAP(userConfig.ota_ssid.c_str(), userConfig.ota_password.c_str());
    display.clearDisplay();
#ifdef LANG_CN
    display.println("进入无线OTA模式！");
#else
    display.println(TXT("Entering Wireless OTA Mode", "进入无线OTA模式"));
#endif
    display.display();
#ifdef LANG_CN
    display.println("请连接WiFi：");
    display.println(userConfig.ota_ssid);
    display.display();
    display.println("密码：");
    display.println(userConfig.ota_password);
    display.display();
    display.println("访问IP：192.168.4.1");
    display.display();
#else
    display.println(TXT("Please connect to:", "请连接WiFi:"));
    display.println(userConfig.ota_ssid);
    display.display();
    display.println(TXT("Password:", "密码:"));
    display.println(userConfig.ota_password);
    display.display();
    display.println(TXT("IP: 192.168.4.1", "访问IP：192.168.4.1"));
    display.display();
#endif
    Serial.println("");

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
              { request->send(200, "text/html", index_html); });

#if defined(USING_ELEGANTOTA_PRO)
    // You can also enable authentication by uncommenting the below line.
    ElegantOTA.setAuth(userConfig.username.c_str(), userConfig.ota_password.c_str());

    ElegantOTA.setTitle("Ruby Protogen Studio OTA Portal"); // Set OTA Webpage Title

    ElegantOTA.setID(userConfig.device_id.c_str()); // Set Hardware ID
    ElegantOTA.setFWVersion(kFirmwareVersion);       // Set Firmware Version

    ElegantOTA.begin(&server); // Start ElegantOTA
#elif defined(USING_ELEGANTOTA)
    ElegantOTA.begin(&server); // Start ElegantOTA
#else
    // Fallback: register an informative page when ElegantOTA is not available.
    server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(501, "text/plain", "OTA portal unavailable: ElegantOTA library not found.");
    });
#endif

    server.begin();
    while (true)
    {
      nowpixels.setPixelColor(0, nowpixels.Color(0, 140, 170));
      nowpixels.show();
#if defined(USING_ELEGANTOTA_PRO) || defined(USING_ELEGANTOTA)
      ElegantOTA.loop();
#else
      delay(100);
#endif
    }
  }

  WiFi.mode(WIFI_AP_STA);

  bool wifiConnected = ConnectWifiWithNetWizard(userConfig, server, 15000, &display);
  if (!wifiConnected)
  {
    Serial.println("[WARN] WiFi not connected via NetWizard; downloads may fail");
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println(TXT("WiFi not connected", "WiFi未连接"));
    display.println(TXT("Continuing startup", "继续启动"));
    display.display();
  }

  // WiFi-dependent relay routes are not registered — WiFi is torn down after
  // firmware/face/animation downloads to free internal DRAM for BLE.

  // Attempt to pull remote user_config.json named by device_id, preferring the lower-latency remote.
  RemoteFileSource userConfigSources[2];
  userConfigSources[0].baseUrl = user_config_gitee_base_url;
  userConfigSources[0].token = user_config_gitee_token;
  userConfigSources[0].authScheme = "Bearer ";
  userConfigSources[0].acceptHeader = gitee_accept_header;
  userConfigSources[0].name = "Gitee";
  userConfigSources[1].baseUrl = user_config_base_url;
  userConfigSources[1].token = user_config_github_token;
  userConfigSources[1].authScheme = "token ";
  userConfigSources[1].acceptHeader = nullptr;
  userConfigSources[1].name = "GitHub";

  bool userConfigFetched = DownloadUserConfigFromSources(userConfigSources,
                                                         2,
                                                         userConfig,
                                                         kVerboseStartup ? true : false,
                                                         &display);
  if (!userConfigFetched)
  {
    Serial.println("[WARN] user_config.json download failed on all configured remotes");
  }
  protogc::ProtoGC::collectFull("post-user-config");

  user_name = userConfig.username.c_str();
  BLE_TX2_UUID = userConfig.ble_tx_uuid.c_str();
  BLE_RX2_UUID = userConfig.ble_rx_uuid.c_str();

  User_R = userConfig.user_r;
  User_G = userConfig.user_g;
  User_B = userConfig.user_b;

  FirmwareUpdateConfig firmwareUpdateConfig;
  firmwareUpdateConfig.primarySource.baseUrl = user_config_gitee_base_url;
  firmwareUpdateConfig.primarySource.token = user_config_gitee_token;
  firmwareUpdateConfig.primarySource.authScheme = "Bearer ";
  firmwareUpdateConfig.primarySource.acceptHeader = gitee_accept_header;
  firmwareUpdateConfig.primarySource.name = "Gitee";
  firmwareUpdateConfig.primaryName = "Gitee";
  firmwareUpdateConfig.fallbackSource.baseUrl = user_config_base_url;
  firmwareUpdateConfig.fallbackSource.token = user_config_github_token;
  firmwareUpdateConfig.fallbackSource.authScheme = "token ";
  firmwareUpdateConfig.fallbackSource.acceptHeader = nullptr;
  firmwareUpdateConfig.fallbackSource.name = "GitHub";
  firmwareUpdateConfig.fallbackName = "GitHub";
  firmwareUpdateConfig.manifestFilename = kFirmwareManifest;
  firmwareUpdateConfig.currentVersion = kFirmwareVersion;
  firmwareUpdateConfig.display = &display;
  firmwareUpdateConfig.verbose = kVerboseStartup;

  const FirmwareUpdateResult firmwareUpdateResult = FirmwareUpdater::CheckAndUpdate(firmwareUpdateConfig);
  if (firmwareUpdateResult == FirmwareUpdateResult::Failed)
  {
    Serial.println("[WARN] Auto firmware update check failed; continuing startup");
  }
  protogc::ProtoGC::collectFull("post-firmware-check");

  FaceUpdateConfig faceConfigs[2] = {
    {userConfig.wifi_ssid.c_str(), userConfig.wifi_password.c_str(), user_config_gitee_base_url, user_config_gitee_token, gitee_accept_header, "Bearer ", "Gitee"},
    {userConfig.wifi_ssid.c_str(), userConfig.wifi_password.c_str(), user_config_base_url, user_config_github_token, nullptr, "token ", "GitHub"}
  };

  // Ensure face model is present before animation startup, preferring the lower-latency remote.
  bool faceReady = EnsureFaceModelJson(faceConfigs, 2, userConfig.device_id, display, kVerboseStartup);

  if (faceReady)
  {
    Serial.println("[INFO] face model is ready");
  }
  else
  {
    Serial.println("[WARN] face model is not available; animation may fail");
    delay(10);
    ESP.restart();
  }
  protogc::ProtoGC::collectFull("post-face-sync");

#ifndef VERBOSE_STARTUP
  display.clearDisplay();
  display.pushImage(22, 4, 84, 40, FusionOpen_startup_logo);
  display.progressBar(14, 50, 100, 8, 0);
  display.display();
#endif

  animation.Initialize(userConfig,
                       user_config_base_url,
                       user_config_gitee_base_url,
                       user_config_github_token,
                       user_config_gitee_token,
                       &display,
                       kVerboseStartup);
  protogc::ProtoGC::collectFull("post-animation-init");

  // All WiFi-dependent work (config download, firmware check, face model sync,
  // animation JSON download) is complete. Tear down WiFi and its HTTP server
  // to free ~50-70 KB of internal DRAM for the BLE controller.
  Serial.println("[INFO] Shutting down WiFi to free memory for BLE...");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(200);
  Serial.printf("[INFO] WiFi off — intFree=%u largestBlk=%u psramFree=%u\n",
                GetFreeInternalDRAM(), GetLargestFreeInternalBlock(), GetFreePSRAM());

  // Phase 2 — runtime: lock ALL future malloc() to PSRAM.
  // Internal DRAM is now a write-once DMA pool. ProtoGC pools handle
  // fragmentation-free small allocations; arenas handle JSON/String scopes.
  protogc::ProtoGC::lockMallocToPsram(0);
  protogc::ProtoGC::collectFull("post-wifi-stop");
  Serial.printf("[INFO] ProtoGC locked — intFree=%u largestBlk=%u\n",
                GetFreeInternalDRAM(), GetLargestFreeInternalBlock());

  // Now initialize BLE + gesture sensor on a clean heap.
  animation.InitializeMenuPeripherals(17, animation.GetBoopSensorThreshold());
  protogc::ProtoGC::collectLight("post-ble-init");

  EnsureControllerInitialized();

#if ANIM_RENDER_PIPELINE
  // Enable double-buffered vertex arrays on all scene objects. Animation keeps
  // writing the mutable mesh; Camera reads the explicit render snapshot.
  {
    Scene* scene = animation.GetScene();
    if (scene) {
      Object3D** objs = scene->GetObjects();
      const unsigned int count = scene->GetObjectCount();
      for (unsigned int i = 0; i < count; i++) {
        if (objs[i]) objs[i]->EnableDoubleBuffer();
      }
    }
  }

  // Create pipeline semaphores and launch the animation task on ANIM_TASK_CORE.
  gAnimDoneSemaphore = xSemaphoreCreateBinary();
  gRenderDoneSemaphore = xSemaphoreCreateBinary();
  if (gAnimDoneSemaphore && gRenderDoneSemaphore) {
    gAnimTaskStop = false;
    if (xTaskCreatePinnedToCore(AnimationTask, "ProtoAnim", ANIM_TASK_STACK_BYTES,
                                nullptr, ANIM_TASK_PRIORITY, &gAnimTaskHandle,
                                ANIM_TASK_CORE) == pdPASS) {
      gPipelineActive = true;
      Serial.printf("[PIPELINE] Animation task started on core %d (serialized publish mode)\n", ANIM_TASK_CORE);
    } else {
      Serial.printf("[PIPELINE] Failed to create animation task (stack=%u, intFree=%u, largestBlk=%u); using single-core\n",
                    ANIM_TASK_STACK_BYTES,
                    GetFreeInternalDRAM(),
                    GetLargestFreeInternalBlock());
      gPipelineActive = false;
    }
  } else {
    Serial.println("[PIPELINE] Failed to create semaphores; using single-core fallback");
    gPipelineActive = false;
  }
#endif

#ifndef VERBOSE_STARTUP
  display.progressBar(14, 50, 100, 8, 100);
#endif
  delay(1000);
  display.clear();
}

void loop()
{
  const uint32_t now = millis();
  const bool otaPressed = (digitalRead(OTA_BTN) == LOW);

  if (otaPressed)
  {
    if (gOtaButtonDownAtMs == 0)
    {
      gOtaButtonDownAtMs = now;
    }
  }
  else
  {
    gOtaButtonDownAtMs = 0;
  }

  const bool otaLongPress = (gOtaButtonDownAtMs != 0) && ((now - gOtaButtonDownAtMs) >= 1200);
  const bool otaCooldownElapsed = (now - gOtaButtonLastTriggerMs) >= 30000;

#ifdef TASESP32S3
  if (otaLongPress && otaCooldownElapsed)
  {
    gOtaButtonLastTriggerMs = now;
    if (virtualDisp) {
      virtualDisp->clearScreen();
      virtualDisp->fillScreenRGB888(255, 255, 255);
    }
    if (WiFi.getMode() != WIFI_AP)
    {
      WiFi.mode(WIFI_AP);
      WiFi.softAP(userConfig.ota_ssid.c_str(), userConfig.ota_password.c_str());
    }
    Serial.println("");
  }
  // controller.SetAccentBrightness(animation.GetAccentBrightness() * 25 + 5);
  // controller.SetBrightness(powf(animation.GetBrightness() + 3, 2) / 3);
  float ratio = (float)(millis() % 5000) / 5000.0f;
  controller.SetBrightness(animation.GetBrightness());

  // Run Menu/BLE/gesture update on core 1 before animation to avoid I2C
  // contention with the animation task (which may run on core 0).
  animation.MenuUpdate();

#if ANIM_RENDER_PIPELINE
  if (gPipelineActive) {
    // Run animation on core 0 and publish a full geometry snapshot before render.
    // Keeping the handoff serialized avoids races with material/effect state.
    gAnimRatio = ratio;
    xSemaphoreGive(gRenderDoneSemaphore);

    // Wait for the animation task to finish this frame before rasterization.
    xSemaphoreTake(gAnimDoneSemaphore, portMAX_DELAY);
  } else
#endif
  {
    yield(); // Feed watchdog before animation update
    animation.UpdateTime(ratio);
    // Publish the completed frame to the render buffer so Camera reads
    // the animated geometry, matching the pipeline path's handoff.
    PublishSceneVertices(animation.GetScene());
    yield(); // Feed watchdog before render
  }

  controller.Render(animation.GetScene());
  yield(); // Feed watchdog before display
#elif defined(TASESP32P4)
  // TODO: add panel-clearing logic for P4 HUB75 if needed
  if (otaLongPress && otaCooldownElapsed)
  {
    gOtaButtonLastTriggerMs = now;
    if (WiFi.getMode() != WIFI_AP)
    {
      WiFi.mode(WIFI_AP);
      WiFi.softAP(userConfig.ota_ssid.c_str(), userConfig.ota_password.c_str());
    }
  }
  float ratio = (float)(millis() % 5000) / 5000.0f;
  controller.SetBrightness(animation.GetBrightness());
  yield(); // Feed watchdog before animation update
  animation.UpdateTime(ratio);
  yield(); // Feed watchdog before render
  controller.Render(animation.GetScene());
  yield(); // Feed watchdog before display
#else
  Serial.print("not defined");
#endif

  // Yield to background WiFi/BLE/LWIP tasks to reduce starvation risk under sustained rendering load.
  delay(1);

  // ProtoGC: poll internal DRAM health every ~5 seconds
  {
    static uint32_t lastGcPollMs = 0;
    if (now - lastGcPollMs >= 5000) {
      lastGcPollMs = now;
      protogc::ProtoGC::poll();
    }
  }

  // controller.

  controller.Display();
  yield(); // Feed watchdog after display update

  // Background relay asset refresh (non-blocking — deferred from async HTTP handlers)
  RefreshRelayAssetsIfPending();

#ifdef PRINTINFO
  static uint32_t lastPrintMs = 0;
  uint32_t nowPrint = millis();
  // Sample every ~2 seconds to avoid serial bottleneck
  if (nowPrint - lastPrintMs >= 2000) {
    lastPrintMs = nowPrint;
    Serial.printf("anim=%.2fms render=%.2fms intFree=%u largestBlk=%u psramFree=%u micFrames=%lu micDrops=%lu micStack=%lu\n",
        animation.GetAnimationTime() * 1000.0f,
        controller.GetRenderTime() * 1000.0f,
        GetFreeInternalDRAM(),
        GetLargestFreeInternalBlock(),
        GetFreePSRAM(),
        static_cast<unsigned long>(MicrophoneFourierIT::GetProcessedFrameCount()),
        static_cast<unsigned long>(MicrophoneFourierIT::GetSamplerDropCount()),
        static_cast<unsigned long>(MicrophoneFourierIT::GetTaskStackHighWater()));
  }
#endif
}
