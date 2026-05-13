// FaceModelUpdater.cpp
// Downloads and verifies the face model JSON (with MD5) over Wi-Fi, showing progress on M5UnitGLASS2.
#include "FaceModelUpdater.h"

#include "RemoteFileSync.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>

#ifdef LANG_CN
#define TXT(en, cn) cn
#else
#define TXT(en, cn) en
#endif

namespace
{
    RemoteFileSource BuildSource(const FaceUpdateConfig &config)
    {
        RemoteFileSource source;
        source.baseUrl = config.base_url;
        source.token = config.auth_token;
        source.authScheme = config.auth_scheme;
        source.acceptHeader = config.accept_header;
        return source;
    }

    RemoteFileSyncOptions BuildFaceSyncOptions(M5UnitGLASS2 &display, bool verbose)
    {
        RemoteFileSyncOptions options;
        options.display = &display;
        options.verbose = verbose;
        options.keepExistingWhenRemoteMd5Unavailable = true;
        options.ui.checkingMd5 = TXT("Checking MD5...", "校验MD5...");
        options.ui.upToDate = TXT("Face up to date", "面部数据已最新");
        options.ui.md5Mismatch = TXT("MD5 mismatch, redownloading...", "MD5不一致，重新下载");
        options.ui.downloading = verbose ? TXT("Downloading face model...", "正在下载面部模型...") : TXT("Downloading face...", "正在下载面部数据...");
        options.ui.success = TXT("Download OK", "下载完成");
        options.ui.httpBeginFail = TXT("HTTP begin fail", "HTTP初始化失败");
        options.ui.httpGetFail = TXT("HTTP GET fail", "HTTP请求失败");
        options.ui.md5VerifyFail = TXT("Checksum mismatch", "校验不匹配");
        options.ui.keepLocal = TXT("Keep local face", "保留本地面部数据");
        return options;
    }

    bool EnsureFaceCandidate(const FaceUpdateConfig &config,
                             const String &remoteFilename,
                             M5UnitGLASS2 &display,
                             bool verbose)
    {
        RemoteFileSyncOptions options = BuildFaceSyncOptions(display, verbose);
        return RemoteFileSync::Sync(BuildSource(config), remoteFilename, "/" + remoteFilename, options);
    }
}

bool EnsureFaceModelJson(const FaceUpdateConfig &config, const String &deviceId, M5UnitGLASS2 &display, bool verbose)
{
    WiFi.mode(WIFI_AP_STA);
    WiFi.disconnect(true, true);
    WiFi.begin(config.download_ssid, config.download_password);

    const unsigned long connectTimeoutMs = 15000;
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < connectTimeoutMs)
    {
        delay(500);
        Serial.print('.');
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[WARN] WiFi connect failed; face sync will use local fallback if available");
        if (verbose)
        {
            display.println(TXT("WiFi failed", "WiFi连接失败"));
            display.display();
        }
    }

    if (!RemoteFileSync::EnsureFsMounted())
    {
        Serial.println("[WARN] LittleFS mount failed; cannot verify face model JSON");
        if (verbose)
        {
            display.println(TXT("FS mount failed", "文件系统挂载失败"));
            display.display();
        }
        return false;
    }

    const String deviceFaceFilename = deviceId.length() > 0 ? deviceId + String("_face.json") : String();
    if (!deviceFaceFilename.isEmpty())
    {
        Serial.printf("[INFO] Trying device face model %s\n", deviceFaceFilename.c_str());
        if (EnsureFaceCandidate(config, deviceFaceFilename, display, verbose))
        {
            return true;
        }

        Serial.printf("[WARN] Device face %s unavailable; falling back to universal_face.json\n", deviceFaceFilename.c_str());
    }

    return EnsureFaceCandidate(config, "universal_face.json", display, verbose);
}
