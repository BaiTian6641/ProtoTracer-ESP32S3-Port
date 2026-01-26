#include "UserConfigManager.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <NetWizard.h>
#include <WiFi.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <HTTPClient.h>
#include <MD5Builder.h>

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

    bool EnsureFsMounted()
    {
        static bool mounted = false;
        if (mounted)
        {
            return true;
        }
        if (LittleFS.begin(false) || LittleFS.begin(true))
        {
            mounted = true;
        }
        return mounted;
    }

    uint8_t ClampByte(long v)
    {
        if (v < 0)
            return 0;
        if (v > 255)
            return 255;
        return static_cast<uint8_t>(v);
    }

    String ComputeFileMd5(const String &path)
    {
        File f = LittleFS.open(path, "r");
        if (!f)
        {
            return String();
        }

        MD5Builder md5;
        md5.begin();
        uint8_t buffer[512];
        while (f.available())
        {
            size_t n = f.read(buffer, sizeof(buffer));
            if (n > 0)
            {
                md5.add(buffer, n);
            }
        }
        f.close();
        md5.calculate();
        return md5.toString();
    }

    String FetchRemoteMd5(const String &baseUrl, const String &filename, const char *authHeader, const String &authValue, const char *acceptHeader)
    {
        if (baseUrl.isEmpty())
        {
            return String();
        }

        String url = baseUrl;
        if (!url.endsWith("/"))
        {
            url += '/';
        }
        url += filename; // expected to already end with .md5

        HTTPClient http;
        if (!http.begin(url))
        {
            return String();
        }

        if (authHeader && authHeader[0] != '\0' && authValue.length() > 0)
        {
            http.addHeader(authHeader, authValue);
        }
        if (acceptHeader && acceptHeader[0] != '\0')
        {
            http.addHeader("Accept", acceptHeader);
        }

        int code = http.GET();
        if (code != HTTP_CODE_OK)
        {
            http.end();
            return String();
        }

        String body = http.getString();
        body.trim();
        http.end();
        return body;
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

    DynamicJsonDocument doc(640);
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

    DynamicJsonDocument doc(640);
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

bool ConnectWifiWithNetWizard(UserConfig &config, AsyncWebServer &server, unsigned long connectTimeoutMs, M5UnitGLASS2 *display)
{
    static NetWizard wizard(&server);
    wizard.setStrategy(NetWizardStrategy::BLOCKING);
    wizard.setConnectTimeout(connectTimeoutMs);
    wizard.setHostname(config.device_id.c_str());

    // Try stored credentials up to three times before offering the portal.
    bool connected = false;
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

            WiFi.mode(WIFI_STA);
            WiFi.begin(config.wifi_ssid.c_str(), config.wifi_password.c_str());

            uint32_t start = millis();
            while (WiFi.status() != WL_CONNECTED && (millis() - start) < connectTimeoutMs)
            {
                delay(100);
            }

            connected = (WiFi.status() == WL_CONNECTED);
            if (connected)
            {
                if (display)
                {
                    display->clearDisplay();
                    display->setCursor(0, 0);
                    display->println(TXT("WiFi connected", "WiFi已连接"));
                    display->println(WiFi.localIP().toString());
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

            const char *apSsid = config.username.isEmpty() ? config.device_id.c_str() : config.username.c_str();
            wizard.autoConnect(apSsid, config.ota_password.c_str());

            bool connectedPortal = (WiFi.status() == WL_CONNECTED);

            // Mirror resolved credentials back into config and persist.
            config.wifi_ssid = wizard.getSSID();
            config.wifi_password = wizard.getPassword();
            SaveUserConfig(config);

            if (display)
            {
                display->clearDisplay();
                display->setCursor(0, 0);
                display->println(connectedPortal ? TXT("WiFi connected", "WiFi已连接") : TXT("WiFi setup done", "WiFi设置完成"));
                if (connectedPortal)
                {
                    display->println(WiFi.localIP().toString());
                }
                display->display();
            }

            return connectedPortal;
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
    const char *apSsid = config.username.isEmpty() ? config.device_id.c_str() : config.username.c_str();
    wizard.autoConnect(apSsid, config.ota_password.c_str());

    connected = (WiFi.status() == WL_CONNECTED);

    // Mirror resolved credentials back into config and persist.
    config.wifi_ssid = wizard.getSSID();
    config.wifi_password = wizard.getPassword();
    SaveUserConfig(config);

    if (display)
    {
        display->clearDisplay();
        display->setCursor(0, 0);
        display->println(connected ? TXT("WiFi connected", "WiFi已连接") : TXT("WiFi setup done", "WiFi设置完成"));
        if (connected)
        {
            display->println(WiFi.localIP().toString());
        }
        display->display();
    }

    return connected;
}

bool DownloadUserConfigFromGithub(const char *baseUrl, UserConfig &config, bool verbose, M5UnitGLASS2 *display, const char *githubToken)
{
    if (!EnsureFsMounted())
    {
        return false;
    }

    // Preserve local Wi-Fi credentials; remote file should not override them to avoid config loops.
    String prevWifiSsid = config.wifi_ssid;
    String prevWifiPassword = config.wifi_password;

    // MD5 guard: compare local file hash to remote <device_id>.md5 to avoid redundant downloads.
    String md5Filename = config.device_id + String(".md5");
    String localMd5;
    if (LittleFS.exists(kUserConfigPath))
    {
        localMd5 = ComputeFileMd5(kUserConfigPath);
    }

    String url = baseUrl;
    if (!url.endsWith("/"))
    {
        url += '/';
    }
    url += config.device_id;
    url += ".json";

    String remoteMd5 = FetchRemoteMd5(String(baseUrl), md5Filename, (githubToken && githubToken[0] != '\0') ? "Authorization" : "", (githubToken && githubToken[0] != '\0') ? String("token ") + githubToken : String(""), "");

    if (!remoteMd5.isEmpty() && !localMd5.isEmpty())
    {
        if (display)
        {
            display->clearDisplay();
            display->setCursor(0, 0);
            display->println(TXT("Checking MD5...", "校验MD5..."));
            display->display();
            delay(100);
        }

        if (remoteMd5.equalsIgnoreCase(localMd5))
        {
            if (display && verbose)
            {
                display->clearDisplay();
                display->setCursor(0, 0);
                display->println(TXT("Config up-to-date", "配置已最新"));
                display->display();
            }
            return true; // skip download
        }
        else if (display)
        {
            display->clearDisplay();
            display->setCursor(0, 0);
            display->println(TXT("MD5 mismatch, redownloading...", "MD5不一致，重新下载"));
            display->display();
            delay(100);
        }
    }

    HTTPClient http;
    if (!http.begin(url))
    {
        return false;
    }

    // Add Authorization header when a token is provided (for private repos).
    if (githubToken != nullptr && githubToken[0] != '\0')
    {
        http.addHeader("Authorization", String("token ") + githubToken);
    }

    int code = http.GET();
    if (code != HTTP_CODE_OK)
    {
        // If file is not found on the server, show device ID so user can register it.
        if (code == HTTP_CODE_NOT_FOUND || code == 404)
        {
            display->clearDisplay();
            display->setCursor(0, 0);
            display->println(TXT("Device not registered!", "设备未注册！"));
            display->println(String(TXT("Device ID: ", "设备ID: ")) + config.device_id);
            display->display();
            while (true)
            {
                /* code */
                delay(1); //Do nothing loop, unless reset
            }
            
        }
        http.end();
        return false;
    }

    File f = LittleFS.open(kUserConfigPath, "w");
    if (!f)
    {
        http.end();
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    uint8_t buffer[512];
    int32_t remaining = http.getSize();
    int32_t totalSize = remaining; // -1 when unknown
    int32_t downloaded = 0;
    uint32_t lastUpdateMs = millis();

    while (http.connected() && (remaining > 0 || remaining == -1))
    {
        size_t available = stream->available();
        if (available)
        {
            int toRead = available;
            if (toRead > (int)sizeof(buffer))
                toRead = sizeof(buffer);

            int readCount = stream->readBytes(buffer, toRead);
            if (readCount > 0)
            {
                f.write(buffer, readCount);
                downloaded += readCount;
                if (remaining > 0)
                {
                    remaining -= readCount;
                }

                // Update on-screen progress occasionally when a display is provided.
                if (verbose && display != nullptr)
                {
                    uint32_t now = millis();
                    if (now - lastUpdateMs > 150)
                    {
                        lastUpdateMs = now;
                        display->clearDisplay();
                        display->setCursor(0, 0);
                        display->println(TXT("Downloading config...", "正在下载配置..."));
                        if (totalSize > 0)
                        {
                            int percent = (downloaded * 100) / totalSize;
                            display->println(String(percent) + "%");
                        }
                        else
                        {
                            display->println(String(downloaded / 1024) + TXT(" KB", " KB"));
                        }
                        display->display();
                    }
                }
            }
        }
        delay(1);
    }

    f.close();
    http.end();

    if (!remoteMd5.isEmpty())
    {
        String downloadedMd5 = ComputeFileMd5(kUserConfigPath);
        if (!downloadedMd5.equalsIgnoreCase(remoteMd5))
        {
            if (display && verbose)
            {
                display->clearDisplay();
                display->setCursor(0, 0);
                display->println(TXT("MD5 verify failed", "MD5校验失败"));
                display->display();
            }
            return false;
        }
    }

    if (verbose && display != nullptr)
    {
        display->clearDisplay();
        display->setCursor(0, 0);
        display->println(TXT("Config updated", "配置已更新"));
        display->display();
    }

    // Reload config from freshly downloaded file.
    bool ok = EnsureUserConfig(config);
    if (ok)
    {
        config.wifi_ssid = prevWifiSsid;
        config.wifi_password = prevWifiPassword;
        SaveUserConfig(config);
    }
    return ok;
}

bool DownloadUserConfigFromGitee(const char *baseUrl, UserConfig &config, bool verbose, M5UnitGLASS2 *display, const char *giteeToken)
{
    if (!EnsureFsMounted())
    {
        return false;
    }

    // Preserve local Wi-Fi credentials; remote file should not override them to avoid config loops.
    String prevWifiSsid = config.wifi_ssid;
    String prevWifiPassword = config.wifi_password;

    // MD5 guard: compare local file hash to remote <device_id>.md5 to avoid redundant downloads.
    String md5Filename = config.device_id + String(".md5");
    String localMd5;
    if (LittleFS.exists(kUserConfigPath))
    {
        localMd5 = ComputeFileMd5(kUserConfigPath);
    }

    String url = baseUrl;
    if (!url.endsWith("/"))
    {
        url += '/';
    }
    url += config.device_id;
    url += ".json";

    String remoteMd5 = FetchRemoteMd5(String(baseUrl), md5Filename, (giteeToken && giteeToken[0] != '\0') ? "Authorization" : "", (giteeToken && giteeToken[0] != '\0') ? String("Bearer ") + giteeToken : String(""), "application/vnd.github.v3.raw");

    if (!remoteMd5.isEmpty() && !localMd5.isEmpty())
    {
        if (display)
        {
            display->clearDisplay();
            display->setCursor(0, 0);
            display->println(TXT("Checking MD5...", "校验MD5..."));
            display->display();
            delay(100);
        }

        if (remoteMd5.equalsIgnoreCase(localMd5))
        {
            if (display && verbose)
            {
                display->clearDisplay();
                display->setCursor(0, 0);
                display->println(TXT("Config up-to-date", "配置已最新"));
                display->display();
            }
            return true; // skip download
        }
        else if (display)
        {
            display->clearDisplay();
            display->setCursor(0, 0);
            display->println(TXT("MD5 mismatch, redownloading...", "MD5不一致，重新下载"));
            display->display();
            delay(100);
        }
    }

    HTTPClient http;
    if (!http.begin(url))
    {
        return false;
    }

    if (giteeToken != nullptr && giteeToken[0] != '\0')
    {
        http.addHeader("Authorization", String("Bearer ") + giteeToken);
    }
    http.addHeader("Accept", "application/vnd.github.v3.raw");

    int code = http.GET();
    if (code != HTTP_CODE_OK)
    {
        // If file is not found on the server, show device ID so user can register it.
        if (code == HTTP_CODE_NOT_FOUND || code == 404)
        {
            if (display)
            {
                display->clearDisplay();
                display->setCursor(0, 0);
                display->println(TXT("Device not registered!", "设备未注册！"));
                display->println(String(TXT("Device ID: ", "设备ID: ")) + config.device_id);
                display->display();
                delay(100);
            }
        }
        http.end();
        return false;
    }

    File f = LittleFS.open(kUserConfigPath, "w");
    if (!f)
    {
        http.end();
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    uint8_t buffer[512];
    int32_t remaining = http.getSize();
    int32_t totalSize = remaining; // -1 when unknown
    int32_t downloaded = 0;
    uint32_t lastUpdateMs = millis();

    while (http.connected() && (remaining > 0 || remaining == -1))
    {
        size_t available = stream->available();
        if (available)
        {
            int toRead = available;
            if (toRead > (int)sizeof(buffer))
                toRead = sizeof(buffer);

            int readCount = stream->readBytes(buffer, toRead);
            if (readCount > 0)
            {
                f.write(buffer, readCount);
                downloaded += readCount;
                if (remaining > 0)
                {
                    remaining -= readCount;
                }

                // Update on-screen progress occasionally when a display is provided.
                if (verbose && display != nullptr)
                {
                    uint32_t now = millis();
                    if (now - lastUpdateMs > 150)
                    {
                        lastUpdateMs = now;
                        display->clearDisplay();
                        display->setCursor(0, 0);
                        display->println(TXT("Downloading config...", "正在下载配置..."));
                        if (totalSize > 0)
                        {
                            int percent = (downloaded * 100) / totalSize;
                            display->println(String(percent) + "%");
                        }
                        else
                        {
                            display->println(String(downloaded / 1024) + TXT(" KB", " KB"));
                        }
                        display->display();
                    }
                }
            }
        }
        delay(1);
    }

    f.close();
    http.end();

    if (!remoteMd5.isEmpty())
    {
        String downloadedMd5 = ComputeFileMd5(kUserConfigPath);
        if (!downloadedMd5.equalsIgnoreCase(remoteMd5))
        {
            if (display && verbose)
            {
                display->clearDisplay();
                display->setCursor(0, 0);
                display->println(TXT("MD5 verify failed", "MD5校验失败"));
                display->display();
            }
            return false;
        }
    }

    if (verbose && display != nullptr)
    {
        display->clearDisplay();
        display->setCursor(0, 0);
        display->println(TXT("Config updated", "配置已更新"));
        display->display();
    }

    // Reload config from freshly downloaded file.
    bool ok = EnsureUserConfig(config);
    if (ok)
    {
        config.wifi_ssid = prevWifiSsid;
        config.wifi_password = prevWifiPassword;
        SaveUserConfig(config);
    }
    return ok;
}
