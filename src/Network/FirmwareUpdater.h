#pragma once

#include <Arduino.h>

#include "RemoteFileSync.h"

class M5UnitGLASS2;

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
    M5UnitGLASS2 *display = nullptr;
    bool verbose = false;
};

class FirmwareUpdater
{
public:
    static FirmwareUpdateResult CheckAndUpdate(const FirmwareUpdateConfig &config);
};