#include "UserConfigManager.h"

#include "RemoteFileSync.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <NetWizard.h>
#include <ProtoGC.h>
#include <WiFi.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>

// Optional display feedback is guarded at the call site.

#ifdef LANG_CN
#define TXT(en, cn) cn
#else
#define TXT(en, cn) en
#endif

String ReadUniqueDeviceId();

namespace
{
    constexpr const char *kUserConfigPath = "/user_config.json";
    constexpr const char *kDefaultPortalSsid = "General ProtogenOTAWiFi";
    constexpr const char *kDefaultPortalPassword = "Protogen#1229#25";
    constexpr size_t kMaxPortalSsidLength = 32;
    constexpr uint32_t kUserConfigConnectTimeoutMs = 2500;
    constexpr uint32_t kUserConfigRequestTimeoutMs = 4000;
    constexpr uint32_t kUserConfigIdleTimeoutMs = 1500;

    M5GFX *g_netWizardDisplay = nullptr;
    bool g_netWizardDisplayActive = false;
    String g_netWizardPortalSsid;
    String g_netWizardPortalPassword;

    bool EnsureFsMounted()
    {
        return RemoteFileSync::EnsureFsMounted();
    }

    uint8_t ClampByte(long v)
    {
        if (v < 0)
            return 0;
        if (v > 255)
            return 255;
        return static_cast<uint8_t>(v);
    }

    String CurrentPortalIpString()
    {
        IPAddress portalIp((uint32_t)0);
#if defined(ESP32)
        if (WiFi.AP.hasIP())
        {
            portalIp = WiFi.AP.localIP();
        }
#endif
        if (portalIp == IPAddress((uint32_t)0))
        {
            portalIp = WiFi.softAPIP();
        }
        if (portalIp == IPAddress((uint32_t)0))
        {
            return String("192.168.4.1");
        }
        return portalIp.toString();
    }

    void ShowNetWizardDisplay(const String &statusLine, const String &hintLine = String())
    {
        if (!g_netWizardDisplay || !g_netWizardDisplayActive)
        {
            return;
        }

        g_netWizardDisplay->clearDisplay();
        g_netWizardDisplay->setCursor(0, 0);
        g_netWizardDisplay->println(statusLine);
        g_netWizardDisplay->println(String(TXT("SSID: ", "热点: ")) + g_netWizardPortalSsid);
        if (!g_netWizardPortalPassword.isEmpty())
        {
            g_netWizardDisplay->println(String(TXT("PW: ", "密码: ")) + g_netWizardPortalPassword);
        }
        g_netWizardDisplay->println(String(TXT("IP: ", "地址: ")) + CurrentPortalIpString());
        g_netWizardDisplay->println(hintLine.isEmpty() ? TXT("Open browser if no popup", "未弹窗请手动打开浏览器") : hintLine);
        g_netWizardDisplay->display();
    }

    const char *ConnectionStatusText(NetWizardConnectionStatus status)
    {
        switch (status)
        {
        case NetWizardConnectionStatus::CONNECTING:
            return TXT("Connecting WiFi...", "正在连接WiFi...");
        case NetWizardConnectionStatus::CONNECTED:
            return TXT("WiFi connected", "WiFi已连接");
        case NetWizardConnectionStatus::CONNECTION_FAILED:
            return TXT("WiFi connect failed", "WiFi连接失败");
        case NetWizardConnectionStatus::CONNECTION_LOST:
            return TXT("WiFi lost", "WiFi连接中断");
        case NetWizardConnectionStatus::NOT_FOUND:
            return TXT("WiFi not found", "未找到WiFi");
        case NetWizardConnectionStatus::DISCONNECTED:
        default:
            return TXT("Waiting for WiFi", "等待WiFi连接");
        }
    }

    const char *PortalStateText(NetWizardPortalState state)
    {
        switch (state)
        {
        case NetWizardPortalState::CONNECTING_WIFI:
            return TXT("Saving WiFi config", "正在保存WiFi配置");
        case NetWizardPortalState::WAITING_FOR_CONNECTION:
            return TXT("Applying WiFi config", "正在应用WiFi配置");
        case NetWizardPortalState::SUCCESS:
            return TXT("WiFi saved", "WiFi已保存");
        case NetWizardPortalState::FAILED:
            return TXT("WiFi setup failed", "WiFi设置失败");
        case NetWizardPortalState::TIMEOUT:
            return TXT("WiFi setup timeout", "WiFi设置超时");
        case NetWizardPortalState::IDLE:
        default:
            return TXT("Join setup WiFi", "连接配置热点");
        }
    }

