#pragma once
#include <SFML/Window/Keyboard.hpp>
#include <string>

// Which action each rebindable key drives -- used by the Settings overlay's
// "click to rebind" flow (see GameWorld::awaitingRebind_) to know which
// KeyBindings field a freshly-pressed key should land in, and to swap two
// actions' keys instead of leaving a collision when the player picks a key
// that's already taken.
enum class RebindAction { None, MoveUp, MoveDown, MoveLeft, MoveRight, Interact, QuickUpgrade, Minimap, Minigame };

// The "common keys" set (see the plan this was built from): the four
// movement keys plus the single-key actions. Arrow keys, Shift-sprint, and
// Space (minigame confirm) are deliberately left out -- they stay fixed
// fallback/secondary bindings so the player always has a way to move and
// confirm even mid-rebind, and so the settings panel doesn't balloon.
struct KeyBindings {
    sf::Keyboard::Key moveUp = sf::Keyboard::Key::W;
    sf::Keyboard::Key moveDown = sf::Keyboard::Key::S;
    sf::Keyboard::Key moveLeft = sf::Keyboard::Key::A;
    sf::Keyboard::Key moveRight = sf::Keyboard::Key::D;
    sf::Keyboard::Key interact = sf::Keyboard::Key::E;
    sf::Keyboard::Key quickUpgrade = sf::Keyboard::Key::U;
    sf::Keyboard::Key minimap = sf::Keyboard::Key::M;
    sf::Keyboard::Key minigame = sf::Keyboard::Key::F;

    sf::Keyboard::Key* find(RebindAction action);
};

// Player-level preferences: unlike Game's own save file (one per character,
// reset on death), these apply across every save slot, so they live in their
// own small file next to the executable -- mirrors SaveManager's own
// executable-relative "saves" folder (see SettingsManager::settingsFilePath).
struct Settings {
    float sfxVolumePercent = 100.f;   // 0-100, master scale over each sound's own baseline volume
    float musicVolumePercent = 55.f;  // 0-100, master scale over the looping per-season background melody
    int resolutionIndex = 0;          // index into GameWorld's kResolutionPresets
    bool fullscreen = false;
    KeyBindings keys;
};

namespace SettingsManager {
    std::string settingsFilePath();
    Settings load();
    void save(const Settings& settings);
}
