#pragma once

#include <M5UnitGLASS2.h>

struct FaceUpdateConfig
{
    const char *download_ssid;
    const char *download_password;
    const char *base_url;
    const char *auth_token;
    const char *accept_header;
    const char *auth_scheme;
    const char *name = "remote";
};

// Ensure a device-specific or fallback universal face JSON is present and up to date.
bool EnsureFaceModelJson(const FaceUpdateConfig &config, const String &deviceId, M5GFX *display, bool verbose);
bool EnsureFaceModelJson(const FaceUpdateConfig *configs, size_t sourceCount, const String &deviceId, M5GFX *display, bool verbose);
