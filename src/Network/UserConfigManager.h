#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <M5UnitGLASS2.h>

struct RemoteFileSource;

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

// Connect to Wi-Fi using stored credentials and enter NetWizard captive portal when needed.
// On first boot with no saved Wi-Fi, setup starts automatically. When a display is provided,
// the setup AP SSID/password/IP and connection state are shown during the portal flow.
bool ConnectWifiWithNetWizard(UserConfig &config, AsyncWebServer &server, unsigned long connectTimeoutMs = 15000, M5GFX *display = nullptr);

// Download user_config.json from a GitHub base URL using device_id as filename (<base>/<device_id>.json).
// Optional: provide a GitHub token for private repos via the Authorization header.
bool DownloadUserConfigFromGithub(const char *baseUrl, UserConfig &config, bool verbose = false, M5GFX *display = nullptr, const char *githubToken = nullptr);

// Download user_config.json from a Gitee base URL using device_id as filename (<base>/<device_id>.json).
// Provide a personal access token for private repos via the Authorization header (Bearer).
bool DownloadUserConfigFromGitee(const char *baseUrl, UserConfig &config, bool verbose = false, M5GFX *display = nullptr, const char *giteeToken = nullptr);

// Probe the configured remotes and download user_config.json from the lower-latency source first.
bool DownloadUserConfigFromSources(const RemoteFileSource *sources, size_t sourceCount, UserConfig &config, bool verbose = false, M5GFX *display = nullptr);
