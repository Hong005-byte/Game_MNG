#include "SaveManager.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <unordered_map>
#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {
    constexpr const char* kSaveExtension = ".sav";

    // Every std::string in this codebase is UTF-8 (the console is switched to
    // CP_UTF8 in Main.cpp, and Localization/GameWorld both assume it too).
    // std::filesystem::path's narrow-string constructor and .string(), on the
    // other hand, assume the Windows ANSI code page -- so round-tripping a
    // Chinese save name through them directly would mangle it. Building the
    // path via std::u8string (a byte-reinterpret of the same UTF-8 data) and
    // reading it back out via .u8string() sidesteps that entirely.
    fs::path toPath(const std::string& utf8) {
        return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
    }
    std::string toUtf8(const fs::path& p) {
        std::u8string u8 = p.u8string();
        return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
    }

    // Reads just the handful of key=value lines a slot listing needs,
    // without going through Game (which owns the full load/save format and
    // pulls in the rest of the game state).
    bool readSlotMeta(const fs::path& file, SaveSlotInfo& info) {
        std::ifstream in(file);
        if (!in) return false;

        std::unordered_map<std::string, std::string> kv;
        std::string line;
        while (std::getline(in, line)) {
            auto pos = line.find('=');
            if (pos == std::string::npos) continue;
            kv[line.substr(0, pos)] = line.substr(pos + 1);
        }

        try {
            if (auto it = kv.find("money"); it != kv.end()) info.money = std::stod(it->second);
            if (auto it = kv.find("generation"); it != kv.end()) info.generation = std::stoi(it->second);
            if (auto it = kv.find("lastTick"); it != kv.end()) info.lastTickEpoch = std::stoll(it->second);
            if (auto it = kv.find("life.ageDays"); it != kv.end()) info.ageDays = std::stod(it->second);
        } catch (...) {
            // Malformed/truncated save file (e.g. interrupted write) -- still
            // list it with whatever defaults SaveSlotInfo already has rather
            // than hiding a slot the player might want to delete manually.
        }
        return true;
    }
}

std::string SaveManager::savesDirectory() {
    fs::path dir = fs::current_path();
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        dir = fs::path(buf).parent_path(); // wide string -> always exact, no encoding guesswork
    }
#endif
    dir /= "saves";
    return toUtf8(dir);
}

std::vector<SaveSlotInfo> SaveManager::listSlots() {
    std::vector<SaveSlotInfo> slots;
    fs::path dir = toPath(savesDirectory());
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return slots;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != kSaveExtension) continue;

        SaveSlotInfo info;
        info.path = toUtf8(entry.path());
        info.displayName = toUtf8(entry.path().stem());
        if (readSlotMeta(entry.path(), info)) slots.push_back(std::move(info));
    }

    std::sort(slots.begin(), slots.end(), [](const SaveSlotInfo& a, const SaveSlotInfo& b) {
        return a.lastTickEpoch > b.lastTickEpoch;
    });
    return slots;
}

std::string SaveManager::createSlotPath(const std::string& desiredName) {
    fs::path dir = toPath(savesDirectory());
    std::error_code ec;
    fs::create_directories(dir, ec);

    // Strip characters Windows filenames can't contain, plus leading/trailing
    // whitespace; fall back to a generic name if nothing usable is left.
    // Byte-wise scanning is safe here even though desiredName is UTF-8: every
    // byte of a multi-byte UTF-8 sequence is >= 0x80 and none of the
    // forbidden ASCII characters can appear as part of one.
    std::string cleaned;
    for (char c : desiredName) {
        if (std::string("<>:\"/\\|?*").find(c) != std::string::npos) continue;
        if (static_cast<unsigned char>(c) < 0x20) continue;
        cleaned += c;
    }
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    cleaned.erase(cleaned.begin(), std::find_if(cleaned.begin(), cleaned.end(), notSpace));
    cleaned.erase(std::find_if(cleaned.rbegin(), cleaned.rend(), notSpace).base(), cleaned.end());
    if (cleaned.empty()) cleaned = "Save";

    std::string candidate = cleaned;
    int suffix = 2;
    while (fs::exists(dir / toPath(candidate + kSaveExtension), ec)) {
        candidate = cleaned + " (" + std::to_string(suffix) + ")";
        suffix++;
    }

    return toUtf8(dir / toPath(candidate + kSaveExtension));
}

std::string SaveManager::displayName(const std::string& path) {
    return toUtf8(toPath(path).stem());
}

bool SaveManager::deleteSlot(const std::string& path) {
    std::error_code ec;
    return fs::remove(toPath(path), ec) && !ec;
}
