#pragma once

#include <Arduino.h>
#include <M5GFX.h>

#include "RemoteFileSync.h"

enum class FirmwareUpdateResult
{
    Skipped,
    UpToDate,
    Updated,
    Failed,
};

struct FirmwareUpdateConfig
{
    RemoteFileSource primarySource;
    const char *primaryName = "primary";
    RemoteFileSource fallbackSource;
    const char *fallbackName = "fallback";
    const char *manifestFilename = nullptr;
    const char *currentVersion = nullptr;
    M5GFX *display = nullptr;
    bool verbose = false;
};

class FirmwareUpdater
{
public:
    static FirmwareUpdateResult CheckAndUpdate(const FirmwareUpdateConfig &config);
};