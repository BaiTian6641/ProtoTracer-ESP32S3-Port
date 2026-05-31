#include "RemoteFileSync.h"

#include <FS.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <M5UnitGLASS2.h>
#include <MD5Builder.h>
#include <WiFi.h>

namespace
{
    constexpr uint32_t kHttpProbeConnectTimeoutMs = 2000;
    constexpr uint32_t kHttpProbeRequestTimeoutMs = 3000;
    constexpr uint32_t kHttpConnectTimeoutMs = 5000;
    constexpr uint32_t kHttpRequestTimeoutMs = 10000;
    constexpr uint32_t kHttpMd5ConnectTimeoutMs = 3000;
    constexpr uint32_t kHttpMd5RequestTimeoutMs = 5000;
    constexpr uint32_t kHttpIdleTimeoutMs = 5000;
    constexpr size_t kMaxSelectableSourceCount = 4;

    const char *SourceName(const RemoteFileSource &source)
    {
        return (source.name != nullptr && source.name[0] != '\0') ? source.name : "remote";
    }

    String BuildUrl(const char *baseUrl, const String &filename)
    {
        String url = baseUrl;
        if (!url.endsWith("/"))
        {
            url += '/';
        }
        url += filename;
        return url;
    }

    void ApplyHeaders(HTTPClient &http, const RemoteFileSource &source)
    {
        if (source.token != nullptr && source.token[0] != '\0')
        {
            String authValue;
            if (source.authScheme != nullptr && source.authScheme[0] != '\0')
            {
                authValue += source.authScheme;
            }
            authValue += source.token;
            http.addHeader("Authorization", authValue);
        }

        if (source.acceptHeader != nullptr && source.acceptHeader[0] != '\0')
        {
            http.addHeader("Accept", source.acceptHeader);
        }
    }

    void ShowStatus(M5UnitGLASS2 *display, const char *message, bool shouldShow)
    {
        if (!display || !shouldShow || message == nullptr || message[0] == '\0')
        {
            return;
        }

        display->clearDisplay();
        display->setCursor(0, 0);
        display->println(message);
        display->display();
    }

    String DeriveMd5Filename(const String &remoteFilename)
    {
        if (remoteFilename.endsWith(".json"))
        {
            return remoteFilename.substring(0, remoteFilename.length() - 5) + ".md5";
        }
        return remoteFilename + ".md5";
    }

    String FetchRemoteMd5(const RemoteFileSource &source, const String &remoteFilename)
    {
        if (source.baseUrl == nullptr || source.baseUrl[0] == '\0')
        {
            return String();
        }

        HTTPClient http;
        String url = BuildUrl(source.baseUrl, DeriveMd5Filename(remoteFilename));
        http.setConnectTimeout(kHttpMd5ConnectTimeoutMs);
        http.setTimeout(kHttpMd5RequestTimeoutMs);
        if (!http.begin(url))
        {
            return String();
        }

        ApplyHeaders(http, source);

        int code = http.GET();
        if (code != HTTP_CODE_OK)
        {
            Serial.printf("[WARN] Remote MD5 fetch failed (%d) for %s\n", code, url.c_str());
            http.end();
            return String();
        }

        String body = http.getString();
        body.trim();
        body.toLowerCase();
        http.end();
        return body;
    }

    bool ProbeSourceLatency(const RemoteFileSource &source,
                            const String &probeFilename,
                            uint32_t &latencyMs)
    {
        if (!RemoteFileSync::IsSourceConfigured(source))
        {
            return false;
        }

        HTTPClient http;
        const String url = BuildUrl(source.baseUrl, probeFilename);
        http.setConnectTimeout(kHttpProbeConnectTimeoutMs);
        http.setTimeout(kHttpProbeRequestTimeoutMs);

        const uint32_t startMs = millis();
        if (!http.begin(url))
        {
            return false;
        }

        ApplyHeaders(http, source);

        const int code = http.GET();
        latencyMs = millis() - startMs;
        http.end();

        return code == HTTP_CODE_OK;
    }

