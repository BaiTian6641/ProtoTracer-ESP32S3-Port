#include "AnimationDownloader.h"

#include "RemoteFileSync.h"

#include <M5UnitGLASS2.h>

bool AnimationDownloader::Download(const AnimationDownloadConfig &cfg, const String &filename)
{
    if (!RemoteFileSync::EnsureFsMounted())
    {
        Serial.println("[WARN] LittleFS mount failed for animation download.");
        return false;
    }

    RemoteFileSource sources[2];
    sources[0].baseUrl = cfg.giteeBase;
    sources[0].token = cfg.giteeToken;
    sources[0].authScheme = "Bearer ";
    sources[0].acceptHeader = "application/vnd.github.v3.raw";
    sources[0].name = "Gitee";

    sources[1].baseUrl = cfg.githubBase;
    sources[1].token = cfg.githubToken;
    sources[1].authScheme = "token ";
    sources[1].acceptHeader = nullptr;
    sources[1].name = "GitHub";

    RemoteFileSyncOptions options;
    options.display = cfg.progressDisplay;
    options.verbose = cfg.verbose;
    options.keepExistingWhenRemoteMd5Unavailable = false;
    options.ui.checkingMd5 = "Checking MD5...";
    options.ui.upToDate = "Animation up-to-date";
    options.ui.md5Mismatch = "MD5 mismatch, redownloading...";
    options.ui.downloading = cfg.verbose ? "Downloading animation..." : "Downloading anim...";
    options.ui.success = "Animation OK";
    options.ui.httpBeginFail = "HTTP begin failed";
    options.ui.httpGetFail = "HTTP GET failed";
    options.ui.md5VerifyFail = "Animation checksum failed";

    int usedSourceIndex = -1;
    if (RemoteFileSync::SyncAny(sources, 2, filename, "/" + filename, options, &usedSourceIndex))
    {
        const char *sourceName = (usedSourceIndex >= 0 && usedSourceIndex < 2) ? sources[usedSourceIndex].name : "remote";
        Serial.printf("[INFO] Animation sync succeeded via %s for %s\n", sourceName, filename.c_str());
        return true;
    }

    Serial.printf("[WARN] Animation sync failed for %s on all configured remotes\n", filename.c_str());
    return false;
}
