#pragma once

// Single source of truth for the game's version, shown in the window title
// and the console startup banner (see Main.cpp) -- also what UpdateChecker
// compares against the latest GitHub Release tag. Bump this on every release
// (and tag the matching GitHub Release the same way, e.g. "v1.0.1").
namespace Version {
    constexpr const char* kGameVersion = "1.5.1";

    // Where UpdateChecker looks for the latest release (GitHub Releases API:
    // https://api.github.com/repos/{owner}/{repo}/releases/latest). Fill
    // these in once the project has a real GitHub repo -- until then (or if
    // they're wrong/private/have no releases yet) the check just fails
    // silently, same as having no internet connection.
    constexpr const char* kUpdateRepoOwner = "Hong005-byte";
    constexpr const char* kUpdateRepoName = "Game_MNG";
}
