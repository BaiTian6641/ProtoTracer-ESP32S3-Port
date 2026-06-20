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

// ── Display auto-detection: include all available drivers ──
#if __has_include(<M5UnitGLASS2.h>)
  #include <M5UnitGLASS2.h>
#endif
#if __has_include(<M5UnitGLASS.h>)
  #include <M5UnitGLASS.h>
#endif
#if __has_include(<M5UnitLCD.h>)
  #include <M5UnitLCD.h>
#endif
#if __has_include(<M5UnitOLED.h>)
  #include <M5UnitOLED.h>
#endif

// Heap-allocated display — type determined at runtime via I2C probe.
M5GFX *display = nullptr;
#include <Wire.h>
#include <LittleFS.h>
#include <ProtoGC.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "Flash/Icons/Icons.h"
#include "Network/FaceModelUpdater.h"
#include "Network/FirmwareUpdater.h"
#include "Network/RemoteFileSync.h"
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
#define ANIM_TASK_STACK_BYTES 8192
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
#define PROTOTRACER_FW_VERSION "1.2.6"
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

// ── Background download task (FreeRTOS) ──
// Runs during early rendering frames so the display starts before all
// remote assets are synced.  Once complete, WiFi is torn down and BLE
// initialised in the main loop.
static TaskHandle_t gBgDownloadTask = nullptr;
static volatile bool gBgDownloadsDone = false;
static volatile bool gBgDownloadsOk = true;
static bool gBgDisplayResetDone = false;
static uint32_t gBgDownloadStartMs = 0;
static constexpr uint32_t kBgDownloadTimeoutMs = 120000; // 120s safety timeout
static constexpr UBaseType_t kBgTaskPriority = 0; // idle priority

// Startup phase: lets the main loop defer WiFi teardown + BLE init
// until background downloads finish (or timeout).
enum class StartupPhase : uint8_t {
    Downloading,      // background task running, WiFi still up
    WifiTeardown,     // signal WiFi + server shutdown
    BleInit,          // initialise BLE on clean heap
    Running           // normal operation
};
static volatile StartupPhase gStartupPhase = StartupPhase::Downloading;

// Sources for background download — captured from setup() probe result.
static const char *gBgPrimaryBase = nullptr;
static const char *gBgFallbackBase = nullptr;
static const char *gBgPrimaryToken = nullptr;
static const char *gBgFallbackToken = nullptr;
static const char *gBgPrimaryAccept = nullptr;
static const char *gBgFallbackAccept = nullptr;
static const char *gBgPrimaryAuth = nullptr;
static const char *gBgFallbackAuth = nullptr;
static const char *gBgPrimaryName = nullptr;
static const char *gBgFallbackName = nullptr;
static String gBgDeviceId;
static String gBgWifiSsid;
static String gBgWifiPass;

static void BackgroundDownloadTask(void *)
{
    Serial.println("[BG] Download task started");
    // User config
    {
        RemoteFileSource sources[2];
        sources[0].baseUrl = gBgPrimaryBase; sources[0].token = gBgPrimaryToken;
        sources[0].authScheme = gBgPrimaryAuth; sources[0].acceptHeader = gBgPrimaryAccept;
        sources[0].name = gBgPrimaryName;
        sources[1].baseUrl = gBgFallbackBase; sources[1].token = gBgFallbackToken;
        sources[1].authScheme = gBgFallbackAuth; sources[1].acceptHeader = gBgFallbackAccept;
        sources[1].name = gBgFallbackName;
        DownloadUserConfigFromSources(sources, 2, userConfig, false, nullptr);
    }
    // Face model
    {
        FaceUpdateConfig fc[2] = {
            {gBgWifiSsid.c_str(), gBgWifiPass.c_str(),
             gBgPrimaryBase, gBgPrimaryToken, gBgPrimaryAccept, gBgPrimaryAuth, gBgPrimaryName},
            {gBgWifiSsid.c_str(), gBgWifiPass.c_str(),
             gBgFallbackBase, gBgFallbackToken, gBgFallbackAccept, gBgFallbackAuth, gBgFallbackName}
        };
        EnsureFaceModelJson(fc, 2, gBgDeviceId, nullptr, false); // no I2C — bg task runs on arbitrary core
    }
    Serial.println("[BG] Download task complete");
    gBgDownloadsDone = true;
    gBgDownloadsOk = true;
    vTaskDelete(nullptr);
}

#ifdef TASESP32S3
extern VirtualMatrixPanel *virtualDisp;
#endif

AsyncWebServer server(80);

// ── Runtime display type + gesture I2C bus ──
enum class DisplayType : uint8_t { Unknown, GLASS2, GLASS, UnitLCD, UnitOLED };
static DisplayType gDisplayType = DisplayType::Unknown;
bool gColoredPreview = false; // true for UnitLCD color mode