    bool ConnectAndStabilizeWifi(const String &ssid,
                                 const String &password,
                                 unsigned long connectTimeoutMs,
                                 IPAddress *resolvedIp = nullptr)
    {
        if (ssid.isEmpty())
        {
            return false;
        }

        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid.c_str(), password.c_str());

        const uint32_t startMs = millis();
        while ((millis() - startMs) < connectTimeoutMs)
        {
            if (WiFi.status() == WL_CONNECTED)
            {
                IPAddress ip = WiFi.localIP();
                if (ip != IPAddress((uint32_t)0))
                {
                    if (resolvedIp)
                    {
                        *resolvedIp = ip;
                    }

                    // Give the STA interface a short settling window before immediate HTTP operations.
                    delay(250);
                    return true;
                }
            }

            delay(100);
        }

        return false;
    }

    String BuildSetupPortalSsid(const UserConfig &config)
    {
        String portalSsid = config.ota_ssid;
        portalSsid.trim();
        if (portalSsid.isEmpty())
        {
            portalSsid = config.username;
            portalSsid.trim();
        }
        if (portalSsid.isEmpty())
        {
            portalSsid = config.device_id;
        }
        if (portalSsid.isEmpty())
        {
            portalSsid = kDefaultPortalSsid;
        }
        if (portalSsid.length() > kMaxPortalSsidLength)
        {
            Serial.printf("[WARN] Setup portal SSID is too long (%u); truncating to %u bytes\n",
                          static_cast<unsigned>(portalSsid.length()),
                          static_cast<unsigned>(kMaxPortalSsidLength));
            portalSsid = portalSsid.substring(0, kMaxPortalSsidLength);
        }
        return portalSsid;
    }

    String BuildSetupPortalPassword(const UserConfig &config)
    {
        String portalPassword = config.ota_password;
        portalPassword.trim();
        if (portalPassword.isEmpty())
        {
            portalPassword = kDefaultPortalPassword;
        }
        if (portalPassword.length() < 8 || portalPassword.length() > 63)
        {
            Serial.printf("[WARN] Setup portal password length %u is invalid; using default password\n",
                          static_cast<unsigned>(portalPassword.length()));
            portalPassword = kDefaultPortalPassword;
        }
        return portalPassword;
    }

    void ConfigureNetWizardDisplayCallbacks(NetWizard &wizard)
    {
        wizard.onConnectionStatus([](NetWizardConnectionStatus status) {
            if (!g_netWizardDisplayActive)
            {
                return;
            }

            const String hint = status == NetWizardConnectionStatus::CONNECTED ? WiFi.localIP().toString() : String();
            ShowNetWizardDisplay(ConnectionStatusText(status), hint);
        });

        wizard.onPortalState([](NetWizardPortalState state) {
            if (!g_netWizardDisplayActive)
            {
                return;
            }

            String hint;
            if (state == NetWizardPortalState::IDLE)
            {
                hint = TXT("Open browser if no popup", "未弹窗请手动打开浏览器");
            }
            else if (state == NetWizardPortalState::SUCCESS)
            {
                hint = TXT("Reconnecting device", "设备正在重新连接");
            }
            ShowNetWizardDisplay(PortalStateText(state), hint);
        });
    }

    bool RunNetWizardSetupPortal(UserConfig &config,
                                 NetWizard &wizard,
                                 unsigned long reconnectTimeoutMs,
                                 M5GFX *display,
                                 IPAddress *resolvedIp)
    {
        const String portalSsid = BuildSetupPortalSsid(config);
        const String portalPassword = BuildSetupPortalPassword(config);

        config.ota_ssid = portalSsid;
        config.ota_password = portalPassword;

        g_netWizardDisplay = display;
        g_netWizardDisplayActive = display != nullptr;
        g_netWizardPortalSsid = portalSsid;
        g_netWizardPortalPassword = portalPassword;

        ConfigureNetWizardDisplayCallbacks(wizard);
        ShowNetWizardDisplay(TXT("Starting WiFi setup", "正在启动WiFi设置"));

        Serial.printf("[INFO] Starting NetWizard setup portal SSID=%s IP=%s\n",
                      portalSsid.c_str(),
                      CurrentPortalIpString().c_str());

        wizard.autoConnect(portalSsid.c_str(), portalPassword.c_str());

        config.wifi_ssid = wizard.getSSID();
        config.wifi_password = wizard.getPassword();

        // autoConnect in BLOCKING mode should already have WiFi connected.
        // Calling WiFi.begin() again would tear down the existing connection and
        // restart DHCP, which can fail to re-assign an IP. Only fall back to a
        // manual connect if the wizard's internal connection didn't stabilize.
        bool connected = (WiFi.status() == WL_CONNECTED);
        if (connected)
        {
            IPAddress ip = WiFi.localIP();
            if (ip != IPAddress((uint32_t)0))
            {
                if (resolvedIp)
                {
                    *resolvedIp = ip;
                }
                Serial.printf("[INFO] NetWizard portal handoff OK, IP=%s\n", ip.toString().c_str());
            }
            else
            {
                // DHCP may be pending; give it a short settle window.
                delay(500);
                ip = WiFi.localIP();
                if (ip != IPAddress((uint32_t)0))
                {
                    if (resolvedIp)
                    {
                        *resolvedIp = ip;
                    }
                    Serial.printf("[INFO] NetWizard portal handoff OK (delayed DHCP), IP=%s\n", ip.toString().c_str());
                }
                else
                {
                    connected = false;
                    Serial.println("[WARN] NetWizard portal handoff: no IP after settle; falling back to manual connect");
                }
            }
        }
        else
        {
            Serial.println("[INFO] NetWizard portal handoff: STA not connected; attempting manual connect");
        }

        if (!connected)
        {
            connected = ConnectAndStabilizeWifi(config.wifi_ssid, config.wifi_password, reconnectTimeoutMs, resolvedIp);
        }
        SaveUserConfig(config);

        if (display)
        {
            display->clearDisplay();
            display->setCursor(0, 0);
            display->println(connected ? TXT("WiFi connected", "WiFi已连接") : TXT("WiFi setup done", "WiFi设置完成"));
            if (connected && resolvedIp)
            {
                display->println(resolvedIp->toString());
            }
            else if (!config.wifi_ssid.isEmpty())
            {
                display->println(config.wifi_ssid);
            }
            if (!connected)
            {
                display->println(TXT("Retry from setup AP", "请重新连接配置热点"));
            }
            display->display();
        }

        if (!connected)
        {
            Serial.printf("[WARN] Portal WiFi handoff did not stabilize for SSID %s\n", config.wifi_ssid.c_str());
        }

        g_netWizardDisplayActive = false;
        return connected;
    }

    RemoteFileSource BuildRemoteSource(const char *baseUrl,
                                       const char *token,
                                       const char *authScheme,
                                       const char *acceptHeader)
    {
        RemoteFileSource source;
        source.baseUrl = baseUrl;
        source.token = token;
        source.authScheme = authScheme;
        source.acceptHeader = acceptHeader;
        return source;
    }

    RemoteFileSyncOptions BuildUserConfigSyncOptions(M5GFX *display, bool verbose)
    {
        RemoteFileSyncOptions options;
        options.display = display;
        options.verbose = verbose;
        options.keepExistingWhenRemoteMd5Unavailable = false;
        options.enableLatencyProbe = false;
        options.skipRemoteMd5 = true;
        options.connectTimeoutMs = kUserConfigConnectTimeoutMs;
        options.requestTimeoutMs = kUserConfigRequestTimeoutMs;
        options.idleTimeoutMs = kUserConfigIdleTimeoutMs;
        options.ui.checkingMd5 = TXT("Checking config...", "检查配置中...");
        options.ui.upToDate = TXT("Config up-to-date", "配置已最新");
        options.ui.md5Mismatch = TXT("Config changed, redownloading...", "配置已变更，重新下载");
        options.ui.downloading = TXT("Downloading config...", "正在下载配置...");
        options.ui.success = TXT("Config updated", "配置已更新");
        options.ui.httpBeginFail = TXT("Config connect fail", "配置连接失败");
        options.ui.httpGetFail = TXT("Config fetch failed", "配置获取失败");
        options.ui.md5VerifyFail = TXT("Config verify failed", "配置校验失败");
        return options;
    }

    bool ReloadDownloadedConfigPreservingWifi(UserConfig &config,
                                              const String &prevWifiSsid,
                                              const String &prevWifiPassword)
    {
        if (!EnsureUserConfig(config))
        {
            return false;
        }

        config.wifi_ssid = prevWifiSsid;
        config.wifi_password = prevWifiPassword;
        return SaveUserConfig(config);
    }

    bool DownloadUserConfigFromSource(const RemoteFileSource &source,
                                      UserConfig &config,
                                      bool verbose,
                                      M5GFX *display)
    {
        if (!EnsureFsMounted())
        {
            return false;
        }

        const String prevWifiSsid = config.wifi_ssid;
        const String prevWifiPassword = config.wifi_password;

        if (display)
        {
            display->clearDisplay();
            display->setCursor(0, 0);
            display->println(TXT("Checking config...", "检查配置中..."));
            display->display();
        }

        const String remoteFilename = config.device_id + String(".json");
        if (!RemoteFileSync::Sync(source, remoteFilename, kUserConfigPath, BuildUserConfigSyncOptions(display, verbose)))
        {
            return false;
        }

        return ReloadDownloadedConfigPreservingWifi(config, prevWifiSsid, prevWifiPassword);
    }

    UserConfig DefaultUserConfig()
    {
        UserConfig cfg;
        cfg.device_id = ReadUniqueDeviceId();
        cfg.username = "Uninit Protogen Firmware";
        cfg.user_r = 25;
        cfg.user_g = 125;
        cfg.user_b = 235;
        cfg.ble_rx_uuid = "8fc048fe-591c-4fb8-a99c-e9f507940964";
        cfg.ble_tx_uuid = "42036097-7c76-4129-a80d-ed4e0d12359c";
        cfg.ota_ssid = "General ProtogenOTAWiFi";
        cfg.ota_password = "Protogen#1229#25";
        cfg.wifi_ssid = "";
        cfg.wifi_password = "";
        cfg.user_animation = "";
        return cfg;
    }
} // namespace

