#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <M5UnitGLASS2.h>

struct UserConfig
{
    String device_id;
    String username;
    uint8_t user_r;
    uint8_t user_g;
    uint8_t user_b;
    String ble_rx_uuid;
    String ble_tx_uuid;
    String ota_ssid;
    String ota_password;
    String wifi_ssid;
    String wifi_password;
    String user_animation; // reserved for future animation selection
};

// Ensure the user config JSON exists on LittleFS and load it into memory.
// If missing or invalid, a default config is written first.
bool EnsureUserConfig(UserConfig &config);

// Save the given config back to LittleFS.
bool SaveUserConfig(const UserConfig &config);

// Read the ESP32-S3 unique ID as a lowercase hex string.
String ReadUniqueDeviceId();

// Connect to Wi-Fi using stored credentials (three attempts) and optionally enter NetWizard captive portal
// if the user holds button on pin 14 for 15 seconds. Updates stored Wi-Fi credentials. Displays status
// when a display is provided.
bool ConnectWifiWithNetWizard(UserConfig &config, AsyncWebServer &server, unsigned long connectTimeoutMs = 15000, M5UnitGLASS2 *display = nullptr);

// Download user_config.json from a GitHub base URL using device_id as filename (<base>/<device_id>.json).
// Optional: provide a GitHub token for private repos via the Authorization header.
bool DownloadUserConfigFromGithub(const char *baseUrl, UserConfig &config, bool verbose = false, M5UnitGLASS2 *display = nullptr, const char *githubToken = nullptr);

// Download user_config.json from a Gitee base URL using device_id as filename (<base>/<device_id>.json).
// Provide a personal access token for private repos via the Authorization header (Bearer).
bool DownloadUserConfigFromGitee(const char *baseUrl, UserConfig &config, bool verbose = false, M5UnitGLASS2 *display = nullptr, const char *giteeToken = nullptr);
