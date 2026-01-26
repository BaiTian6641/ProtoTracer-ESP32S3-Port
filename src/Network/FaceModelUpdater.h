#pragma once

#include <M5UnitGLASS2.h>

struct FaceUpdateConfig
{
    const char *download_ssid;
    const char *download_password;
    const char *face_json_url;
    const char *face_checksum_url;
    const char *face_path;
    const char *auth_token;     // Optional Bearer token for private repos (e.g., Gitee)
    const char *accept_header;  // Optional Accept header (e.g., "application/vnd.github.v3.raw")
};

// Ensure the face JSON is present and up to date; downloads if missing or outdated.
bool EnsureUniversalFaceJson(const FaceUpdateConfig &config, M5UnitGLASS2 &display, bool verbose);