String ReadUniqueDeviceId()
{
    uint8_t raw[16] = {0};
    esp_err_t err = esp_efuse_read_field_blob(ESP_EFUSE_OPTIONAL_UNIQUE_ID, raw, sizeof(raw) * 8);
    if (err != ESP_OK)
    {
        // Fallback to MAC-based ID if unique ID is unavailable.
        uint64_t mac = ESP.getEfuseMac();
        for (size_t i = 0; i < 6; ++i)
        {
            raw[i] = static_cast<uint8_t>((mac >> ((5 - i) * 8)) & 0xFF);
        }
    }

    String hex;
    hex.reserve(sizeof(raw) * 2);
    for (size_t i = 0; i < sizeof(raw); ++i)
    {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", raw[i]);
        hex += buf;
    }
    return hex;
}

bool SaveUserConfig(const UserConfig &config)
{
    if (!EnsureFsMounted())
    {
        return false;
    }

    BasicJsonDocument<protogc::ProtoJsonPsramAllocator> doc(640);
    doc["device_id"] = config.device_id;
    doc["username"] = config.username;
    doc["user_r"] = config.user_r;
    doc["user_g"] = config.user_g;
    doc["user_b"] = config.user_b;
    doc["ble_rx_uuid"] = config.ble_rx_uuid;
    doc["ble_tx_uuid"] = config.ble_tx_uuid;
    doc["ota_ssid"] = config.ota_ssid;
    doc["ota_password"] = config.ota_password;
    doc["wifi_ssid"] = config.wifi_ssid;
    doc["wifi_password"] = config.wifi_password;
    doc["user_animation"] = config.user_animation;

    File f = LittleFS.open(kUserConfigPath, "w");
    if (!f)
    {
        return false;
    }
    bool ok = (serializeJson(doc, f) > 0);
    f.close();
    return ok;
}

