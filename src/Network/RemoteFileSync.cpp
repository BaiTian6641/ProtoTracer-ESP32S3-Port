#include "RemoteFileSync.h"

#include <FS.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <M5UnitGLASS2.h>
#include <MD5Builder.h>
#include <WiFi.h>

namespace
{
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
        if (!http.begin(url))
        {
            return String();
        }

        ApplyHeaders(http, source);

        int code = http.GET();
        if (code != HTTP_CODE_OK)
        {
            http.end();
            return String();
        }

        String body = http.getString();
        body.trim();
        body.toLowerCase();
        http.end();
        return body;
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

    if (LittleFS.begin(false) || LittleFS.begin(true))
    {
        mounted = true;
    }

    return mounted;
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

    String remoteMd5 = FetchRemoteMd5(source, remoteFilename);
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

        delay(1);
    }

    tempFile.close();
    http.end();

    if (writeFailed || (totalSize >= 0 && downloaded != (uint32_t)totalSize))
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