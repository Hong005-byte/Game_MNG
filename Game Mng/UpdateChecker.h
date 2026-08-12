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

    // Non-blocking poll for the graphical loop: returns true (and fills
    // `out`) exactly once, the first call after the background check
    // finishes -- every other call (not finished yet, or already consumed)
    // returns false. Call this once per frame; it's cheap.
    bool pollResult(Result& out);

    // Blocking poll for the console path, which doesn't have a per-frame
    // loop to check in on: waits up to `timeoutMs` for the check to finish
    // (returns immediately once it does), then behaves like pollResult.
    bool waitForResult(int timeoutMs, Result& out);

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
