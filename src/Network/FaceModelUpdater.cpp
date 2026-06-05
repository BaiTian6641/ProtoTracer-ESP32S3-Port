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
    constexpr size_t kMaxFaceSources = 4;

    RemoteFileSource BuildSource(const FaceUpdateConfig &config)
    {
        RemoteFileSource source;
        source.baseUrl = config.base_url;
        source.token = config.auth_token;
        source.authScheme = config.auth_scheme;
        source.acceptHeader = config.accept_header;
        source.name = config.name;
        return source;
    }

    RemoteFileSyncOptions BuildFaceSyncOptions(M5GFX *display, bool verbose)
    {
        RemoteFileSyncOptions options;
        options.display = display;
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

    bool EnsureFaceCandidate(const FaceUpdateConfig *configs,
                             size_t sourceCount,
                             const String &remoteFilename,
                             M5GFX *display,
                             bool verbose)
    {
        RemoteFileSyncOptions options = BuildFaceSyncOptions(display, verbose);
        RemoteFileSource sources[kMaxFaceSources];
        size_t configuredCount = 0;
        for (size_t i = 0; i < sourceCount && configuredCount < kMaxFaceSources; ++i)
        {
            if (configs[i].base_url == nullptr || configs[i].base_url[0] == '\0')
            {
                continue;
            }

            sources[configuredCount] = BuildSource(configs[i]);
            ++configuredCount;
        }

        return RemoteFileSync::SyncAny(sources, configuredCount, remoteFilename, "/" + remoteFilename, options);
    }
}

bool EnsureFaceModelJson(const FaceUpdateConfig &config, const String &deviceId, M5GFX *display, bool verbose)
{
    return EnsureFaceModelJson(&config, 1, deviceId, display, verbose);
}

bool EnsureFaceModelJson(const FaceUpdateConfig *configs, size_t sourceCount, const String &deviceId, M5GFX *display, bool verbose)
{
    if (configs == nullptr || sourceCount == 0)
    {
        return false;
    }

    // If already connected to the target SSID, skip the disconnect/reconnect cycle
    // to avoid tearing down an active NetWizard connection (which can cause IP loss).
    bool alreadyConnected = (WiFi.status() == WL_CONNECTED);
    if (alreadyConnected && configs[0].download_ssid != nullptr)
    {
        String currentSsid = WiFi.SSID();
        String targetSsid = String(configs[0].download_ssid);
        if (currentSsid != targetSsid)
        {
            alreadyConnected = false;
        }
    }

    if (!alreadyConnected)
    {
        WiFi.mode(WIFI_AP_STA);
        WiFi.disconnect(false, false);  // keep radio on; only drop STA association
        WiFi.begin(configs[0].download_ssid, configs[0].download_password);

        const unsigned long connectTimeoutMs = 8000;
        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < connectTimeoutMs)
        {
            delay(200);
            Serial.print('.');
        }
        Serial.println();
    }
    else
    {
        Serial.printf("[INFO] WiFi already connected to %s, skipping reconnect\n", WiFi.SSID().c_str());
    }

    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("[WARN] WiFi connect failed; face sync will use local fallback if available");
        if (verbose && display)
        {
            display->println(TXT("WiFi failed", "WiFi连接失败"));
            display->display();
        }
    }

    if (!RemoteFileSync::EnsureFsMounted())
    {
        Serial.println("[WARN] LittleFS mount failed; cannot verify face model JSON");
        if (verbose && display)
        {
            display->println(TXT("FS mount failed", "文件系统挂载失败"));
            display->display();
        }
        return false;
    }

    const String deviceFaceFilename = deviceId.length() > 0 ? deviceId + String("_face.json") : String();
    if (!deviceFaceFilename.isEmpty())
    {
        Serial.printf("[INFO] Trying device face model %s\n", deviceFaceFilename.c_str());
        if (EnsureFaceCandidate(configs, sourceCount, deviceFaceFilename, display, verbose))
        {
            return true;
        }

        Serial.printf("[WARN] Device face %s unavailable; falling back to universal_face.json\n", deviceFaceFilename.c_str());
    }

    return EnsureFaceCandidate(configs, sourceCount, "universal_face.json", display, verbose);
}