bool EnsureUserConfig(UserConfig &config)
{
    if (!EnsureFsMounted())
    {
        return false;
    }

    if (!LittleFS.exists(kUserConfigPath))
    {
        config = DefaultUserConfig();
        return SaveUserConfig(config);
    }

    File f = LittleFS.open(kUserConfigPath, "r");
    if (!f)
    {
        config = DefaultUserConfig();
        return SaveUserConfig(config);
    }

    BasicJsonDocument<protogc::ProtoJsonPsramAllocator> doc(640);
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err)
    {
        config = DefaultUserConfig();
        return SaveUserConfig(config);
    }

    config.device_id = doc["device_id"] | ReadUniqueDeviceId();
    config.username = doc["username"] | String("Protogen");
    config.user_r = ClampByte(doc["user_r"] | 25);
    config.user_g = ClampByte(doc["user_g"] | 125);
    config.user_b = ClampByte(doc["user_b"] | 235);
    config.ble_rx_uuid = doc["ble_rx_uuid"] | String("8fc048fe-591c-4fb8-a99c-e9f507940964");
    config.ble_tx_uuid = doc["ble_tx_uuid"] | String("42036097-7c76-4129-a80d-ed4e0d12359c");
    config.ota_ssid = doc["ota_ssid"] | String("General ProtogenOTAWiFi");
    config.ota_password = doc["ota_password"] | String("Protogen#1229#25");
    config.wifi_ssid = doc["wifi_ssid"] | String("");
    config.wifi_password = doc["wifi_password"] | String("");
    config.user_animation = doc["user_animation"] | String("");

    // Persist back if device_id was missing.
    if (!LittleFS.exists(kUserConfigPath) || config.device_id.isEmpty())
    {
        SaveUserConfig(config);
    }
    return true;
}