// Try init each display driver in priority order; returns true on first success.
static bool DetectDisplay(uint8_t sda, uint8_t scl, uint32_t freq)
{
#if __has_include(<M5UnitGLASS2.h>)
  {
    auto *d = new M5UnitGLASS2(sda, scl, freq);
    d->begin();
    // Quick probe: clear and draw a test pixel
    d->fillScreen(TFT_BLACK);
    d->display();
    if (d->width() >= 64 && d->height() >= 32)
    {
      display = d;
      gDisplayType = DisplayType::GLASS2;
      Serial.println("[DISP] Detected M5UnitGLASS2 (dual I2C)");
      return true;
    }
    delete d;
  }
#endif
#if __has_include(<M5UnitGLASS.h>)
  {
    auto *d = new M5UnitGLASS(sda, scl, freq);
    d->begin();
    d->fillScreen(TFT_BLACK);
    d->display();
    if (d->width() >= 64 && d->height() >= 32)
    {
      display = d;
      gDisplayType = DisplayType::GLASS;
      Serial.println("[DISP] Detected M5UnitGLASS (single I2C)");
      return true;
    }
    delete d;
  }
#endif
#if __has_include(<M5UnitLCD.h>)
  {
    auto *d = new M5UnitLCD(sda, scl, freq);
    d->begin();
    d->fillScreen(TFT_BLACK);
    d->display();
    if (d->width() >= 64 && d->height() >= 32)
    {
      display = d;
      gDisplayType = DisplayType::UnitLCD;
      gColoredPreview = true; // color-capable LCD
      Serial.println("[DISP] Detected M5UnitLCD (color mode)");
      return true;
    }
    delete d;
  }
#endif
#if __has_include(<M5UnitOLED.h>)
  {
    auto *d = new M5UnitOLED(sda, scl, freq);
    d->begin();
    d->fillScreen(TFT_BLACK);
    d->display();
    if (d->width() >= 64 && d->height() >= 32)
    {
      display = d;
      gDisplayType = DisplayType::UnitOLED;
      Serial.println("[DISP] Detected M5UnitOLED (fallback)");
      return true;
    }
    delete d;
  }
#endif
  return false;
}

// ── Gesture I2C bus: shared with display for GLASS2, separate for others ──
TwoWire *gGestureWire = &Wire;

// Controller selection per target
#if defined(TASESP32S3)
TasESP32S3KitV1 controller = TasESP32S3KitV1(maxBrightness);
Adafruit_NeoPixel nowpixels(1, 45, NEO_GRB + NEO_KHZ800);
#elif defined(TASESP32P4)
TasESP32P4KitV0 controller = TasESP32P4KitV0(maxBrightness);
Adafruit_NeoPixel nowpixels(1, 20, NEO_GRB + NEO_KHZ800);
#endif

JsonDrivenProtogenAnimation animation = JsonDrivenProtogenAnimation();

namespace
{
  constexpr const char *kUserConfigPath = "/user_config.json";

  bool HasLocalFaceAsset(const UserConfig &config)
  {
    const String deviceFaceFile = "/" + config.device_id + "_face.json";
    return LittleFS.exists(deviceFaceFile) || LittleFS.exists("/universal_face.json");
  }

  bool HasLocalAnimationAsset(const UserConfig &config)
  {
    const String animFile = "/" + (config.user_animation.length() > 0
                                        ? config.user_animation
                                        : config.device_id + "_animation.json");
    return LittleFS.exists(animFile) || LittleFS.exists("/example_animation.json");
  }

  void ShowOfflineStartupFailure(bool hasConfig, bool hasFace, bool hasAnimation)
  {
    Serial.printf("[FATAL] Offline boot blocked: config=%d face=%d animation=%d\n",
                  hasConfig ? 1 : 0,
                  hasFace ? 1 : 0,
                  hasAnimation ? 1 : 0);

    if (!display)
    {
      return;
    }

    display->fillScreen(TFT_BLACK);
    DrawSadComputerStartupIcon(display, 36, 2, 3, TFT_WHITE, TFT_BLACK);
    display->setTextColor(TFT_WHITE, TFT_BLACK);
    display->setCursor(6, 72);
    display->println(TXT("Offline boot blocked", "离线启动失败"));
    if (!hasConfig) display->println(TXT("Missing: user_config", "缺少: user_config"));
    if (!hasFace) display->println(TXT("Missing: face model", "缺少: 面部模型"));
    if (!hasAnimation) display->println(TXT("Missing: animation", "缺少: 动画配置"));
    display->println("0F00 0042");
    display->display();
  }

