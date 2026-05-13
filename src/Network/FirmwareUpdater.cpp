#include "FirmwareUpdater.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <M5UnitGLASS2.h>
#include <StreamString.h>
#include <Update.h>
#include <WiFi.h>

namespace
{
    constexpr uint32_t kHttpConnectTimeoutMs = 8000;
    constexpr uint32_t kHttpRequestTimeoutMs = 15000;
    constexpr uint32_t kHttpIdleTimeoutMs = 5000;

    struct FirmwareManifest
    {
        String version;
        String file;
        String md5;
    };

    String BuildUrl(const RemoteFileSource &source, const String &filename)
    {
        if (filename.startsWith("http://") || filename.startsWith("https://"))
        {
            return filename;
        }

        String url = source.baseUrl;
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

    void ShowStatus(M5UnitGLASS2 *display, const String &message, bool shouldShow)
    {
        if (!display || !shouldShow)
        {
            return;
        }

        display->clearDisplay();
        display->setCursor(0, 0);
        display->println(message);
        display->display();
    }

    String GetUpdateErrorString()
    {
        StreamString errorText;
        Update.printError(errorText);
        return String(errorText.c_str());
    }

    int NextVersionComponent(const String &version, size_t &index)
    {
        while (index < version.length() && !isDigit(static_cast<unsigned char>(version[index])))
        {
            ++index;
        }

        if (index >= version.length())
        {
            return 0;
        }

        int value = 0;
        while (index < version.length() && isDigit(static_cast<unsigned char>(version[index])))
        {
            value = (value * 10) + (version[index] - '0');
            ++index;
        }
        return value;
    }

    int CompareVersions(const String &currentVersion, const String &remoteVersion)
    {
        size_t currentIndex = 0;
        size_t remoteIndex = 0;

        while (currentIndex < currentVersion.length() || remoteIndex < remoteVersion.length())
        {
            const int currentPart = NextVersionComponent(currentVersion, currentIndex);
            const int remotePart = NextVersionComponent(remoteVersion, remoteIndex);
            if (currentPart != remotePart)
            {
                return currentPart < remotePart ? -1 : 1;
            }
        }

        return 0;
    }

    bool FetchManifest(const RemoteFileSource &source,
                       const char *sourceName,
                       const String &manifestFilename,
                       FirmwareManifest &manifest)
    {
        HTTPClient http;
        const String url = BuildUrl(source, manifestFilename);
        http.setConnectTimeout(kHttpConnectTimeoutMs);
        http.setTimeout(kHttpRequestTimeoutMs);
        if (!http.begin(url))
        {
            Serial.printf("[WARN] Firmware manifest begin failed for %s (%s)\n", sourceName, url.c_str());
            return false;
        }

        ApplyHeaders(http, source);

        const int code = http.GET();
        if (code != HTTP_CODE_OK)
        {
            Serial.printf("[WARN] Firmware manifest fetch failed (%d) via %s for %s\n", code, sourceName, url.c_str());
            http.end();
            return false;
        }

        const String payload = http.getString();
        http.end();

        DynamicJsonDocument doc(payload.length() + 512);
        const DeserializationError error = deserializeJson(doc, payload);
        if (error)
        {
            Serial.printf("[WARN] Firmware manifest parse failed via %s: %s\n", sourceName, error.c_str());
            return false;
        }

        manifest.version = doc["version"] | doc["fwv"] | String("");
        manifest.file = doc["file"] | doc["filename"] | doc["bin"] | doc["url"] | String("");
        manifest.md5 = doc["md5"] | doc["file_md5"] | doc["hash"] | String("");

        if (manifest.version.isEmpty() || manifest.file.isEmpty())
        {
            Serial.printf("[WARN] Firmware manifest via %s is missing required fields\n", sourceName);
            return false;
        }

        Serial.printf("[INFO] Firmware manifest via %s: version=%s file=%s\n",
                      sourceName,
                      manifest.version.c_str(),
                      manifest.file.c_str());
        return true;
    }

    bool ApplyFirmware(const RemoteFileSource &source,
                       const char *sourceName,
                       const FirmwareManifest &manifest,
                       M5UnitGLASS2 *display,
                       bool verbose)
    {
        HTTPClient http;
        const String url = BuildUrl(source, manifest.file);
        http.setConnectTimeout(kHttpConnectTimeoutMs);
        http.setTimeout(kHttpRequestTimeoutMs);
        if (!http.begin(url))
        {
            Serial.printf("[WARN] Firmware download begin failed via %s (%s)\n", sourceName, url.c_str());
            return false;
        }

        ApplyHeaders(http, source);

        const int code = http.GET();
        if (code != HTTP_CODE_OK)
        {
            Serial.printf("[WARN] Firmware download failed (%d) via %s for %s\n", code, sourceName, url.c_str());
            http.end();
            ShowStatus(display, "Firmware fetch failed", verbose);
            return false;
        }

        const int32_t totalSize = http.getSize();
        if (manifest.md5.length() > 0 && !Update.setMD5(manifest.md5.c_str()))
        {
            Serial.println("[WARN] Firmware MD5 from manifest is invalid");
            http.end();
            return false;
        }

        if (!Update.begin(totalSize > 0 ? totalSize : UPDATE_SIZE_UNKNOWN, U_FLASH))
        {
            Serial.printf("[WARN] Update.begin failed: %s\n", GetUpdateErrorString().c_str());
            http.end();
            ShowStatus(display, "Update init failed", verbose);
            return false;
        }

        ShowStatus(display, String("Updating firmware\n") + manifest.version, true);

        WiFiClient *stream = http.getStreamPtr();
        uint8_t buffer[1024];
        int32_t remaining = totalSize;
        size_t writtenTotal = 0;
        uint32_t lastDataMs = millis();
        int lastPercent = -1;
        bool failed = false;

        while (http.connected() && (remaining > 0 || remaining == -1))
        {
            const size_t available = stream->available();
            if (available)
            {
                int toRead = static_cast<int>(available);
                if (toRead > static_cast<int>(sizeof(buffer)))
                {
                    toRead = sizeof(buffer);
                }

                const int readCount = stream->readBytes(buffer, toRead);
                if (readCount > 0)
                {
                    const size_t written = Update.write(buffer, readCount);
                    if (written != static_cast<size_t>(readCount))
                    {
                        Serial.printf("[WARN] Firmware write failed: %s\n", GetUpdateErrorString().c_str());
                        failed = true;
                        break;
                    }

                    writtenTotal += written;
                    lastDataMs = millis();
                    if (remaining > 0)
                    {
                        remaining -= readCount;
                    }

                    if (display && totalSize > 0)
                    {
                        const int percent = static_cast<int>((writtenTotal * 100) / totalSize);
                        if (percent != lastPercent)
                        {
                            lastPercent = percent;
                            display->progressBar(14, 50, 100, 8, percent);
                            display->display();
                        }
                    }
                }
            }
            else if (millis() - lastDataMs > kHttpIdleTimeoutMs)
            {
                Serial.printf("[WARN] Firmware download stalled via %s for %s\n", sourceName, url.c_str());
                failed = true;
                break;
            }

            delay(1);
        }

        http.end();

        if (failed || writtenTotal == 0 || (totalSize > 0 && writtenTotal != static_cast<size_t>(totalSize)))
        {
            Update.abort();
            ShowStatus(display, "Update download failed", verbose);
            return false;
        }

        if (!Update.end(true))
        {
            Serial.printf("[WARN] Update.end failed: %s\n", GetUpdateErrorString().c_str());
            Update.abort();
            ShowStatus(display, "Update finalize failed", verbose);
            return false;
        }

        if (!Update.isFinished())
        {
            Serial.println("[WARN] Update did not finish cleanly");
            ShowStatus(display, "Update incomplete", verbose);
            return false;
        }

        if (display)
        {
            display->progressBar(14, 50, 100, 8, 100);
            display->setCursor(0, 20);
            display->println("Rebooting...");
            display->display();
        }

        Serial.printf("[INFO] Firmware update applied via %s to version %s\n", sourceName, manifest.version.c_str());
        delay(200);
        ESP.restart();
        return true;
    }

    FirmwareUpdateResult TrySource(const RemoteFileSource &source,
                                   const char *sourceName,
                                   const FirmwareUpdateConfig &config)
    {
        FirmwareManifest manifest;
        if (!FetchManifest(source, sourceName, config.manifestFilename, manifest))
        {
            return FirmwareUpdateResult::Failed;
        }

        const String currentVersion = config.currentVersion != nullptr ? String(config.currentVersion) : String("0.0.0");
        if (CompareVersions(currentVersion, manifest.version) >= 0)
        {
            Serial.printf("[INFO] Firmware is current (%s); %s manifest reports %s\n",
                          currentVersion.c_str(),
                          sourceName,
                          manifest.version.c_str());
            ShowStatus(config.display, String("Firmware current\n") + currentVersion, config.verbose);
            return FirmwareUpdateResult::UpToDate;
        }

        Serial.printf("[INFO] Firmware update available via %s: %s -> %s\n",
                      sourceName,
                      currentVersion.c_str(),
                      manifest.version.c_str());

        if (!ApplyFirmware(source, sourceName, manifest, config.display, config.verbose))
        {
            return FirmwareUpdateResult::Failed;
        }

        return FirmwareUpdateResult::Updated;
    }
}

FirmwareUpdateResult FirmwareUpdater::CheckAndUpdate(const FirmwareUpdateConfig &config)
{
    if (config.manifestFilename == nullptr || config.manifestFilename[0] == '\0')
    {
        return FirmwareUpdateResult::Skipped;
    }

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[INFO] Skipping auto firmware update: WiFi not connected");
        return FirmwareUpdateResult::Skipped;
    }

