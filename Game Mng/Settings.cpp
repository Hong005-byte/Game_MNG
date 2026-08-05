#include "Settings.h"
#include <filesystem>
#include <fstream>
#include <unordered_map>
#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

sf::Keyboard::Key* KeyBindings::find(RebindAction action) {
    switch (action) {
        case RebindAction::MoveUp:      return &moveUp;
        case RebindAction::MoveDown:    return &moveDown;
        case RebindAction::MoveLeft:    return &moveLeft;
        case RebindAction::MoveRight:   return &moveRight;
        case RebindAction::Interact:    return &interact;
        case RebindAction::QuickUpgrade:return &quickUpgrade;
        case RebindAction::Minimap:     return &minimap;
        case RebindAction::Minigame:    return &minigame;
        default:                        return nullptr;
    }
}

std::string SettingsManager::settingsFilePath() {
    fs::path dir = fs::current_path();
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        dir = fs::path(buf).parent_path(); // same "always exact" reasoning as SaveManager::savesDirectory()
    }
#endif
    dir /= "settings.cfg";
    return dir.string(); // ASCII-only path/filename -- no UTF-8 round-trip concern like save names have
}

Settings SettingsManager::load() {
    Settings settings; // defaults if the file doesn't exist yet or a key is missing
    std::ifstream in(settingsFilePath());
    if (!in) return settings;

    std::unordered_map<std::string, std::string> kv;
    std::string line;
    while (std::getline(in, line)) {
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        kv[line.substr(0, pos)] = line.substr(pos + 1);
    }

    auto getF = [&](const std::string& k, float def) {
        auto it = kv.find(k);
        try { return it != kv.end() ? std::stof(it->second) : def; } catch (...) { return def; }
    };
    auto getI = [&](const std::string& k, int def) {
        auto it = kv.find(k);
        try { return it != kv.end() ? std::stoi(it->second) : def; } catch (...) { return def; }
    };
    auto getKey = [&](const std::string& k, sf::Keyboard::Key def) {
        auto it = kv.find(k);
        if (it == kv.end()) return def;
        try { return static_cast<sf::Keyboard::Key>(std::stoi(it->second)); } catch (...) { return def; }
    };

    settings.sfxVolumePercent = getF("sfxVolumePercent", settings.sfxVolumePercent);
    settings.musicVolumePercent = getF("musicVolumePercent", settings.musicVolumePercent);
    settings.resolutionIndex = getI("resolutionIndex", settings.resolutionIndex);
    settings.fullscreen = getI("fullscreen", settings.fullscreen ? 1 : 0) != 0;
    settings.keys.moveUp = getKey("key.moveUp", settings.keys.moveUp);
    settings.keys.moveDown = getKey("key.moveDown", settings.keys.moveDown);
    settings.keys.moveLeft = getKey("key.moveLeft", settings.keys.moveLeft);
    settings.keys.moveRight = getKey("key.moveRight", settings.keys.moveRight);
    settings.keys.interact = getKey("key.interact", settings.keys.interact);
    settings.keys.quickUpgrade = getKey("key.quickUpgrade", settings.keys.quickUpgrade);
    settings.keys.minimap = getKey("key.minimap", settings.keys.minimap);
    settings.keys.minigame = getKey("key.minigame", settings.keys.minigame);
    return settings;
}

void SettingsManager::save(const Settings& settings) {
    std::ofstream out(settingsFilePath());
    if (!out) return;
    out << "sfxVolumePercent=" << settings.sfxVolumePercent << "\n";
    out << "musicVolumePercent=" << settings.musicVolumePercent << "\n";
    out << "resolutionIndex=" << settings.resolutionIndex << "\n";
    out << "fullscreen=" << (settings.fullscreen ? 1 : 0) << "\n";
    out << "key.moveUp=" << static_cast<int>(settings.keys.moveUp) << "\n";
    out << "key.moveDown=" << static_cast<int>(settings.keys.moveDown) << "\n";
    out << "key.moveLeft=" << static_cast<int>(settings.keys.moveLeft) << "\n";
    out << "key.moveRight=" << static_cast<int>(settings.keys.moveRight) << "\n";
    out << "key.interact=" << static_cast<int>(settings.keys.interact) << "\n";
    out << "key.quickUpgrade=" << static_cast<int>(settings.keys.quickUpgrade) << "\n";
    out << "key.minimap=" << static_cast<int>(settings.keys.minimap) << "\n";
    out << "key.minigame=" << static_cast<int>(settings.keys.minigame) << "\n";
}
