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
namespace UpdateChecker {
    struct Result {
        bool updateAvailable = false;
        std::string latestVersion; // e.g. "1.1.0" (the "v" prefix, if any, already stripped)
        std::string releaseUrl;    // the GitHub Release page to open if the player wants to update
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
}
