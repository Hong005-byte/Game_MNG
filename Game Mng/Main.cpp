#include "Game.h"
#include "GameWorld.h"
#include "Localization.h"
#include "SaveManager.h"
#include "Format.h"
#include "Version.h"
#include "UpdateChecker.h"
#include <iostream>
#include <limits>
#include <ctime>
#include <iomanip>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
    std::string formatLastPlayed(long long epochSeconds) {
        std::time_t t = static_cast<std::time_t>(epochSeconds);
        std::tm tmBuf{};
#ifdef _WIN32
        localtime_s(&tmBuf, &t);
#else
        localtime_r(&t, &tmBuf);
#endif
        std::ostringstream oss;
        oss << std::put_time(&tmBuf, "%Y-%m-%d %H:%M");
        return oss.str();
    }

    // "Spring, 23yo" / "春季, 23岁" -- lets the save-slot picker show what
    // stage of life/year each character is at without loading the whole save.
    std::string seasonAndAgeLabel(double ageDays) {
        int dayInCycle = static_cast<int>(ageDays) % (4 * Game::kGameDaysPerSeason);
        Season season = static_cast<Season>(dayInCycle / Game::kGameDaysPerSeason);
        const char* key = "season_spring";
        switch (season) {
        case Season::Spring: key = "season_spring"; break;
        case Season::Summer: key = "season_summer"; break;
        case Season::Autumn: key = "season_autumn"; break;
        default:             key = "season_winter"; break;
        }
        int ageYears = static_cast<int>(ageDays / Life::kDaysPerYear);
        std::ostringstream oss;
        oss << Localization::t(key) << ", " << ageYears << Localization::t("save_slot_age_suffix");
        return oss.str();
    }

    struct SaveSelection {
        std::string path;
        bool isNew = false;
        int difficulty = 1; // only meaningful when isNew
    };

    // Lets the player continue an existing save, delete one or more they no
    // longer want (see the "D<number>" handling below), or start a fresh
    // one -- and returns the file path Game should load/save from (plus,
    // for a new save, the difficulty they picked). Runs after language
    // selection (so its own prompts are already localized) and before mode
    // selection, since either mode just needs a save path to start from.
    SaveSelection chooseSaveSlot() {
        std::vector<SaveSlotInfo> slots = SaveManager::listSlots();

        // Loops so deleting a slot (or mistyping) redisplays the list
        // instead of forcing a restart -- only the final "load this one" or
        // "fall through to new game" path breaks out.
        for (;;) {
            std::cout << "\n" << Localization::t("save_select_title") << "\n";
            std::cout << "1) " << Localization::t("save_new_game") << "\n";
            for (size_t i = 0; i < slots.size(); ++i) {
                const SaveSlotInfo& s = slots[i];
                std::cout << (i + 2) << ") " << s.displayName
                    << "  (" << Localization::t("save_slot_gen") << s.generation
                    << ", $" << formatNumber(s.money)
                    << ", " << seasonAndAgeLabel(s.ageDays)
                    << ", " << Localization::t("save_slot_last_played") << formatLastPlayed(s.lastTickEpoch)
                    << ")\n";
            }
            if (!slots.empty()) std::cout << Localization::t("save_delete_hint") << "\n";
            std::cout << Localization::t("save_slot_prompt");

            std::string line;
            if (!std::getline(std::cin, line)) break; // input closed -- fall through to new game, same as before

            size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos) break; // blank line -> new game, same as the old "!(cin >> choice)" fallback

            if (line[start] == 'd' || line[start] == 'D') {
                int delChoice = 0;
                try { delChoice = std::stoi(line.substr(start + 1)); } catch (...) { delChoice = 0; }
                size_t delIndex = static_cast<size_t>(delChoice - 2);
                if (delChoice >= 2 && delIndex < slots.size()) {
                    const SaveSlotInfo& s = slots[delIndex];
                    std::cout << Localization::t("save_delete_confirm_prefix") << s.displayName << Localization::t("save_delete_confirm_suffix");
                    int confirm = 0;
                    if (!(std::cin >> confirm)) confirm = 0;
                    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                    if (confirm == 1) {
                        if (SaveManager::deleteSlot(s.path)) {
                            std::cout << Localization::t("save_deleted_prefix") << s.displayName << Localization::t("save_deleted_suffix");
                            slots.erase(slots.begin() + static_cast<std::ptrdiff_t>(delIndex));
                        } else {
                            std::cout << Localization::t("save_delete_failed");
                        }
                    }
                } else {
                    std::cout << Localization::t("invalid_choice_menu");
                }
                continue; // redisplay the (possibly shorter) list
            }

            int choice = 1;
            try { choice = std::stoi(line); } catch (...) { choice = 1; }

            size_t slotIndex = static_cast<size_t>(choice - 2);
            if (choice >= 2 && slotIndex < slots.size()) {
                const SaveSlotInfo& s = slots[slotIndex];
                std::cout << Localization::t("save_loaded_prefix") << s.displayName << Localization::t("save_loaded_suffix");
                return SaveSelection{ s.path, false, 1 };
            }
            break; // anything else (including "1") -> new game
        }

        std::cout << Localization::t("save_name_prompt");
        std::string name;
        std::getline(std::cin, name);
        std::string path = SaveManager::createSlotPath(name);
        std::cout << Localization::t("save_created_prefix") << SaveManager::displayName(path) << Localization::t("save_created_suffix");

        std::cout << Localization::t("difficulty_prompt");
        int diffChoice = 2;
        if (!(std::cin >> diffChoice)) diffChoice = 2;
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        int difficulty = (diffChoice == 1) ? 0 : (diffChoice == 3) ? 2 : 1;

        return SaveSelection{ path, true, difficulty };
    }
}