    RemoteFileSource sources[2];
    sources[0] = config.primarySource;
    sources[0].name = config.primaryName;
    sources[1] = config.fallbackSource;
    sources[1].name = config.fallbackName;

    RemoteFileSourceSelection orderedSelections[2];
    const size_t orderedCount = RemoteFileSync::SelectSourcesByLatency(sources,
                                                                       2,
                                                                       String(config.manifestFilename),
                                                                       orderedSelections,
                                                                       2);
    if (orderedCount == 0)
    {
        return FirmwareUpdateResult::Skipped;
    }

    for (size_t i = 0; i < orderedCount; ++i)
    {
        const RemoteFileSource &source = *orderedSelections[i].source;
        const char *sourceName = (source.name != nullptr && source.name[0] != '\0') ? source.name : "remote";
        const FirmwareUpdateResult result = TrySource(source, sourceName, config);
        if (result != FirmwareUpdateResult::Failed)
        {
            return result;
        }

        if (i + 1 < orderedCount)
        {
            const RemoteFileSource &fallbackSource = *orderedSelections[i + 1].source;
            const char *fallbackName = (fallbackSource.name != nullptr && fallbackSource.name[0] != '\0') ? fallbackSource.name : "remote";
            Serial.printf("[WARN] %s firmware update check failed; trying %s fallback\n", sourceName, fallbackName);
        }
    }

    return FirmwareUpdateResult::Failed;
}