  [[noreturn]] void HaltStartupOfflineFailure(bool hasConfig, bool hasFace, bool hasAnimation)
  {
    ShowOfflineStartupFailure(hasConfig, hasFace, hasAnimation);
    nowpixels.setBrightness(80);
    while (true)
    {
      nowpixels.setPixelColor(0, nowpixels.Color(120, 0, 0));
      nowpixels.show();
      delay(250);
      nowpixels.setPixelColor(0, 0);
      nowpixels.show();
      delay(450);
      yield();
    }
  }
}

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

bool gControllerInitialized = false;

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

static void DoWifiTeardownAndBleInit()
{
  // Guard against double execution — the phase machine in loop() may call
  // this after setup() already ran it for the non-fast (offline) path.
  static bool sAlreadyDone = false;
  if (sAlreadyDone) {
    Serial.println("[INFO] WiFi teardown already done, skipping");
    return;
  }
  sAlreadyDone = true;

  Serial.println("[INFO] Shutting down WiFi to free memory for BLE...");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(200);

  server.reset();
  server.end();
  Serial.printf("[INFO] WiFi + HTTP server off — intFree=%u largestBlk=%u psramFree=%u\n",
                GetFreeInternalDRAM(), GetLargestFreeInternalBlock(), GetFreePSRAM());

  protogc::ProtoGC::lockMallocToPsram(0);
  protogc::ProtoGC::collectFull("post-wifi-stop");
  Serial.printf("[INFO] ProtoGC locked — intFree=%u largestBlk=%u\n",
                GetFreeInternalDRAM(), GetLargestFreeInternalBlock());

  animation.InitializeMenuPeripherals(17, animation.GetBoopSensorThreshold());
  protogc::ProtoGC::collectLight("post-ble-init");
  Serial.println("[INFO] BLE initialised");
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
  pinMode(21, OUTPUT);  // LED/backlight — set early to avoid GPIO spam before Menu::Initialize
  Serial.begin(115200);
  protogc::HeapGuard::onWarning(ProtoGcWarnHandler);
  protogc::HeapGuard::onCritical(ProtoGcCriticalHandler);
  Serial.println("/nStarting...");
  delay(100);

  // ── Display auto-detection ──
#if defined(TASESP32S3)
  if (!DetectDisplay(41, 42, 400000))
#elif defined(TASESP32P4)
  if (!DetectDisplay(47, 48, 400000))
#endif
  {
    Serial.println("[DISP] No display detected; Continue with headless");
  }

  // ── Gesture I2C bus setup ──
  // Only M5UnitGLASS2 shares the I2C bus with the gesture sensor.
  // All other displays (M5UnitGLASS, UnitLCD, UnitOLED) need a separate Wire1.
  if (gDisplayType == DisplayType::GLASS2)
  {
    gGestureWire = &Wire;
    Serial.println("[INFO] Gesture sharing display I2C bus (M5UnitGLASS2)");
  }
  else
  {
    Wire1.begin(38, 39); // SDA=38, SCL=39
    gGestureWire = &Wire1;
    Serial.println("[INFO] Wire1 initialised for gesture (GPIO38=SDA, GPIO39=SCL)");
  }

  // Also set colored preview for UnitLCD
  if (gDisplayType == DisplayType::UnitLCD)
  {
    gColoredPreview = true;
  }

  // I2C timeout: prevents indefinite hang if PAJ7620 or display
  // NACKs or holds SDA low. 50ms is long enough for a 1KB framebuffer
  // transfer at 400kHz (~25ms) with margin for retries.
  Wire.setTimeOut(50);

  if (display)
  {
    // Color depth: 1-bit monochrome for GLASS/OLED, color for UnitLCD
    display->setColorDepth(gColoredPreview ? 16 : 1);
    display->setEpdMode(epd_mode_t::epd_fastest);
    display->setRotation(1);
    display->setBrightness(45);

    nowpixels.begin();
    nowpixels.clear();
    nowpixels.show();
    delay(100);

    display->setFont(&fonts::efontCN_12);
    display->setTextSize(gColoredPreview ? 2 : 1);
    display->setTextScroll(true);
    display->setTextColor(TFT_WHITE);

    display->fillScreen(TFT_BLACK);
    // display->pushImage(22,4,84,40,FusionOpen_startup_logo);
    // display->progressBar(14,50,100,8,75);
    // display->display();
    // delay(2000);
    display->println(TXT("Starting...", "启动中..."));
    delay(1000);
  }

#ifdef TASESP32S3
  // Reserve HUB75 DMA internal SRAM before WiFi/BLE/animation allocate from the same heap.
  EnsureControllerInitialized();
#endif

  const bool fsReady = RemoteFileSync::EnsureFsMounted();
  const bool hadLocalUserConfig = fsReady && LittleFS.exists(kUserConfigPath);

  if (!EnsureUserConfig(userConfig))
  {
    if (display) { display->println(TXT("Config load fail", "配置加载失败")); display->display(); }
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
  if (display) {
    display->println(TXT("Device ID:", "设备ID:"));
    display->println(userConfig.device_id);
    display->display();
    delay(1500);
  }

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
    if (display) {
    display->clearDisplay();
#ifdef LANG_CN
    display->println("进入无线OTA模式！");
#else
    display->println(TXT("Entering Wireless OTA Mode", "进入无线OTA模式"));
#endif
    display->display();
#ifdef LANG_CN
    display->println("请连接WiFi：");
    display->println(userConfig.ota_ssid);
    display->display();
    display->println("密码：");
    display->println(userConfig.ota_password);
    display->display();
    display->println("访问IP：192.168.4.1");
    display->display();
#else
    display->println(TXT("Please connect to:", "请连接WiFi:"));
    display->println(userConfig.ota_ssid);
    display->display();
    display->println(TXT("Password:", "密码:"));
    display->println(userConfig.ota_password);
    display->display();
    display->println(TXT("IP: 192.168.4.1", "访问IP：192.168.4.1"));
    display->display();
#endif
    }
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
  delay(200); // Let RF calibration settle before NetWizard AP start

  bool wifiConnected = ConnectWifiWithNetWizard(userConfig, server, 15000, display);

  // ── Network update block ──
  // All remote operations are gated behind a single WiFi guard.
  // When WiFi is unavailable, skip everything and continue with local files.
  bool networkAvailable = wifiConnected && (WiFi.status() == WL_CONNECTED);
  const bool localConfigExists = hadLocalUserConfig;
  const bool localFaceExists = HasLocalFaceAsset(userConfig);
  const bool localAnimExists = HasLocalAnimationAsset(userConfig);

  // ── Probe Gitee → GitHub, cache faster source ──
  // Determine which remote has lower HTTP latency, then use it as the
  // primary source for all subsequent downloads (user config, firmware,
  // face model, animation config).  Result is cached to /source_cache
  // and reused across boots; re-probed if older than 24 h.
  const char *cachedSourcePath = "/source_cache";
  const char *primaryName = "Gitee";   // default for China users
  const char *fallbackName = "GitHub";
  const char *primaryBaseUrl = user_config_gitee_base_url;
  const char *fallbackBaseUrl = user_config_base_url;
  const char *primaryToken = user_config_gitee_token;
  const char *fallbackToken = user_config_github_token;
  const char *primaryAccept = gitee_accept_header;
  const char *fallbackAccept = nullptr;
  const char *primaryAuthScheme = "Bearer ";
  const char *fallbackAuthScheme = "token ";

  if (networkAvailable)
  {
    // Read cache
    String cachedName;
    {
      File f = LittleFS.open(cachedSourcePath, "r");
      if (f)
      {
        cachedName = f.readStringUntil('\n');
        cachedName.trim();
        f.close();
        Serial.printf("[INFO] Source cache: %s\n", cachedName.c_str());
      }
    }

    if (cachedName.isEmpty() || cachedName == "0")
    {
      // Probe Gitee first (for China users), then GitHub
      if (display) { display->println(TXT("Probing Gitee...", "测速 Gitee...")); display->display(); }

      uint32_t giteeLatency = 0;
      uint32_t githubLatency = 0;

      {
        WiFiClient probeClient;
        probeClient.setTimeout(3);
        uint32_t t0 = millis();
        if (probeClient.connect("gitee.com", 443))
        {
          giteeLatency = millis() - t0;
          probeClient.stop();
        }
      }

      if (display) { display->println(TXT("Probing GitHub...", "测速 GitHub...")); display->display(); }

      {
        WiFiClient probeClient;
        probeClient.setTimeout(3);
        uint32_t t0 = millis();
        if (probeClient.connect("api.github.com", 443))
        {
          githubLatency = millis() - t0;
          probeClient.stop();
        }
      }

      // Decide: prefer Gitee unless GitHub is >100ms faster
      bool preferGitee = true;
      if (giteeLatency == 0)
        preferGitee = false; // Gitee unreachable, fall back to GitHub
      else if (githubLatency > 0 && (giteeLatency > githubLatency + 100))
        preferGitee = false; // GitHub significantly faster

      primaryName = preferGitee ? "Gitee" : "GitHub";

      if (!preferGitee)
      {
        // GitHub is primary — swap
        primaryBaseUrl = user_config_base_url;
        fallbackBaseUrl = user_config_gitee_base_url;
        primaryToken = user_config_github_token;
        fallbackToken = user_config_gitee_token;
        primaryAccept = nullptr;
        fallbackAccept = gitee_accept_header;
        primaryAuthScheme = "token ";
        fallbackAuthScheme = "Bearer ";
        fallbackName = "Gitee";
      }

      // Write cache
      File f = LittleFS.open(cachedSourcePath, "w");
      if (f)
      {
        f.print(primaryName);
        f.close();
      }

      Serial.printf("[INFO] Probe: Gitee=%ums GitHub=%ums → primary=%s\n",
                    giteeLatency, githubLatency, primaryName);
    }
    else if (cachedName == "GitHub")
    {
      primaryName = "GitHub";
      fallbackName = "Gitee";
      primaryBaseUrl = user_config_base_url;
      fallbackBaseUrl = user_config_gitee_base_url;
      primaryToken = user_config_github_token;
      fallbackToken = user_config_gitee_token;
      primaryAccept = nullptr;
      fallbackAccept = gitee_accept_header;
      primaryAuthScheme = "token ";
      fallbackAuthScheme = "Bearer ";
    }
  }

  // ── Build reusable source arrays ──
  // [0] = primary (fastest), [1] = fallback
  auto buildSources = [&](RemoteFileSource (&arr)[2])
  {
    arr[0].baseUrl = primaryBaseUrl;
    arr[0].token = primaryToken;
    arr[0].authScheme = primaryAuthScheme;
    arr[0].acceptHeader = primaryAccept;
    arr[0].name = primaryName;
    arr[1].baseUrl = fallbackBaseUrl;
    arr[1].token = fallbackToken;
    arr[1].authScheme = fallbackAuthScheme;
    arr[1].acceptHeader = fallbackAccept;
    arr[1].name = fallbackName;
  };

  if (display)
    display->printf(TXT("WiFi: %s\n", "WiFi: %s\n"),
                   networkAvailable ? TXT("Connected", "已连接") : TXT("Skipped", "已跳过"));

  if (!networkAvailable)
  {
    Serial.println("[WARN] WiFi not connected; skipping all remote downloads");
    if (!(localConfigExists && localFaceExists && localAnimExists))
    {
      HaltStartupOfflineFailure(localConfigExists, localFaceExists, localAnimExists);
    }

    gStartupPhase = StartupPhase::WifiTeardown;
    if (display) {
      display->println(TXT("Network unavailable", "网络不可用"));
      display->println(TXT("Using local files", "使用本地文件"));
      display->display();
      delay(1200);
    }
  }
  else
  {
    // ── Decide whether we can render immediately before any blocking HTTP work ──
    const String deviceFaceFile = "/" + userConfig.device_id + "_face.json";
    const String animFile = "/" + (userConfig.user_animation.length() > 0
                                       ? userConfig.user_animation
                                       : userConfig.device_id + "_animation.json");
    const bool localFaceExists = LittleFS.exists(deviceFaceFile) ||
                                 LittleFS.exists("/universal_face.json");
    const bool localAnimExists = LittleFS.exists(animFile) ||
                                 LittleFS.exists("/example_animation.json");

    if (localFaceExists && localAnimExists)
    {
      // Assets present — start rendering NOW, queue bg download
      Serial.println("[INFO] Local assets present — fast path, background download queued");
      if (display) {
        display->println(TXT("Local assets OK", "本地文件就绪"));
        display->println(TXT("Rendering...", "渲染中..."));
        display->display();
      }

      // Capture source info for background task
      gBgPrimaryBase = primaryBaseUrl;   gBgFallbackBase = fallbackBaseUrl;
      gBgPrimaryToken = primaryToken;    gBgFallbackToken = fallbackToken;
      gBgPrimaryAccept = primaryAccept;  gBgFallbackAccept = fallbackAccept;
      gBgPrimaryAuth = primaryAuthScheme; gBgFallbackAuth = fallbackAuthScheme;
      gBgPrimaryName = primaryName;      gBgFallbackName = fallbackName;
      gBgDeviceId = userConfig.device_id;
      gBgWifiSsid = userConfig.wifi_ssid;
      gBgWifiPass = userConfig.wifi_password;

      // Run firmware update check before launching background downloads.
      // Non-blocking: failure is logged but does not stall startup.
      if (display) {
        display->clearDisplay();
        display->setCursor(0, 0);
        display->println(TXT("Checking firmware...", "检查固件更新..."));
        display->display();
      }
      {
        FirmwareUpdateConfig fwCfg;
        fwCfg.primarySource.baseUrl = primaryBaseUrl;
        fwCfg.primarySource.token = primaryToken;
        fwCfg.primarySource.authScheme = primaryAuthScheme;
        fwCfg.primarySource.acceptHeader = primaryAccept;
        fwCfg.primarySource.name = primaryName;
        fwCfg.primaryName = primaryName;
        fwCfg.fallbackSource.baseUrl = fallbackBaseUrl;
        fwCfg.fallbackSource.token = fallbackToken;
        fwCfg.fallbackSource.authScheme = fallbackAuthScheme;
        fwCfg.fallbackSource.acceptHeader = fallbackAccept;
        fwCfg.fallbackSource.name = fallbackName;
        fwCfg.fallbackName = fallbackName;
        fwCfg.manifestFilename = kFirmwareManifest;
        fwCfg.currentVersion = kFirmwareVersion;
        fwCfg.display = display;
        fwCfg.verbose = kVerboseStartup;
        Serial.printf("[INFO] Checking firmware update via %s before background downloads...\n", primaryName);
        const FirmwareUpdateResult fwResult = FirmwareUpdater::CheckAndUpdate(fwCfg);
        if (fwResult == FirmwareUpdateResult::Failed)
        {
          Serial.println("[WARN] Firmware update check failed; continuing with background downloads");
          if (display && kVerboseStartup) {
            display->clearDisplay();
            display->setCursor(0, 0);
            display->println(TXT("FW check failed", "固件检查失败"));
            display->println(TXT("Continuing...", "继续启动..."));
            display->display();
            delay(800);
          }
        }
        else if (fwResult == FirmwareUpdateResult::UpToDate)
        {
          Serial.println("[INFO] Firmware is up to date");
        }
        // Updated result reboots internally, never reaches here
      }
      protogc::ProtoGC::collectFull("post-firmware-check");

      // Don't tear down WiFi here — background task needs it
      gStartupPhase = StartupPhase::Downloading;
      gBgDownloadStartMs = millis();
    }
    else
    {
      // Missing assets — download synchronously before rendering
      if (display) { display->println(TXT("Downloading assets...", "下载资源中...")); display->display(); }

      RemoteFileSource userConfigSources[2];
      buildSources(userConfigSources);
      DownloadUserConfigFromSources(userConfigSources, 2, userConfig,
                                    kVerboseStartup, display);
      protogc::ProtoGC::collectFull("post-user-config");

      FirmwareUpdateConfig firmwareUpdateConfig;
      firmwareUpdateConfig.primarySource.baseUrl = primaryBaseUrl;
      firmwareUpdateConfig.primarySource.token = primaryToken;
      firmwareUpdateConfig.primarySource.authScheme = primaryAuthScheme;
      firmwareUpdateConfig.primarySource.acceptHeader = primaryAccept;
      firmwareUpdateConfig.primarySource.name = primaryName;
      firmwareUpdateConfig.primaryName = primaryName;
      firmwareUpdateConfig.fallbackSource.baseUrl = fallbackBaseUrl;
      firmwareUpdateConfig.fallbackSource.token = fallbackToken;
      firmwareUpdateConfig.fallbackSource.authScheme = fallbackAuthScheme;
      firmwareUpdateConfig.fallbackSource.acceptHeader = fallbackAccept;
      firmwareUpdateConfig.fallbackSource.name = fallbackName;
      firmwareUpdateConfig.fallbackName = fallbackName;
      firmwareUpdateConfig.manifestFilename = kFirmwareManifest;
      firmwareUpdateConfig.currentVersion = kFirmwareVersion;
      firmwareUpdateConfig.display = display;
      firmwareUpdateConfig.verbose = kVerboseStartup;

      Serial.printf("[INFO] Checking firmware update via %s after config sync...\n", primaryName);
      const FirmwareUpdateResult firmwareUpdateResult = FirmwareUpdater::CheckAndUpdate(firmwareUpdateConfig);
      if (firmwareUpdateResult == FirmwareUpdateResult::Failed)
      {
        Serial.println("[WARN] Auto firmware update check failed; continuing startup");
      }
      protogc::ProtoGC::collectFull("post-firmware-check");

      FaceUpdateConfig faceConfigs[2] = {
        {userConfig.wifi_ssid.c_str(), userConfig.wifi_password.c_str(),
         primaryBaseUrl, primaryToken, primaryAccept, primaryAuthScheme, primaryName},
        {userConfig.wifi_ssid.c_str(), userConfig.wifi_password.c_str(),
         fallbackBaseUrl, fallbackToken, fallbackAccept, fallbackAuthScheme, fallbackName}
      };
      bool faceReady = EnsureFaceModelJson(faceConfigs, 2, userConfig.device_id,
                                           display, kVerboseStartup);
      if (!faceReady)
      {
        if (!LittleFS.exists("/universal_face.json") &&
            !LittleFS.exists(deviceFaceFile))
        {
          if (display) {
            display->println(TXT("Face model missing", "面部模型缺失"));
            display->println(TXT("Restarting...", "重启中..."));
            display->display();
            delay(2000);
          }
          ESP.restart();
        }
      }
      protogc::ProtoGC::collectFull("post-face-sync");

      // Skip background phase — go straight to WiFi teardown below
      gStartupPhase = StartupPhase::WifiTeardown;
    }
  }

  // Always update these from config regardless of network availability
  user_name = userConfig.username.c_str();
  BLE_TX2_UUID = userConfig.ble_tx_uuid.c_str();
  BLE_RX2_UUID = userConfig.ble_rx_uuid.c_str();
  User_R = userConfig.user_r;
  User_G = userConfig.user_g;
  User_B = userConfig.user_b;

#ifndef VERBOSE_STARTUP
  if (display) {
    display->clearDisplay();
    display->pushImage(22, 4, 84, 40, FusionOpen_startup_logo);
    display->progressBar(14, 50, 100, 8, 0);
    display->display();
  }
#endif

  animation.Initialize(userConfig,
                       networkAvailable ? user_config_base_url : nullptr,
                       networkAvailable ? user_config_gitee_base_url : nullptr,
                       networkAvailable ? user_config_github_token : nullptr,
                       networkAvailable ? user_config_gitee_token : nullptr,
                       display,
                       kVerboseStartup);
  protogc::ProtoGC::collectFull("post-animation-init");

  // ── Launch background download task if on fast path ──
  if (gStartupPhase == StartupPhase::Downloading)
  {
    // Flash I/O (LittleFS) requires the task stack in internal DRAM.
    // PSRAM stacks trigger cache-disable assertions during SPI flash access.
    xTaskCreatePinnedToCore(BackgroundDownloadTask, "BgDL", 8192,
                            nullptr, kBgTaskPriority, &gBgDownloadTask, 0);
    if (gBgDownloadTask)
      Serial.println("[INFO] Background download task launched");
    else
      Serial.println("[WARN] Failed to create background download task");
  }

  // ── Deferred WiFi teardown + BLE init ──
  // On the fast path, WiFi stays up during early rendering frames while
  // the background task syncs assets.  The main loop tears down WiFi and
  // initialises BLE once the task completes (or times out).
  //
  // On the non-fast path (offline / sync-download), we still tear down
  // WiFi and init BLE here, but the animation pipeline (semaphores +
  // AnimationTask) is deferred to the loop() phase machine so it runs
  // exactly once per boot regardless of startup path.
  if (gStartupPhase != StartupPhase::Downloading)
  {
    DoWifiTeardownAndBleInit();
    EnsureControllerInitialized();
    // Pipeline (semaphores + AnimationTask) is created by loop() BleInit phase.
  }
  else
  {
    // Fast path: controller already initialized, pipeline deferred to loop().
    // BLE init + final pipeline wiring deferred to loop() phase machine.
  }

#ifndef VERBOSE_STARTUP
  if (display) display->progressBar(14, 50, 100, 8, 100);
#endif
  delay(1000);
  if (display) display->clear();
}

void loop()
{
  const uint32_t now = millis();

  // ── Startup phase machine: wait for background downloads to fully
  //     complete before tearing down WiFi.  Once BLE is initialised we
  //     cannot fetch any more remote assets, so every download must
  //     finish first.
  if (gStartupPhase == StartupPhase::Downloading)
  {
    const bool timedOut = (now - gBgDownloadStartMs) >= kBgDownloadTimeoutMs;
    if (gBgDownloadsDone || timedOut)
    {
      if (timedOut && !gBgDownloadsDone)
      {
        Serial.println("[WARN] Background download timeout — forcing WiFi teardown");
        gBgDownloadsOk = false;
      }
      if (!gBgDisplayResetDone)
      {
        controller.ResetDisplayDriver();
        gBgDisplayResetDone = true;
      }
      Serial.println("[INFO] Background downloads complete — proceeding to WiFi teardown");
      gStartupPhase = StartupPhase::WifiTeardown;
    }
  }
  if (gStartupPhase == StartupPhase::WifiTeardown)
  {
    DoWifiTeardownAndBleInit();
    gStartupPhase = StartupPhase::BleInit;
  }
  if (gStartupPhase == StartupPhase::BleInit)
  {
    // BLE already initialised by DoWifiTeardownAndBleInit above;
    // now complete the remaining post-BLE setup.
    EnsureControllerInitialized();
#if ANIM_RENDER_PIPELINE
    {
      Scene *scene = animation.GetScene();
      if (scene)
      {
        Object3D **objs = scene->GetObjects();
        const unsigned int count = scene->GetObjectCount();
        for (unsigned int i = 0; i < count; i++)
        {
          if (objs[i]) objs[i]->EnableDoubleBuffer();
        }
      }
    }
    gAnimDoneSemaphore = xSemaphoreCreateBinary();
    gRenderDoneSemaphore = xSemaphoreCreateBinary();
    if (gAnimDoneSemaphore && gRenderDoneSemaphore)
    {
      gAnimTaskStop = false;
      if (xTaskCreatePinnedToCore(AnimationTask, "ProtoAnim", ANIM_TASK_STACK_BYTES,
                                  nullptr, ANIM_TASK_PRIORITY, &gAnimTaskHandle,
                                  ANIM_TASK_CORE) == pdPASS)
      {
        gPipelineActive = true;
        Serial.printf("[PIPELINE] Animation task started on core %d\n", ANIM_TASK_CORE);
        vTaskDelay(1); // let the task reach its first semaphore block before we signal it
      }
      else
      {
        Serial.printf("[PIPELINE] Failed to create animation task; using single-core\n");
        gPipelineActive = false;
      }
    }
    else
    {
      gPipelineActive = false;
    }
#endif
    // Free the bg task stack allocation (no longer needed)
    if (gBgDownloadTask)
    {
      gBgDownloadTask = nullptr;
    }
    gStartupPhase = StartupPhase::Running;
    // ── One-time pipeline health diagnostic ──
    {
      const char* mode = gPipelineActive ? "DUAL-CORE" : "SINGLE-CORE";
      Serial.printf("[PIPELINE] mode=%s core=%d taskStack=%u animTask=%p semA=%p semR=%p\n",
                    mode,
                    ANIM_TASK_CORE,
                    ANIM_TASK_STACK_BYTES,
                    static_cast<void*>(gAnimTaskHandle),
                    static_cast<void*>(gAnimDoneSemaphore),
                    static_cast<void*>(gRenderDoneSemaphore));
      if (!gPipelineActive) {
        Serial.println("[PIPELINE] WARNING: Running single-core! Check internal DRAM headroom.");
      }
    }
    // Quick HUD refresh after updates complete
    if (display) {
      display->clear();
      display->setCursor(0, 0);
    }
    Serial.println("[INFO] Startup complete — running");
  }

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

  // ── Dual-core pipeline: overlap I2C/display with animation ──
  // Core 1 kicks off the animation task on core 0, then does
  // MenuUpdate (I2C display, gesture, BLE, NeoPixel) while core 0
  // computes UpdateTime + PublishSceneVertices.  When both finish,
  // core 1 proceeds to rasterization and HUB75 display.
  //
  // MenuUpdate touches only core 1 peripherals (I2C, BLE, NeoPixel);
  // the animation task on core 0 does CPU-only work (morphs, materials,
  // FFT, vertex transforms) — no peripheral contention.
  // Shared scalar state (face expression, boop flags, hue) is read by
  // the animation task and written by MenuUpdate; on ESP32-S3 aligned
  // word accesses are hardware-atomic, so stale-read is the worst case.

#if ANIM_RENDER_PIPELINE
  if (gPipelineActive) {
    // Signal core 0 to begin animation computation
    gAnimRatio = ratio;
    xSemaphoreGive(gRenderDoneSemaphore);

    // Core 1: do I2C/display/gesture work while core 0 runs animation
    animation.MenuUpdate();

    // Wait for the animation task to finish this frame before rasterization.
    xSemaphoreTake(gAnimDoneSemaphore, portMAX_DELAY);
  } else
#endif
  {
    yield(); // Feed watchdog before animation update
    animation.MenuUpdate();
    yield();
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

#ifdef PRINTINFO
  static uint32_t lastPrintMs = 0;
  uint32_t nowPrint = millis();
  // Sample every ~2 seconds to avoid serial bottleneck
  if (nowPrint - lastPrintMs >= 2000) {
    lastPrintMs = nowPrint;
    Serial.printf("anim=%.2fms render=%.2fms pipe=%d intFree=%u largestBlk=%u psramFree=%u micFrames=%lu micDrops=%lu micStack=%lu\n",
        animation.GetAnimationTime() * 1000.0f,
        controller.GetRenderTime() * 1000.0f,
        gPipelineActive ? 1 : 0,
        GetFreeInternalDRAM(),
        GetLargestFreeInternalBlock(),
        GetFreePSRAM(),
        static_cast<unsigned long>(MicrophoneFourierIT::GetProcessedFrameCount()),
        static_cast<unsigned long>(MicrophoneFourierIT::GetSamplerDropCount()),
        static_cast<unsigned long>(MicrophoneFourierIT::GetTaskStackHighWater()));
  }
#endif
}