int main() {
#ifdef _WIN32
    // The rest of the program's Chinese text is compiled as UTF-8 (see the
    // project's /utf-8 flag); the console itself also needs to be told to
    // interpret/output UTF-8, or it falls back to the legacy code page and
    // prints mojibake instead of the actual characters.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    // Kicked off as early as possible so it has the whole language/save/mode
    // prompt sequence below to finish on its own background thread -- by the
    // time we're ready to check the result, it's usually already done and
    // there's nothing to wait on (see waitForResult's short capped wait).
    UpdateChecker::startCheck();

    std::cout << "Tycoon Idle v" << Version::kGameVersion << "\n\n";
    std::cout << "Select Language / 选择语言:\n1) English\n2) 中文\n> ";
    int langChoice = 1;
    if (!(std::cin >> langChoice)) langChoice = 1;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    Localization::current = (langChoice == 2) ? Language::Chinese : Language::English;

    SaveSelection saveSelection = chooseSaveSlot();

    std::cout << "\n" << Localization::t("mode_title") << "\n"
        << Localization::t("mode_1") << "\n"
        << Localization::t("mode_2") << "\n"
        << Localization::t("mode_prompt");
    int choice = 0;
    if (!(std::cin >> choice)) choice = 1;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    // By now the player has answered several prompts, so the background
    // check (started way back at the very top of main()) has almost always
    // already finished -- this rarely actually waits the full timeout.
    UpdateChecker::Result updateResult;
    if (UpdateChecker::waitForResult(1500, updateResult)) {
        std::cout << "\n" << Localization::t("update_available_prefix") << updateResult.latestVersion
            << Localization::t("update_available_mid") << updateResult.releaseUrl << Localization::t("update_available_suffix");
    }

    Game game;
    game.setSaveFile(saveSelection.path);
    if (saveSelection.isNew) game.setDifficulty(saveSelection.difficulty);
    if (choice == 2) {
        game.startSession();
        GameWorld world(game);
        world.run();
    } else {
        game.run();
    }
    return 0;
}
