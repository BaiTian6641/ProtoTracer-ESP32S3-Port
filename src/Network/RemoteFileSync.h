#pragma once

#include <Arduino.h>
#include <M5GFX.h>

class M5UnitGLASS2;

struct RemoteFileSource
{
    const char *baseUrl = nullptr;
    const char *token = nullptr;
    const char *authScheme = nullptr;
    const char *acceptHeader = nullptr;
    const char *name = "remote";
};

struct RemoteFileUiText
{
    const char *checkingMd5 = "Checking MD5...";
    const char *upToDate = "File up-to-date";
    const char *md5Mismatch = "MD5 mismatch, redownloading...";
    const char *downloading = "Downloading file...";
    const char *success = "Download OK";
    const char *httpBeginFail = "HTTP begin failed";
    const char *httpGetFail = "HTTP GET failed";
    const char *md5VerifyFail = "MD5 verify failed";
    const char *keepLocal = "Keeping local file";
};

struct RemoteFileSyncOptions
{
    M5GFX *display = nullptr;
    bool verbose = false;
    bool keepExistingWhenRemoteMd5Unavailable = false;
    // Disabled by default: the initial source probe in setup() already
    // determines and caches the faster remote; subsequent downloads
    // should trust the pre-selected source order instead of re-probing
    // every file (which wastes time and fragments internal DRAM).
    bool enableLatencyProbe = false;
    bool skipRemoteMd5 = false;
    uint32_t connectTimeoutMs = 0;
    uint32_t requestTimeoutMs = 0;
    uint32_t idleTimeoutMs = 0;
    RemoteFileUiText ui;
};

struct RemoteFileSourceSelection
{
    const RemoteFileSource *source = nullptr;
    int sourceIndex = -1;
    uint32_t latencyMs = 0;
    bool latencyKnown = false;
};

class RemoteFileSync
{
public:
    static bool EnsureFsMounted();
    static bool IsSourceConfigured(const RemoteFileSource &source);
    static size_t SelectSourcesByLatency(const RemoteFileSource *sources,
                                         size_t sourceCount,
                                         const String &probeFilename,
                                         RemoteFileSourceSelection *orderedSelections,
                                         size_t selectionCapacity);
    static String ComputeFileMd5(const String &path);
    static bool Sync(const RemoteFileSource &source,
                     const String &remoteFilename,
                     const String &localPath,
                     const RemoteFileSyncOptions &options);
    static bool SyncAny(const RemoteFileSource *sources,
                        size_t sourceCount,
                        const String &remoteFilename,
                        const String &localPath,
                        const RemoteFileSyncOptions &options,
                        int *usedSourceIndex = nullptr);
};