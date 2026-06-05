#pragma once

#include <Arduino.h>
#include <M5GFX.h>

struct AnimationDownloadConfig
{
    const char *githubBase = nullptr;
    const char *giteeBase = nullptr;
    const char *githubToken = nullptr;
    const char *giteeToken = nullptr;
    M5GFX *progressDisplay = nullptr;
    bool verbose = false;
};

class AnimationDownloader
{
public:
    // Probe the configured remotes and prefer the lower-latency source first.
    static bool Download(const AnimationDownloadConfig &cfg, const String &filename);
};
