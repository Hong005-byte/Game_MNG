#include "UpdateChecker.h"
#include "Version.h"
#include <thread>
#include <mutex>
#include <optional>
#include <atomic>
#include <sstream>
#include <vector>
#include <array>
#include <chrono>
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")
#endif

namespace {
    std::once_flag g_startFlag;
    std::mutex g_mutex;
    std::optional<UpdateChecker::Result> g_result; // set once the background thread finishes
    std::atomic<bool> g_consumed{ false };          // pollResult only ever returns true once
    std::atomic<UpdateChecker::DownloadState> g_downloadState{ UpdateChecker::DownloadState::Idle };

#ifdef _WIN32
    // Minimal synchronous HTTPS GET against a fixed host -- good enough for
    // one small JSON response or a several-MB binary download, not meant as
    // a general-purpose client. Follows redirects transparently (WinHTTP's
    // own default policy -- WINHTTP_OPTION_REDIRECT_POLICY defaults to
    // "allow, except HTTPS -> HTTP downgrades", which covers GitHub's own
    // release-asset redirect from api.github.com/github.com over to
    // *.githubusercontent.com, both HTTPS), no extra code needed for that.
    // Times out quickly (this all runs on a background thread anyway, but a
    // game shouldn't leave a thread hanging forever if GitHub never responds).
    bool httpsGet(const std::wstring& host, const std::wstring& path, std::string& outBody, int timeoutMs = 4000) {
        HINTERNET hSession = WinHttpOpen(L"TycoonIdleUpdateChecker/1.0",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return false;
        WinHttpSetTimeouts(hSession, timeoutMs, timeoutMs, timeoutMs, timeoutMs); // resolve/connect/send/receive, ms

        HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

        // GitHub's API rejects requests with no User-Agent and expects this Accept header.
        // (Harmless -- if ignored by a non-API host like objects.githubusercontent.com --
        // to send the same header on every hop of a redirect chain too.)
        const wchar_t* headers = L"Accept: application/vnd.github+json\r\n";
        bool ok = false;
        if (WinHttpSendRequest(hRequest, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            && WinHttpReceiveResponse(hRequest, nullptr)) {
            DWORD statusCode = 0, statusSize = sizeof(statusCode);
            WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
            if (statusCode == 200) {
                std::string body;
                DWORD avail = 0;
                do {
                    avail = 0;
                    if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;
                    std::vector<char> buf(avail);
                    DWORD read = 0;
                    if (!WinHttpReadData(hRequest, buf.data(), avail, &read)) break;
                    body.append(buf.data(), read);
                } while (avail > 0);
                outBody = std::move(body);
                ok = true;
            }
        }
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return ok;
    }

    // Splits "https://host/path?query" into host + "/path?query" for
    // httpsGet above -- every URL this file ever fetches is HTTPS (GitHub
    // never serves plain HTTP), so there's no scheme to branch on.
    bool splitUrl(const std::string& url, std::wstring& outHost, std::wstring& outPath) {
        const std::string prefix = "https://";
        if (url.compare(0, prefix.size(), prefix) != 0) return false;
        size_t hostStart = prefix.size();
        size_t pathStart = url.find('/', hostStart);
        std::string hostA = (pathStart == std::string::npos) ? url.substr(hostStart) : url.substr(hostStart, pathStart - hostStart);
        std::string pathA = (pathStart == std::string::npos) ? "/" : url.substr(pathStart);
        outHost.assign(hostA.begin(), hostA.end()); // host/path are always plain ASCII for GitHub's own URLs
        outPath.assign(pathA.begin(), pathA.end());
        return true;
    }

    // Pulls `"key":"value"` out of a small, well-formed JSON blob -- not a
    // real parser, just enough for the string fields this needs (tag_name/
    // html_url/name/browser_download_url), which never contain escaped
    // quotes in practice.
    std::string extractJsonString(const std::string& json, const std::string& key, size_t from = 0) {
        std::string pattern = "\"" + key + "\":\"";
        size_t pos = json.find(pattern, from);
        if (pos == std::string::npos) return "";
        pos += pattern.size();
        size_t end = json.find('"', pos);
        if (end == std::string::npos) return "";
        return json.substr(pos, end - pos);
    }

    // Finds the Windows installer asset among a release's `"assets":[...]`
    // array -- searches for an asset `"name"` starting with
    // "TycoonIdle-Setup-" and reads that SAME asset object's own
    // "browser_download_url" (which GitHub's API always places after
    // "name" within one asset object, so searching forward from the name
    // match for the next browser_download_url key lands in the right
    // object rather than a later asset's). Returns "" if this release
    // predates 2026-08-12 and only ever published the portable zip.
    std::string findInstallerAssetUrl(const std::string& json) {
        const std::string namePrefix = "\"name\":\"TycoonIdle-Setup-";
        size_t pos = json.find(namePrefix);
        if (pos == std::string::npos) return "";
        return extractJsonString(json, "browser_download_url", pos);
    }
#endif

    // Parses "1.2.3" (or "v1.2.3") into up to 3 numeric parts; missing parts
    // default to 0 so "1.2" still compares sanely against "1.2.0".
    std::array<int, 3> parseVersion(std::string v) {
        if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
        std::array<int, 3> parts{ 0, 0, 0 };
        std::stringstream ss(v);
        std::string part;
        for (int i = 0; i < 3 && std::getline(ss, part, '.'); ++i) {
            try { parts[static_cast<size_t>(i)] = std::stoi(part); } catch (...) { parts[static_cast<size_t>(i)] = 0; }
        }
        return parts;
    }

    bool isNewerVersion(const std::string& latest, const std::string& current) {
        return parseVersion(latest) > parseVersion(current);
    }

    void runCheck() {
        UpdateChecker::Result result;
#ifdef _WIN32
        std::wstring path = L"/repos/";
        auto widen = [](const std::string& s) { return std::wstring(s.begin(), s.end()); }; // repo owner/name are always plain ASCII
        // 2026-08-12: was "/releases/latest" -- confirmed (via a live
        // standalone probe against the real repo, not a guess) that this
        // was quietly 404ing this whole time and every past release just
        // never got detected. GitHub's "latest" endpoint explicitly
        // excludes prereleases, and every release this project has ever
        // published (see the "release a new version" workflow) is created
        // with `gh release create --prerelease`. "/releases" (the full
        // list, newest first, prereleases included) is what actually
        // matches how this repo's releases really get published --
        // extractJsonString/findInstallerAssetUrl below already just take
        // the FIRST match of each key in the raw JSON text regardless of
        // whether the response is one object or an array of them, so
        // nothing else needed to change for this to keep working the same
        // way, just pointed at an endpoint that isn't always empty.
        path += widen(Version::kUpdateRepoOwner) + L"/" + widen(Version::kUpdateRepoName) + L"/releases";

        std::string body;
        if (httpsGet(L"api.github.com", path, body)) {
            std::string tag = extractJsonString(body, "tag_name");
            std::string url = extractJsonString(body, "html_url");
            if (!tag.empty() && isNewerVersion(tag, Version::kGameVersion)) {
                result.updateAvailable = true;
                result.latestVersion = (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) ? tag.substr(1) : tag;
                result.releaseUrl = url;
                result.installerUrl = findInstallerAssetUrl(body);
            }
        }
#endif
        std::lock_guard<std::mutex> lock(g_mutex);
        g_result = result; // set even when no update/network failed -- an "already checked, nothing found" result, not left unset
    }

#ifdef _WIN32
    // Background worker for downloadAndRunInstaller -- see its own header
    // comment for the overall flow.
    void runDownloadAndLaunch(std::string url) {
        std::wstring host, path;
        std::string body;
        if (!splitUrl(url, host, path) || !httpsGet(host, path, body, /*timeoutMs=*/20000) || body.empty()) {
            g_downloadState.store(UpdateChecker::DownloadState::Failed);
            return;
        }

        wchar_t tempDir[MAX_PATH];
        if (GetTempPathW(MAX_PATH, tempDir) == 0) {
            g_downloadState.store(UpdateChecker::DownloadState::Failed);
            return;
        }
        std::wstring destPath = std::wstring(tempDir) + L"TycoonIdle-Setup-latest.exe";

        HANDLE hFile = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) {
            g_downloadState.store(UpdateChecker::DownloadState::Failed);
            return;
        }
        DWORD written = 0;
        BOOL wroteOk = WriteFile(hFile, body.data(), static_cast<DWORD>(body.size()), &written, nullptr);
        CloseHandle(hFile);
        if (!wroteOk || written != body.size()) {
            g_downloadState.store(UpdateChecker::DownloadState::Failed);
            return;
        }

        // NOT silent -- the installer's own small wizard (Next/Install/
        // Finish) is what actually makes this safe: CloseApplications/
        // RestartApplications (see TycoonIdle.iss) needs the normal UI path
        // to visibly close this running instance and relaunch it after,
        // rather than a fully unattended silent run trying to do the same
        // thing with nothing on screen to show for it. `/DIR=` isn't
        // passed here on purpose -- Inno Setup remembers the directory a
        // previous install of this same AppId used (from its uninstall
        // registry entry) and pre-fills that automatically, which is
        // already the running copy's own folder for anyone who installed
        // via the installer in the first place; a portable-zip user gets
        // the normal default location instead, same as a first-time install.
        std::wstring args = L"/SUPPRESSMSGBOXES /NORESTART";
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"open";
        sei.lpFile = destPath.c_str();
        sei.lpParameters = args.c_str();
        sei.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&sei)) {
            g_downloadState.store(UpdateChecker::DownloadState::Failed);
            return;
        }
        if (sei.hProcess) CloseHandle(sei.hProcess);
        g_downloadState.store(UpdateChecker::DownloadState::LaunchedInstaller);
    }
#endif
}

void UpdateChecker::startCheck() {
    std::call_once(g_startFlag, []() {
        std::thread(runCheck).detach();
    });
}

bool UpdateChecker::pollResult(Result& out) {
    if (g_consumed.load()) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_result.has_value()) return false;
    out = *g_result;
    g_consumed.store(true);
    return out.updateAvailable;
}

bool UpdateChecker::waitForResult(int timeoutMs, Result& out) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_result.has_value()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (g_consumed.load()) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_result.has_value()) return false; // still not done -- skip silently rather than block indefinitely
    out = *g_result;
    g_consumed.store(true);
    return out.updateAvailable;
}

UpdateChecker::DownloadState UpdateChecker::downloadState() {
    return g_downloadState.load();
}

void UpdateChecker::downloadAndRunInstaller(const std::string& url) {
    if (g_downloadState.load() == DownloadState::Downloading) return; // already in progress
    g_downloadState.store(DownloadState::Downloading);
#ifdef _WIN32
    std::thread(runDownloadAndLaunch, url).detach();
#else
    g_downloadState.store(DownloadState::Failed);
#endif
}
