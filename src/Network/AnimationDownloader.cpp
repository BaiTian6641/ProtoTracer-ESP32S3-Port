#include "AnimationDownloader.h"

#include <LittleFS.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <M5UnitGLASS2.h>
#include <MD5Builder.h>

namespace
{
    bool EnsureFs()
    {
        static bool mounted = false;
        if (mounted)
            return true;
        if (LittleFS.begin(false) || LittleFS.begin(true))
        {
            mounted = true;
        }
        return mounted;
    }

    String ComputeFileMd5(const String &path)
    {
        File f = LittleFS.open(path, "r");
        if (!f)
        {
            return String();
        }

        MD5Builder md5;
        md5.begin();
        uint8_t buffer[512];
        while (f.available())
        {
            size_t n = f.read(buffer, sizeof(buffer));
            if (n > 0)
            {
                md5.add(buffer, n);
            }
        }
        f.close();
        md5.calculate();
        return md5.toString();
    }

    String DeriveMd5Filename(const String &jsonFilename)
    {
        if (jsonFilename.endsWith(".json"))
        {
            return jsonFilename.substring(0, jsonFilename.length() - 5) + ".md5";
        }
        return jsonFilename + ".md5";
    }

    String FetchRemoteMd5(const char *baseUrl, const char *token, const String &jsonFilename)
    {
        if (baseUrl == nullptr || baseUrl[0] == '\0')
        {
            return String();
        }

        String md5Name = DeriveMd5Filename(jsonFilename);
        String url = baseUrl;
        if (!url.endsWith("/"))
        {
            url += '/';
        }
        url += md5Name;

        HTTPClient http;
        if (!http.begin(url))
        {
            return String();
        }

        if (token != nullptr && token[0] != '\0')
        {
            if (url.indexOf("gitee") >= 0)
            {
                http.addHeader("Authorization", String("Bearer ") + token);
                http.addHeader("Accept", "application/vnd.github.v3.raw");
            }
            else
            {
                http.addHeader("Authorization", String("token ") + token);
            }
        }

        int code = http.GET();
        if (code != HTTP_CODE_OK)
        {
            http.end();
            return String();
        }

        String body = http.getString();
        body.trim();
        http.end();
        return body;
    }
}

bool AnimationDownloader::DownloadFrom(const char *baseUrl, const char *token, const String &filename, M5UnitGLASS2 *display, bool verbose)
{
    if (baseUrl == nullptr || baseUrl[0] == '\0')
    {
        return false;
    }

    if (!EnsureFs())
    {
        Serial.println("[WARN] LittleFS mount failed for animation download.");
        return false;
    }

    String url = baseUrl;
    if (!url.endsWith("/"))
    {
        url += '/';
    }
    url += filename;

    String localPath = "/" + filename;
    String localMd5;
    if (LittleFS.exists(localPath))
    {
        localMd5 = ComputeFileMd5(localPath);
    }

    String remoteMd5 = FetchRemoteMd5(baseUrl, token, filename);

    if (!remoteMd5.isEmpty() && !localMd5.isEmpty())
    {
        if (display)
        {
            display->clearDisplay();
            display->setCursor(0, 0);
            display->println("Checking MD5...");
            display->display();
            delay(100);
        }

        if (remoteMd5.equalsIgnoreCase(localMd5))
        {
            if (display && verbose)
            {
                display->clearDisplay();
                display->setCursor(0, 0);
                display->println("Animation up-to-date");
                display->display();
                delay(100);
            }
            return true; // Checksums match; skip download.
        }
        else if (display)
        {
            display->clearDisplay();
            display->setCursor(0, 0);
            display->println("MD5 mismatch, redownloading...");
            display->display();
            delay(100);
        }
    }

    HTTPClient http;
    if (!http.begin(url))
    {
        if (display && verbose)
        {
            display->clearDisplay();
            display->setCursor(0, 0);
            display->println("HTTP begin failed");
            display->display();
        }
        return false;
    }

    if (token != nullptr && token[0] != '\0')
    {
        if (url.indexOf("gitee") >= 0)
        {
            http.addHeader("Authorization", String("Bearer ") + token);
            http.addHeader("Accept", "application/vnd.github.v3.raw");
        }
        else
        {
            http.addHeader("Authorization", String("token ") + token);
        }
    }

    int code = http.GET();
    if (code != HTTP_CODE_OK)
    {
        http.end();
        if (display && verbose)
        {
            display->clearDisplay();
            display->setCursor(0, 0);
            display->println("HTTP GET failed");
            display->display();
        }
        return false;
    }

    if (display)
    {
        display->clearDisplay();
        display->setCursor(0, 0);
        display->println(verbose ? "Downloading animation..." : "Downloading anim...");
        display->display();
    }

    String path = "/" + filename;
    File f = LittleFS.open(path, "w");
    if (!f)
    {
        http.end();
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    uint8_t buffer[512];
    int32_t remaining = http.getSize();
    int32_t totalSize = remaining; // -1 when unknown
    uint32_t downloaded = 0;
    int lastPct = -1;
    unsigned long lastUi = 0;

    while (http.connected() && (remaining > 0 || remaining == -1))
    {
        size_t available = stream->available();
        if (available)
        {
            int toRead = available;
            if (toRead > (int)sizeof(buffer))
                toRead = sizeof(buffer);

            int readCount = stream->readBytes(buffer, toRead);
            if (readCount > 0)
            {
                f.write(buffer, readCount);
                downloaded += readCount;
                if (remaining > 0)
                {
                    remaining -= readCount;
                }

                if (display && totalSize > 0)
                {
                    int pct = (int)((downloaded * 100) / totalSize);
                    unsigned long now = millis();
                    if (pct != lastPct || now - lastUi > 200)
                    {
                        lastPct = pct;
                        lastUi = now;
                        display->progressBar(14, 50, 100, 8, pct);
                        display->display();
                    }
                }
            }
        }
        delay(1);
    }

    f.close();
    http.end();
    Serial.printf("[INFO] Downloaded animation JSON from %s\n", url.c_str());

    // Verify download when remote checksum is available.
    if (!remoteMd5.isEmpty())
    {
        String downloadedMd5 = ComputeFileMd5(path);
        if (!downloadedMd5.equalsIgnoreCase(remoteMd5))
        {
            Serial.println("[WARN] Downloaded animation checksum mismatch; keeping file but reporting failure.");
            return false;
        }
    }

    if (display)
    {
        display->progressBar(14, 50, 100, 8, 100);
        display->setCursor(0, 20);
        display->println("Animation OK");
        display->display();
    }
    return true;
}

bool AnimationDownloader::Download(const AnimationDownloadConfig &cfg, const String &filename)
{
    if (DownloadFrom(cfg.githubBase, cfg.githubToken, filename, cfg.progressDisplay, cfg.verbose))
    {
        return true;
    }
    if (DownloadFrom(cfg.giteeBase, cfg.giteeToken, filename, cfg.progressDisplay, cfg.verbose))
    {
        return true;
    }
    return false;
}
