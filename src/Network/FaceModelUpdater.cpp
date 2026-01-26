// FaceModelUpdater.cpp
// Downloads and verifies the face model JSON (with MD5) over Wi-Fi, showing progress on M5UnitGLASS2.
#include "FaceModelUpdater.h"

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <mbedtls/md5.h>
#include "Flash/Icons/Icons.h"

#ifdef LANG_CN
#define TXT(en, cn) cn
#else
#define TXT(en, cn) en
#endif

// Add optional auth/accept headers for private repo requests (e.g., Gitee).
static void ApplyOptionalHeaders(HTTPClient &http, const FaceUpdateConfig &config)
{
    if (config.auth_token && config.auth_token[0] != '\0')
    {
        http.addHeader("Authorization", String("Bearer ") + config.auth_token);
    }
    if (config.accept_header && config.accept_header[0] != '\0')
    {
        http.addHeader("Accept", config.accept_header);
    }
}

// Compute MD5 for a given LittleFS path to compare with remote checksum.
static String ComputeFileMD5(const char *path)
{
    File f = LittleFS.open(path, "r");
    if (!f)
    {
        Serial.println("[WARN] MD5: failed to open file");
        return "";
    }

    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts(&ctx);

    uint8_t buffer[512];
    while (f.available())
    {
        size_t n = f.read(buffer, sizeof(buffer));
        if (n > 0)
        {
            mbedtls_md5_update(&ctx, buffer, n);
        }
    }

    uint8_t digest[16];
    mbedtls_md5_finish(&ctx, digest);
    mbedtls_md5_free(&ctx);
    f.close();

    char hex[33];
    for (int i = 0; i < 16; ++i)
    {
        sprintf(hex + (i * 2), "%02x", digest[i]);
    }
    hex[32] = '\0';
    return String(hex);
}

// Fetch remote checksum (plain lowercase hex MD5) from gist.
static bool FetchRemoteChecksum(const FaceUpdateConfig &config, String &checksumOut)
{
    HTTPClient http;
    if (!http.begin(config.face_checksum_url))
    {
        Serial.println("[WARN] HTTP begin failed for checksum URL");
        return false;
    }

    ApplyOptionalHeaders(http, config);

    int code = http.GET();
    if (code != HTTP_CODE_OK)
    {
        Serial.printf("[WARN] HTTP GET failed (%d) for checksum URL\n", code);
        http.end();
        return false;
    }
    checksumOut = http.getString();
    checksumOut.trim();
    checksumOut.toLowerCase();
    http.end();
    return checksumOut.length() > 0;
}

