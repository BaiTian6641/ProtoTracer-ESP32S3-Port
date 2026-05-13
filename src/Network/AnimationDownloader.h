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
    // Probe the configured remotes and prefer the lower-latency source first.
    static bool Download(const AnimationDownloadConfig &cfg, const String &filename);
};
