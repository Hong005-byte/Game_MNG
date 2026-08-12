#pragma once
#include <string>

// Checks GitHub Releases (see Version::kUpdateRepoOwner/kUpdateRepoName) for
// a version newer than Version::kGameVersion, entirely passively: it never
// downloads or installs anything itself, just reports "here's a newer
// version and its download page" so the caller can prompt the player and,
// if they say yes, open that page in their browser. Runs the actual network
// request on a background thread (a game shouldn't hang at startup because
// the player's offline or GitHub is slow) with a short timeout, so a player
// who never launches the game never triggers a check, and one who launches
// an old copy after being offline just silently skips it.
//
// 2026-08-12 ("我不需要每次都要跑到github去重新下载了" -- stop needing to
// manually go to GitHub and re-download every time): added an actual
// one-click update path (downloadAndRunInstaller) alongside the existing
// "just open the release page" one -- see its own comment for how it
// works and why "open the browser" stays as a fallback rather than being
// replaced outright.
namespace UpdateChecker {
    struct Result {
        bool updateAvailable = false;
        std::string latestVersion; // e.g. "1.1.0" (the "v" prefix, if any, already stripped)
        std::string releaseUrl;    // the GitHub Release page to open if the player wants to update
        // Direct download URL for that release's Windows installer asset
        // (a "TycoonIdle-Setup-*.exe" file -- see installer/TycoonIdle.iss),
        // empty if this release has no such asset (every release before
        // 2026-08-12 only ever published the portable zip). Empty means
        // the UI has nothing to one-click download and should fall back to
        // "open the release page" only.
        std::string installerUrl;
    };

    // Starts the background check exactly once (later calls are no-ops) --
    // safe to call from both the console and graphical startup paths.
    void startCheck();

    // Blocking poll for the console path, which doesn't have a per-frame
    // loop to check in on: waits up to `timeoutMs` for the check to finish
    // (returns immediately once it does), then reports the same way
    // currentResult() below does.
    bool waitForResult(int timeoutMs, Result& out);

    // 2026-08-12 ("我觉得...在游戏里面的设置可以加一个检查版本更新,就不
    // 需要玩家每次登入等待有没有新版本" -- add a manual "check for
    // updates" to Settings, so players aren't only ever finding out via
    // the passive automatic check on login): startCheck() above only ever
    // runs once per process (std::call_once), which is right for "check
    // quietly at startup" but wrong for a button the player can press
    // again whenever they want -- startManualCheck() below is the same
    // background worker with that restriction lifted, guarded instead by
    // isChecking() so mashing the button can't stack up overlapping
    // requests. Both this and the automatic startCheck() feed the SAME
    // underlying result, readable via currentResult() below (a plain,
    // repeatable peek -- unlike the old one-shot pollResult this replaces,
    // reading it doesn't consume/invalidate it, so both the always-on
    // update banner and a Settings status line can read it independently
    // without racing each other over who gets to see it).
    void startManualCheck();
    bool isChecking();       // true while EITHER the automatic or a manual check is in flight
    bool hasCheckedOnce();   // true once at least one check (automatic or manual) has ever finished
    Result currentResult();  // last known result -- Result{} (updateAvailable false) if hasCheckedOnce() is still false

    // How the one-click update (see downloadAndRunInstaller) is going, so a
    // UI can show "Downloading..." and disable its own button rather than
    // let the player fire it off twice. Stays Idle if downloadAndRunInstaller
    // is never called at all (the "open in browser" path doesn't touch this).
    enum class DownloadState { Idle, Downloading, LaunchedInstaller, Failed };
    DownloadState downloadState();

    // Starts (on a background thread, returns immediately) downloading
    // `url` (Result::installerUrl) to a temp file and then launching it --
    // NOT silently: the installer's own normal wizard shows, which is what
    // actually makes this safe to fire while the game itself is still
    // running. The installer script has CloseApplications/RestartApplications
    // set (see TycoonIdle.iss), so Windows' own Restart Manager closes this
    // running instance for it right before the file-copy step (the game
    // already saves on that close, same as the player clicking the window's
    // own X button -- see GameWorld::run's Closed-event handling) and
    // relaunches it once the install finishes. No-op if a download/launch
    // is already in progress (check downloadState() first).
    void downloadAndRunInstaller(const std::string& url);
}
