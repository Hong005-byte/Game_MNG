#pragma once
#include <string>
#include <vector>

// Metadata for one save slot, read directly from the save file's own
// key=value lines -- there's no separate manifest/index file, so the slot
// list can never drift out of sync with what's actually on disk.
struct SaveSlotInfo {
    std::string path;        // full path to the .sav file
    std::string displayName; // filename without extension, as the player typed it
    int generation = 1;
    double money = 0.0;
    long long lastTickEpoch = 0; // for both "last played" display and recency sort
    double ageDays = 0.0;    // raw life.ageDays -- the save-slot picker converts this to a season/age display (see Game::currentSeason/Life::kDaysPerYear)
};

// Where save slots live and how they're discovered/created. Slots are plain
// files in a "saves" folder next to the executable (not the current working
// directory, which differs depending on whether the game is launched from
// Visual Studio, a terminal, or a shortcut, and would otherwise scatter save
// files across several unrelated locations).
namespace SaveManager {
    std::string savesDirectory();

    // All *.sav files in savesDirectory(), most-recently-played first.
    std::vector<SaveSlotInfo> listSlots();

    // Turns a player-entered name into a safe path under savesDirectory()
    // (creating the directory if needed): blank becomes a generic default,
    // and a name that collides with an existing slot gets " (2)", " (3)",
    // etc. appended rather than overwriting it. The returned path doesn't
    // exist on disk yet -- Game::save() creates it on first save.
    std::string createSlotPath(const std::string& desiredName);

    // Filename without extension, from a path returned by createSlotPath()
    // or a SaveSlotInfo::path -- for confirmation/feedback messages that
    // only have the path on hand.
    std::string displayName(const std::string& path);

    // Permanently removes the save file at `path` (a SaveSlotInfo::path from
    // listSlots()). Returns false if the file didn't exist or couldn't be
    // removed; the caller should keep showing it in that case rather than
    // assuming it's gone.
    bool deleteSlot(const std::string& path);
}
