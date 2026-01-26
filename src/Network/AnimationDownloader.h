#pragma once

#include <Arduino.h>

class M5UnitGLASS2;

struct AnimationDownloadConfig
{
    const char *githubBase = nullptr;
    const char *giteeBase = nullptr;
    const char *githubToken = nullptr;
    const char *giteeToken = nullptr;
    M5UnitGLASS2 *progressDisplay = nullptr;
    bool verbose = false;
};

class AnimationDownloader
{
public:
    // Try GitHub first, then Gitee. Returns true if any download succeeded.
    static bool Download(const AnimationDownloadConfig &cfg, const String &filename);

private:
    static bool DownloadFrom(const char *baseUrl, const char *token, const String &filename, M5UnitGLASS2 *display, bool verbose);
};
