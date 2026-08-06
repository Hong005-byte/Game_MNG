#pragma once
#include <string>
#include <vector>
#include <functional>

// Snapshot of the stats achievements can be checked against, built fresh by
// Game each time it wants to re-evaluate progress.
struct GameStats {
    double money = 0.0;
    int totalBusinessLevels = 0;
    int distinctBusinessTypesOwned = 0;
    double totalTradeRevenue = 0.0; // cumulative $ earned from selling on the market
    int staffLevel = 0;
    double ageYears = 0.0;
    int totalBusinessTypesAvailable = 0; // size of the whole production tree
    int highestTierOwned = 0;            // deepest tier (1/2/3) with any level built
    int tier2BusinessesOwned = 0;        // how many tier-2 processors have been built
    bool anyBusinessFullyStaffed = false; // true once any single business has hired its max workers
    bool farmInSeason = false;         // true if the Farm's active crop's favorite season is current
    int cropChangeCount = 0;           // this life's successful crop switches
    int minigameHitCount = 0;          // this life's successful minigame hits (fishing/mining/chopping)
    bool anyHarborBusinessOwned = false;
    bool anyHighlandsBusinessOwned = false;
    bool allSeasonsWitnessed = false;  // true once Spring/Summer/Autumn/Winter have each occurred at least once this life
    bool allCropsWitnessedInSeason = false; // true once every CropType has grown during its favorite season at least once this life

    // ---- Construction system (see Business::constructionDaysRemaining /
    // BusinessManager::requiresConstruction) and Port -> Fisher's Isle. ----
    bool anyConstructionCompleted = false; // at least one non-free-starter business finished construction
    int constructionsCompletedCount = 0;   // how many non-free-starter businesses are currently built
    bool portBuilt = false;
    bool hasIslandShip = false;
    bool hasVisitedIsland = false;
    bool allIslandBusinessesOwned = false;    // all of Fisher's Isle's businesses built
    bool allMarketRowBusinessesOwned = false; // all of Market Row's businesses built
    int distinctGoodsInStock = 0;             // how many different market goods currently have stock > 0
};

struct Achievement {
    std::string id;
    std::string name;
    std::string description;
    double cashReward;
    bool unlocked = false;
    std::function<bool(const GameStats&)> condition;
};

class AchievementManager {
public:
    AchievementManager();

    std::vector<Achievement>& achievements() { return achievements_; }
    const std::vector<Achievement>& achievements() const { return achievements_; }

    // Checks all locked achievements against `stats`; unlocks any newly met,
    // applies their cash reward to `money`, and returns the newly unlocked ones.
    std::vector<const Achievement*> checkAndUnlock(const GameStats& stats, double& money);

private:
    std::vector<Achievement> achievements_;
};