bool EnsureUniversalFaceJson(const FaceUpdateConfig &config, M5UnitGLASS2 &display, bool verbose)
{
    // Connect to download WiFi first so checksum/face requests can succeed.
    WiFi.mode(WIFI_AP_STA);
    WiFi.disconnect(true, true);
    WiFi.begin(config.download_ssid, config.download_password);

    const unsigned long connect_timeout_ms = 15000;
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < connect_timeout_ms)
    {
        delay(500);
        Serial.print('.');
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[WARN] WiFi connect failed; cannot verify/download universal_face.json");
        if (verbose)
        {
            display.println(TXT("WiFi failed", "WiFi连接失败"));
            display.display();
        }
        //return false;
    }

    if (!LittleFS.begin(false) && !LittleFS.begin(true))
    {
        Serial.println("[WARN] LittleFS mount failed; cannot verify universal_face.json");
        if (verbose)
        {
            display.println(TXT("FS mount failed", "文件系统挂载失败"));
            display.display();
        }
        return false;
    }

    const char *facePath = config.face_path;
    bool faceExists = LittleFS.exists(facePath);

    String remoteChecksum;
    bool haveRemoteChecksum = FetchRemoteChecksum(config, remoteChecksum);

    if (verbose)
    {
        if (haveRemoteChecksum)
        {
            display.println(TXT("Checksum fetched", "校验获取成功"));
            display.display();
        }
        else
        {
            display.println(TXT("Checksum unavailable", "无法获取校验"));
            display.display();
        }
    }

    if (faceExists && haveRemoteChecksum)
    {
        String localMd5 = ComputeFileMD5(facePath);
        if (localMd5.length() == 32 && localMd5.equals(remoteChecksum))
        {
            Serial.println("[INFO] universal_face.json is up to date (checksum match)");
            if (verbose)
            {
                display.println(TXT("Face up to date", "面部数据已最新"));
                display.display();
            }
            return true;
        }
        Serial.println("[INFO] universal_face.json differs from remote checksum; will re-download");
        if (verbose)
        {
            display.println(TXT("Face outdated", "面部数据需要更新"));
            display.display();
        }
    }
    else if (faceExists && !haveRemoteChecksum)
    {
        Serial.println("[INFO] Face exists but checksum unavailable; keeping existing file");
        if (verbose)
        {
            display.println(TXT("Keep local face", "保留本地面部数据"));
            display.display();
        }
        return true;
    }

    Serial.println("[INFO] universal_face.json missing; attempting WiFi download...");

    HTTPClient http;
    if (!http.begin(config.face_json_url))
    {
        Serial.println("[WARN] HTTP begin failed for universal_face.json");
        if (verbose)
        {
            display.println(TXT("HTTP begin fail", "HTTP初始化失败"));
            display.display();
        }
        return false;
    }

    ApplyOptionalHeaders(http, config);

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK)
    {
        Serial.printf("[WARN] HTTP GET failed (%d) for universal_face.json\n", httpCode);
        http.end();
        if (verbose)
        {
            display.println(TXT("HTTP GET fail", "HTTP请求失败"));
            display.display();
        }
        return false;
    }

    // Prepare download progress UI on the M5UnitGLASS2 display
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println(verbose ? TXT("Downloading face model...", "正在下载面部模型...") : TXT("Downloading face...", "正在下载面部数据..."));
    display.pushImage(52, 16, 24, 24, epd_bitmap_download);
    display.display();

    File faceFile = LittleFS.open(facePath, "w");
    if (!faceFile)
    {
        Serial.println("[WARN] Failed to open /universal_face.json for writing");
        http.end();
        if (verbose)
        {
            display.println(TXT("File open fail", "文件打开失败"));
            display.display();
        }
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    uint8_t buffer[512];
    int32_t remaining = http.getSize();
    int32_t totalSize = remaining;
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
                faceFile.write(buffer, readCount);
                downloaded += readCount;
                if (remaining > 0)
                {
                    remaining -= readCount;
                }

                if (totalSize > 0)
                {
                    int pct = (int)((downloaded * 100) / totalSize);
                    unsigned long now = millis();
                    if (pct != lastPct || now - lastUi > 200)
                    {
                        lastPct = pct;
                        lastUi = now;
                        display.progressBar(14, 50, 100, 8, pct);
                        display.display();
                    }
                }
            }
        }
        delay(1);
    }

    faceFile.close();
    http.end();

    Serial.println("[INFO] Downloaded universal_face.json to LittleFS");
    if (verbose)
    {
        display.setCursor(0, 36);
        display.println(TXT("Saved to flash", "已保存到闪存"));
        display.display();
    }

    if (haveRemoteChecksum)
    {
        String localMd5 = ComputeFileMD5(facePath);
        if (!localMd5.equals(remoteChecksum))
        {
            Serial.println(localMd5);
            Serial.println("[WARN] Downloaded file checksum mismatch");
            if (verbose)
            {
                display.setCursor(0, 44);
                display.println(TXT("Checksum mismatch", "校验不匹配"));
                display.display();
            }

            // Remove the bad file and abort startup to avoid using a corrupted model.
            LittleFS.remove(facePath);
            WiFi.disconnect(true, true);
            WiFi.mode(WIFI_OFF);
            return false;
        }
    }

    // Finalize UI
    display.progressBar(14, 50, 100, 8, 100);
    display.setCursor(0, 20);
    display.println(TXT("Download OK", "下载完成"));
    if (verbose)
    {
        display.setCursor(0, 32);
        display.println(TXT("Checksum verified", "校验已验证"));
    }
    display.display();

    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);

    return true;
}
