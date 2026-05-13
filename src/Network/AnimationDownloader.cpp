#include "AnimationDownloader.h"

#include "RemoteFileSync.h"

#include <M5UnitGLASS2.h>

bool AnimationDownloader::DownloadFrom(const char *baseUrl, const char *token, const String &filename, M5UnitGLASS2 *display, bool verbose)
{
    if (!RemoteFileSync::EnsureFsMounted())
    {
        Serial.println("[WARN] LittleFS mount failed for animation download.");
        return false;
    }

    RemoteFileSource source;
    source.baseUrl = baseUrl;
    source.token = token;
    source.authScheme = (baseUrl != nullptr && String(baseUrl).indexOf("gitee") >= 0) ? "Bearer " : "token ";
    source.acceptHeader = (baseUrl != nullptr && String(baseUrl).indexOf("gitee") >= 0) ? "application/vnd.github.v3.raw" : nullptr;

    RemoteFileSyncOptions options;
    options.display = display;
    options.verbose = verbose;
    options.keepExistingWhenRemoteMd5Unavailable = false;
    options.ui.checkingMd5 = "Checking MD5...";
    options.ui.upToDate = "Animation up-to-date";
    options.ui.md5Mismatch = "MD5 mismatch, redownloading...";
    options.ui.downloading = verbose ? "Downloading animation..." : "Downloading anim...";
    options.ui.success = "Animation OK";
    options.ui.httpBeginFail = "HTTP begin failed";
    options.ui.httpGetFail = "HTTP GET failed";
    options.ui.md5VerifyFail = "Animation checksum failed";

    return RemoteFileSync::Sync(source, filename, "/" + filename, options);
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