bool ConnectWifiWithNetWizard(UserConfig &config, AsyncWebServer &server, unsigned long connectTimeoutMs, M5GFX *display)
{
    static NetWizard wizard(&server);
    wizard.setStrategy(NetWizardStrategy::BLOCKING);
    wizard.setConnectTimeout(connectTimeoutMs);
    wizard.setHostname(config.device_id.c_str());
    const unsigned long portalReconnectTimeoutMs = connectTimeoutMs < 10000 ? 10000 : connectTimeoutMs;

    // Try stored credentials up to three times before offering the portal.
    bool connected = false;
    IPAddress connectedIp;
    if (!config.wifi_ssid.isEmpty())
    {
        for (int attempt = 1; attempt <= 1 && !connected; ++attempt)
        {
            if (display)
            {
                display->clearDisplay();
                display->setCursor(0, 0);
                display->println(TXT("WiFi connect...", "正在连接WiFi..."));
                display->println(String(TXT("Try ", "尝试 ")) + attempt + "/1");
                display->println(config.wifi_ssid);
                display->display();
            }

            connected = ConnectAndStabilizeWifi(config.wifi_ssid, config.wifi_password, connectTimeoutMs, &connectedIp);
            if (connected)
            {
                if (display)
                {
                    display->clearDisplay();
                    display->setCursor(0, 0);
                    display->println(TXT("WiFi connected", "WiFi已连接"));
                    display->println(connectedIp.toString());
                    display->display();
                }
            }
            else if (display)
            {
                display->clearDisplay();
                display->setCursor(0, 0);
                display->println(TXT("WiFi failed", "WiFi连接失败"));
                display->println(String(TXT("Try ", "尝试 ")) + attempt + "/3");
                display->display();
                delay(400);
            }
        }
    }

    if (connected)
    {
        SaveUserConfig(config);
        return true;
    }

    if (config.wifi_ssid.isEmpty())
    {
        Serial.println("[INFO] No saved WiFi credentials; entering NetWizard setup on first boot");
        return RunNetWizardSetupPortal(config, wizard, portalReconnectTimeoutMs, display, &connectedIp);
    }

    // Prompt user to press button on pin 14 within 10s to enter portal; otherwise continue boot.
    const uint32_t pressWindowMs = 10000;
    uint32_t waitStartMs = millis();
    uint32_t lastUiMs = 0;
    bool pressed = false;

    while ((millis() - waitStartMs) < pressWindowMs && !pressed)
    {
        if (digitalRead(OTA_BTN) == LOW) //14
        {
            pressed = true;
            break;
        }

        if (display)
        {
            uint32_t now = millis();
            if (now - lastUiMs > 250)
            {
                lastUiMs = now;
                uint32_t remaining = pressWindowMs - (now - waitStartMs);
                display->clearDisplay();
                display->setCursor(0, 0);
                display->println(TXT("Press OTA BTN", "按住OTA按钮"));
                display->println(TXT("for WiFi setup", "进入WiFi设置"));
                display->println(String(remaining / 1000) + String(TXT("s left", "秒内完成")));
                display->display();
            }
        }

        delay(50);
    }

    // If button was not pressed, decide whether to skip or auto-enter portal based on local animation availability.
    if (!pressed)
    {
        // Check for local animation files (user-specified or device fallback). If none exist, prompt and enter WiFi setup automatically.
        bool animExists = false;
        if (EnsureFsMounted())
        {
            String animFilename = config.user_animation.length() > 0 ? config.user_animation : (config.device_id + String("_animation.json"));
            String path = "/" + animFilename;
            String fallbackPath = "/example_animation.json";
            if (LittleFS.exists(path) || LittleFS.exists(fallbackPath))
            {
                animExists = true;
            }
        }

        if (!animExists)
        {
            if (display)
            {
                display->clearDisplay();
                display->setCursor(0, 0);
                display->println(String(TXT("Device ID: ", "设备ID: ")) + config.device_id);
                display->println(TXT("Entering WiFi setup", "进入WiFi设置"));
                display->display();
                delay(100);
            }

            return RunNetWizardSetupPortal(config, wizard, portalReconnectTimeoutMs, display, &connectedIp);
        }

        if (display)
        {
            display->clearDisplay();
            display->setCursor(0, 0);
            display->println(TXT("Skip WiFi setup", "跳过WiFi设置"));
            display->println(TXT("Continuing...", "继续启动"));
            display->display();
        }
        return false;
    }

    // Start captive portal / connect flow using username (fallback to device_id) as AP SSID and OTA password as portal password.
    return RunNetWizardSetupPortal(config, wizard, portalReconnectTimeoutMs, display, &connectedIp);
}