    bool ReplaceFileAtomically(const String &tempPath, const String &finalPath)
    {
        if (LittleFS.exists(finalPath))
        {
            LittleFS.remove(finalPath);
        }

        if (LittleFS.rename(tempPath, finalPath))
        {
            return true;
        }

        File src = LittleFS.open(tempPath, "r");
        if (!src)
        {
            return false;
        }

        File dst = LittleFS.open(finalPath, "w");
        if (!dst)
        {
            src.close();
            return false;
        }

        uint8_t buffer[512];
        while (src.available())
        {
            size_t readCount = src.read(buffer, sizeof(buffer));
            if (readCount == 0)
            {
                break;
            }

            if (dst.write(buffer, readCount) != readCount)
            {
                dst.close();
                src.close();
                LittleFS.remove(finalPath);
                return false;
            }
        }

        dst.close();
        src.close();
        LittleFS.remove(tempPath);
        return true;
    }
}

bool RemoteFileSync::EnsureFsMounted()
{
    static bool mounted = false;
    if (mounted)
    {
        return true;
    }

    // Try to mount without format first, with watchdog protection against I/O hangs
    const uint32_t mountTimeoutMs = 5000;
    const uint32_t startMs = millis();
    
    // Attempt mount without format (faster, non-destructive)
    if (LittleFS.begin(false))
    {
        mounted = true;
        return true;
    }
    
    // Watchdog check: if mount took too long, abandon second attempt to avoid double hang
    if ((millis() - startMs) > (mountTimeoutMs / 2))
    {
        Serial.printf("[WARN] LittleFS first mount attempt took >%lu ms; skipping format attempt\n", mountTimeoutMs / 2);
        return false;
    }
    
    // If non-format mount failed, try with format (destructive, slower)
    if (LittleFS.begin(true))
    {
        mounted = true;
        Serial.println("[INFO] LittleFS mounted after format");
        return true;
    }
    
    // Total mount failure
    Serial.printf("[ERROR] LittleFS mount failed after %lu ms (total operation)\n", millis() - startMs);
    return false;
}

bool RemoteFileSync::IsSourceConfigured(const RemoteFileSource &source)
{
    return source.baseUrl != nullptr && source.baseUrl[0] != '\0';
}

size_t RemoteFileSync::SelectSourcesByLatency(const RemoteFileSource *sources,
                                              size_t sourceCount,
                                              const String &probeFilename,
                                              RemoteFileSourceSelection *orderedSelections,
                                              size_t selectionCapacity)
{
    if (sources == nullptr || orderedSelections == nullptr || selectionCapacity == 0)
    {
        return 0;
    }

    size_t count = 0;
    for (size_t i = 0; i < sourceCount && count < selectionCapacity; ++i)
    {
        if (!IsSourceConfigured(sources[i]))
        {
            continue;
        }

        orderedSelections[count].source = &sources[i];
        orderedSelections[count].sourceIndex = static_cast<int>(i);
        orderedSelections[count].latencyMs = 0;
        orderedSelections[count].latencyKnown = false;
        ++count;
    }

    if (count <= 1 || probeFilename.isEmpty())
    {
        return count;
    }

    for (size_t i = 0; i < count; ++i)
    {
        uint32_t latencyMs = 0;
        if (ProbeSourceLatency(*orderedSelections[i].source, probeFilename, latencyMs))
        {
            orderedSelections[i].latencyMs = latencyMs;
            orderedSelections[i].latencyKnown = true;
            Serial.printf("[INFO] %s latency probe for %s: %lu ms\n",
                          SourceName(*orderedSelections[i].source),
                          probeFilename.c_str(),
                          static_cast<unsigned long>(latencyMs));
        }
        else
        {
            Serial.printf("[WARN] %s latency probe failed for %s\n",
                          SourceName(*orderedSelections[i].source),
                          probeFilename.c_str());
        }
    }

    for (size_t i = 0; i + 1 < count; ++i)
    {
        for (size_t j = i + 1; j < count; ++j)
        {
            const bool leftKnown = orderedSelections[i].latencyKnown;
            const bool rightKnown = orderedSelections[j].latencyKnown;

            bool shouldSwap = false;
            if (!leftKnown && rightKnown)
            {
                shouldSwap = true;
            }
            else if (leftKnown && rightKnown && orderedSelections[j].latencyMs < orderedSelections[i].latencyMs)
            {
                shouldSwap = true;
            }

            if (shouldSwap)
            {
                RemoteFileSourceSelection temp = orderedSelections[i];
                orderedSelections[i] = orderedSelections[j];
                orderedSelections[j] = temp;
            }
        }
    }

    if (count > 1)
    {
        if (orderedSelections[0].latencyKnown && orderedSelections[1].latencyKnown)
        {
            Serial.printf("[INFO] Selecting %s before %s for %s based on latency (%lu ms vs %lu ms)\n",
                          SourceName(*orderedSelections[0].source),
                          SourceName(*orderedSelections[1].source),
                          probeFilename.c_str(),
                          static_cast<unsigned long>(orderedSelections[0].latencyMs),
                          static_cast<unsigned long>(orderedSelections[1].latencyMs));
        }
        else if (orderedSelections[0].latencyKnown)
        {
            Serial.printf("[INFO] Selecting %s first for %s; alternate latency probe failed\n",
                          SourceName(*orderedSelections[0].source),
                          probeFilename.c_str());
        }
    }

    return count;
}

