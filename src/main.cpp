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
#include "qrcode.h"
#include <M5Unified.h>
#include <M5UnitGLASS2.h>
#include <Wire.h>
#include <LittleFS.h>
#include "Network/FaceModelUpdater.h"
#include "Network/UserConfigManager.h"

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


QRCode qrcode;
uint8_t qrcodeData[((29 * 29) + 7) / 8];

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

void setup()
{
  heap_caps_malloc_extmem_enable(0);
  pinMode(OTA_BTN, INPUT_PULLUP);
  Serial.begin(115200);
  Serial.println("/nStarting...");
  WiFi.mode(WIFI_AP_STA);
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

  if (!EnsureUserConfig(userConfig))
  {
    display.println(TXT("Config load fail", "配置加载失败"));
    display.display();
  }

  // Factory flash indicator: blink internal WS2812 red/blue if face model or animation config is missing.
  if (LittleFS.begin(false) || LittleFS.begin(true))
  {
    const char *markerPath = "/factory_blink_done";

    bool faceExists = LittleFS.exists("/universal_face.json");

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

  qrcode_initText(&qrcode, qrcodeData, 3, 0, userConfig.ble_rx_uuid.c_str());

  if (digitalRead(OTA_BTN) == LOW)
  {
    controller.Initialize();
#ifdef TASESP32S3
    virtualDisp->clearScreen();
    virtualDisp->fillScreenRGB888(255, 255, 255);
    qrcode_initText(&qrcode, qrcodeData, 3, 0, userConfig.ble_rx_uuid.c_str());
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
    #ifdef TASESP32S3
    for (uint8_t y = 0; y < qrcode.size; y++)
    {
      for (uint8_t x = 0; x < qrcode.size; x++)
      {
        virtualDisp->drawPixelRGB888(60 - x, (y) + 34,
                                     qrcode_getModule(&qrcode, x, (28 - y)) ? 0 : userConfig.user_r,
                                     qrcode_getModule(&qrcode, x, (28 - y)) ? 0 : userConfig.user_g,
                                     qrcode_getModule(&qrcode, x, (28 - y)) ? 0 : userConfig.user_b);
      }
    }
    #endif

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
              { request->send(200, "text/html", index_html); });

#if defined(USING_ELEGANTOTA_PRO) || defined(USING_ELEGANTOTA)
    // You can also enable authentication by uncommenting the below line.
    ElegantOTA.setAuth(userConfig.username.c_str(), userConfig.ota_password.c_str());

    ElegantOTA.setTitle("Ruby Protogen Studio OTA Portal"); // Set OTA Webpage Title

    ElegantOTA.setID(userConfig.device_id.c_str()); // Set Hardware ID
    ElegantOTA.setFWVersion("1.0.0");               // Set Firmware Version

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

  bool wifiConnected = ConnectWifiWithNetWizard(userConfig, server, 3000, &display);
  if (!wifiConnected)
  {
    Serial.println("[WARN] WiFi not connected via NetWizard; downloads may fail");
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println(TXT("WiFi not connected", "WiFi未连接"));
    display.println(TXT("Continuing startup", "继续启动"));
    display.display();
  }

  // Attempt to pull remote user_config.json named by device_id (GitHub first, Gitee fallback).
  bool userConfigFetched = DownloadUserConfigFromGithub(user_config_base_url, userConfig, kVerboseStartup ? true : false, &display, user_config_github_token);
  if (!userConfigFetched && user_config_gitee_token != nullptr && user_config_gitee_token[0] != '\0')
  {
    Serial.println("[WARN] GitHub user_config.json download failed; trying Gitee fallback");
    userConfigFetched = DownloadUserConfigFromGitee(user_config_gitee_base_url, userConfig, kVerboseStartup ? true : false, &display, user_config_gitee_token);
  }

  user_name = userConfig.username.c_str();
  BLE_TX2_UUID = userConfig.ble_tx_uuid.c_str();
  BLE_RX2_UUID = userConfig.ble_rx_uuid.c_str();

  User_R = userConfig.user_r;
  User_G = userConfig.user_g;
  User_B = userConfig.user_b;

  FaceUpdateConfig githubFaceConfig = {userConfig.wifi_ssid.c_str(), userConfig.wifi_password.c_str(), face_json_url, face_checksum_url, "/universal_face.json", "", ""};
  FaceUpdateConfig giteeFaceConfig = {userConfig.wifi_ssid.c_str(), userConfig.wifi_password.c_str(), face_gitee_json_url, face_gitee_checksum_url, "/universal_face.json", face_repo_token, gitee_accept_header};

  // Ensure face model is present before animation startup (GitHub first, Gitee fallback)
  bool faceReady = EnsureUniversalFaceJson(githubFaceConfig, display, kVerboseStartup);
  if (!faceReady && face_gitee_json_url != nullptr && face_gitee_json_url[0] != '\0')
  {
    Serial.println("[WARN] GitHub face download failed; trying Gitee fallback");
    faceReady = EnsureUniversalFaceJson(giteeFaceConfig, display, kVerboseStartup);
  }

  if (faceReady)
  {
    Serial.println("[INFO] universal_face.json is ready");
  }
  else
  {
    Serial.println("[WARN] universal_face.json is not available; animation may fail");
    delay(10);
    ESP.restart();
  }

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
  controller.Initialize();
#ifndef VERBOSE_STARTUP
  display.progressBar(14, 50, 100, 8, 100);
#endif
  delay(1000);
  display.clear();
}

void loop()
{

#ifdef TASESP32S3
  if (digitalRead(OTA_BTN) == LOW)
  {
    virtualDisp->clearScreen();
    virtualDisp->fillScreenRGB888(255, 255, 255);
    qrcode_initText(&qrcode, qrcodeData, 3, 0, userConfig.ble_rx_uuid.c_str());
    WiFi.mode(WIFI_AP);
    WiFi.softAP(userConfig.ota_ssid.c_str(), userConfig.ota_password.c_str());
    Serial.println("");
    for (uint8_t y = 0; y < qrcode.size; y++)
    {
      for (uint8_t x = 0; x < qrcode.size; x++)
      {
        virtualDisp->drawPixelRGB888(60 - x, (y) + 34,
                                     qrcode_getModule(&qrcode, x, (28 - y)) ? 0 : userConfig.user_r,
                                     qrcode_getModule(&qrcode, x, (28 - y)) ? 0 : userConfig.user_g,
                                     qrcode_getModule(&qrcode, x, (28 - y)) ? 0 : userConfig.user_b);
      }
    }
    delay(15000);
  }
  // controller.SetAccentBrightness(animation.GetAccentBrightness() * 25 + 5);
  // controller.SetBrightness(powf(animation.GetBrightness() + 3, 2) / 3);
  float ratio = (float)(millis() % 5000) / 5000.0f;
  controller.SetBrightness(animation.GetBrightness());
  animation.UpdateTime(ratio);
  controller.Render(animation.GetScene());
#elif defined(TASESP32P4)
  // TODO: add panel-clearing logic for P4 HUB75 if needed
  if (digitalRead(OTA_BTN) == LOW)
  {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(userConfig.ota_ssid.c_str(), userConfig.ota_password.c_str());
    delay(15000);
  }
  float ratio = (float)(millis() % 5000) / 5000.0f;
  controller.SetBrightness(animation.GetBrightness());
  animation.UpdateTime(ratio);
  controller.Render(animation.GetScene());
#else
  Serial.print("not defined");
#endif

  // controller.

  controller.Display();

#ifdef PRINTINFO
  Serial.print("Animated in ");
  Serial.print(animation.GetAnimationTime(), 4);

  Serial.print("s, Rendered in ");
  Serial.print(controller.GetRenderTime(), 4);

  Serial.print("s, Free memory ");
  Serial.print(FreeMem(), 3);

  Serial.println("Kb");

  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
#endif
}