bool DownloadUserConfigFromGithub(const char *baseUrl, UserConfig &config, bool verbose, M5GFX *display, const char *githubToken)
{
    return DownloadUserConfigFromSource(BuildRemoteSource(baseUrl, githubToken, "token ", nullptr),
                                        config,
                                        verbose,
                                        display);
}

bool DownloadUserConfigFromGitee(const char *baseUrl, UserConfig &config, bool verbose, M5GFX *display, const char *giteeToken)
{
    return DownloadUserConfigFromSource(BuildRemoteSource(baseUrl,
                                                          giteeToken,
                                                          "Bearer ",
                                                          "application/vnd.github.v3.raw"),
                                        config,
                                        verbose,
                                        display);
}

bool DownloadUserConfigFromSources(const RemoteFileSource *sources,
                                   size_t sourceCount,
                                   UserConfig &config,
                                   bool verbose,
                                   M5GFX *display)
{
    if (sources == nullptr || sourceCount == 0)
    {
        return false;
    }

    if (!EnsureFsMounted())
    {
        return false;
    }

    if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress((uint32_t)0))
    {
        Serial.println("[WARN] Skipping remote user_config.json fetch: WiFi is not ready");
        return false;
    }

    const String prevWifiSsid = config.wifi_ssid;
    const String prevWifiPassword = config.wifi_password;

    if (display)
    {
        display->clearDisplay();
        display->setCursor(0, 0);
        display->println(TXT("Checking config...", "检查配置中..."));
        display->display();
    }

    const String remoteFilename = config.device_id + String(".json");
    if (!RemoteFileSync::SyncAny(sources,
                                 sourceCount,
                                 remoteFilename,
                                 kUserConfigPath,
                                 BuildUserConfigSyncOptions(display, verbose)))
    {
        return false;
    }

    return ReloadDownloadedConfigPreservingWifi(config, prevWifiSsid, prevWifiPassword);
}