String RemoteFileSync::ComputeFileMd5(const String &path)
{
    File file = LittleFS.open(path, "r");
    if (!file)
    {
        return String();
    }

    MD5Builder md5;
    md5.begin();
    uint8_t buffer[512];
    while (file.available())
    {
        size_t readCount = file.read(buffer, sizeof(buffer));
        if (readCount > 0)
        {
            md5.add(buffer, readCount);
        }
    }

    file.close();
    md5.calculate();
    return md5.toString();
}

bool RemoteFileSync::Sync(const RemoteFileSource &source,
                          const String &remoteFilename,
                          const String &localPath,
                          const RemoteFileSyncOptions &options)
{
    if (source.baseUrl == nullptr || source.baseUrl[0] == '\0')
    {
        return false;
    }

    if (!EnsureFsMounted())
    {
        return false;
    }

    String localMd5;
    const bool localExists = LittleFS.exists(localPath);
    if (localExists)
    {
        localMd5 = ComputeFileMd5(localPath);
    }

    // Only fetch remote MD5 when we have a local file to compare against.
    // If no local file exists, skip the MD5 round-trip and download directly.
    String remoteMd5;
    if (localExists)
    {
        remoteMd5 = FetchRemoteMd5(source, remoteFilename);
    }

    if (!remoteMd5.isEmpty() && !localMd5.isEmpty())
    {
        ShowStatus(options.display, options.ui.checkingMd5, true);
        if (remoteMd5.equalsIgnoreCase(localMd5))
        {
            ShowStatus(options.display, options.ui.upToDate, options.verbose);
            return true;
        }

        ShowStatus(options.display, options.ui.md5Mismatch, options.display != nullptr);
    }
    else if (localExists && options.keepExistingWhenRemoteMd5Unavailable)
    {
        ShowStatus(options.display, options.ui.keepLocal, options.verbose);
        return true;
    }

    HTTPClient http;
    String url = BuildUrl(source.baseUrl, remoteFilename);
    http.setConnectTimeout(kHttpConnectTimeoutMs);
    http.setTimeout(kHttpRequestTimeoutMs);
    if (!http.begin(url))
    {
        ShowStatus(options.display, options.ui.httpBeginFail, options.verbose);
        return false;
    }

    ApplyHeaders(http, source);

    int code = http.GET();
    if (code != HTTP_CODE_OK)
    {
        Serial.printf("[WARN] HTTP GET failed (%d) for %s\n", code, url.c_str());
        http.end();
        ShowStatus(options.display, options.ui.httpGetFail, options.verbose);
        return false;
    }

    if (options.display)
    {
        options.display->clearDisplay();
        options.display->setCursor(0, 0);
        options.display->println(options.ui.downloading);
        options.display->display();
    }

    const String tempPath = localPath + ".tmp";
    if (LittleFS.exists(tempPath))
    {
        LittleFS.remove(tempPath);
    }

    File tempFile = LittleFS.open(tempPath, "w");
    if (!tempFile)
    {
        http.end();
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    uint8_t buffer[512];
    int32_t remaining = http.getSize();
    const int32_t totalSize = remaining;
    uint32_t downloaded = 0;
    int lastPct = -1;
    unsigned long lastUi = 0;
    bool writeFailed = false;
    uint32_t lastDataMs = millis();
    bool downloadStalled = false;

    while (http.connected() && (remaining > 0 || remaining == -1))
    {
        size_t available = stream->available();
        if (available)
        {
            int toRead = available;
            if (toRead > (int)sizeof(buffer))
            {
                toRead = sizeof(buffer);
            }

            int readCount = stream->readBytes(buffer, toRead);
            if (readCount > 0)
            {
                if (tempFile.write(buffer, readCount) != (size_t)readCount)
                {
                    writeFailed = true;
                    break;
                }

                downloaded += readCount;
                lastDataMs = millis();
                if (remaining > 0)
                {
                    remaining -= readCount;
                }

                if (options.display && totalSize > 0)
                {
                    int pct = (int)((downloaded * 100) / totalSize);
                    unsigned long now = millis();
                    if (pct != lastPct || now - lastUi > 200)
                    {
                        lastPct = pct;
                        lastUi = now;
                        options.display->progressBar(14, 50, 100, 8, pct);
                        options.display->display();
                    }
                }
            }
        }
        else if (millis() - lastDataMs > kHttpIdleTimeoutMs)
        {
            Serial.printf("[WARN] Remote download stalled for %s\n", url.c_str());
            downloadStalled = true;
            break;
        }

        delay(1);
    }

    tempFile.close();
    http.end();

    if (writeFailed || downloadStalled || downloaded == 0 || (totalSize >= 0 && downloaded != (uint32_t)totalSize))
    {
        LittleFS.remove(tempPath);
        ShowStatus(options.display, options.ui.httpGetFail, options.verbose);
        return false;
    }

    if (!remoteMd5.isEmpty())
    {
        String downloadedMd5 = ComputeFileMd5(tempPath);
        if (!downloadedMd5.equalsIgnoreCase(remoteMd5))
        {
            LittleFS.remove(tempPath);
            ShowStatus(options.display, options.ui.md5VerifyFail, true);
            return false;
        }
    }

    if (!ReplaceFileAtomically(tempPath, localPath))
    {
        LittleFS.remove(tempPath);
        return false;
    }

    if (options.display)
    {
        options.display->progressBar(14, 50, 100, 8, 100);
        options.display->setCursor(0, 20);
        options.display->println(options.ui.success);
        options.display->display();
    }

    return true;
}

bool RemoteFileSync::SyncAny(const RemoteFileSource *sources,
                            size_t sourceCount,
                            const String &remoteFilename,
                            const String &localPath,
                            const RemoteFileSyncOptions &options,
                            int *usedSourceIndex)
{
    if (usedSourceIndex)
    {
        *usedSourceIndex = -1;
    }

    if (sources == nullptr || sourceCount == 0)
    {
        return false;
    }

    RemoteFileSourceSelection orderedSelections[kMaxSelectableSourceCount];
    const size_t cappedSourceCount = sourceCount > kMaxSelectableSourceCount ? kMaxSelectableSourceCount : sourceCount;
    const size_t orderedCount = SelectSourcesByLatency(sources,
                                                       cappedSourceCount,
                                                       remoteFilename,
                                                       orderedSelections,
                                                       kMaxSelectableSourceCount);

    for (size_t i = 0; i < orderedCount; ++i)
    {
        const RemoteFileSource &source = *orderedSelections[i].source;
        if (Sync(source, remoteFilename, localPath, options))
        {
            if (usedSourceIndex)
            {
                *usedSourceIndex = orderedSelections[i].sourceIndex;
            }
            return true;
        }

        if (i + 1 < orderedCount)
        {
            Serial.printf("[WARN] %s sync failed for %s; trying next remote\n",
                          SourceName(source),
                          remoteFilename.c_str());
        }
    }

    return false;
}