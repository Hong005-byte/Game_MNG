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
#pragma comment(lib, "winhttp.lib")
#endif

namespace {
    std::once_flag g_startFlag;
    std::mutex g_mutex;
    std::optional<UpdateChecker::Result> g_result; // set once the background thread finishes
    std::atomic<bool> g_consumed{ false };          // pollResult only ever returns true once

#ifdef _WIN32
    // Minimal synchronous HTTPS GET -- good enough for one small JSON
    // response, not meant as a general-purpose client. Times out quickly
    // (this all runs on a background thread anyway, but a game shouldn't
    // leave a thread hanging forever if GitHub never responds).
    bool httpsGet(const std::wstring& host, const std::wstring& path, std::string& outBody) {
        HINTERNET hSession = WinHttpOpen(L"TycoonIdleUpdateChecker/1.0",
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return false;
        WinHttpSetTimeouts(hSession, 3000, 3000, 4000, 4000); // resolve/connect/send/receive, ms

        HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

        // GitHub's API rejects requests with no User-Agent and expects this Accept header.
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

    // Pulls `"key":"value"` out of a small, well-formed JSON blob -- not a
    // real parser, just enough for the two string fields this needs
    // (tag_name/html_url), which never contain escaped quotes in practice.
    std::string extractJsonString(const std::string& json, const std::string& key) {
        std::string pattern = "\"" + key + "\":\"";
        size_t pos = json.find(pattern);
        if (pos == std::string::npos) return "";
        pos += pattern.size();
        size_t end = json.find('"', pos);
        if (end == std::string::npos) return "";
        return json.substr(pos, end - pos);
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
        path += widen(Version::kUpdateRepoOwner) + L"/" + widen(Version::kUpdateRepoName) + L"/releases/latest";

        std::string body;
        if (httpsGet(L"api.github.com", path, body)) {
            std::string tag = extractJsonString(body, "tag_name");
            std::string url = extractJsonString(body, "html_url");
            if (!tag.empty() && isNewerVersion(tag, Version::kGameVersion)) {
                result.updateAvailable = true;
                result.latestVersion = (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) ? tag.substr(1) : tag;
                result.releaseUrl = url;
            }
        }
#endif
        std::lock_guard<std::mutex> lock(g_mutex);
        g_result = result; // set even when no update/network failed -- an "already checked, nothing found" result, not left unset
    }
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
