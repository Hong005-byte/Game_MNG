#include "Achievements.h"

AchievementManager::AchievementManager() {
    achievements_.push_back(Achievement{ "first_business", "First Steps", "Build your first business.", 25.0, false,
        [](const GameStats& s) { return s.totalBusinessLevels >= 1; } });
    achievements_.push_back(Achievement{ "diversified", "Diversified Portfolio", "Own every kind of business in the production tree.", 100.0, false,
        [](const GameStats& s) { return s.totalBusinessTypesAvailable > 0 && s.distinctBusinessTypesOwned >= s.totalBusinessTypesAvailable; } });
    achievements_.push_back(Achievement{ "thousandaire", "Thousandaire", "Reach $1,000 cash.", 50.0, false,
        [](const GameStats& s) { return s.money >= 1000.0; } });
    achievements_.push_back(Achievement{ "ten_thousandaire", "Ten Thousandaire", "Reach $10,000 cash.", 250.0, false,
        [](const GameStats& s) { return s.money >= 10000.0; } });
    achievements_.push_back(Achievement{ "trader", "Savvy Trader", "Earn $1,000 total from selling goods.", 75.0, false,
        [](const GameStats& s) { return s.totalTradeRevenue >= 1000.0; } });
    achievements_.push_back(Achievement{ "tycoon", "Tycoon", "Own 20 combined business levels.", 300.0, false,
        [](const GameStats& s) { return s.totalBusinessLevels >= 20; } });
    achievements_.push_back(Achievement{ "well_staffed", "Well Staffed", "Hire staff up to level 5.", 100.0, false,
        [](const GameStats& s) { return s.staffLevel >= 5; } });
    achievements_.push_back(Achievement{ "quarter_century", "Quarter Century", "Reach 25 years old.", 50.0, false,
        [](const GameStats& s) { return s.ageYears >= 25.0; } });
    achievements_.push_back(Achievement{ "half_century", "Half Century", "Reach 50 years old.", 200.0, false,
        [](const GameStats& s) { return s.ageYears >= 50.0; } });
    achievements_.push_back(Achievement{ "centenarian", "Centenarian", "Reach the maximum lifespan of 100 years.", 1000.0, false,
        [](const GameStats& s) { return s.ageYears >= 100.0; } });
    achievements_.push_back(Achievement{ "supply_chain", "Full Supply Chain", "Build all five tier-2 processing businesses.", 400.0, false,
        [](const GameStats& s) { return s.tier2BusinessesOwned >= 5; } });
    achievements_.push_back(Achievement{ "craftsman", "Craftsman", "Reach tier 3 in a production chain (Blacksmith or Carpenter).", 500.0, false,
        [](const GameStats& s) { return s.highestTierOwned >= 3; } });
    achievements_.push_back(Achievement{ "full_crew", "Full Crew", "Fully staff a single business with hired workers.", 150.0, false,
        [](const GameStats& s) { return s.anyBusinessFullyStaffed; } });
    achievements_.push_back(Achievement{ "good_timing", "Good Timing", "Have the Farm growing a crop during its favorite season.", 75.0, false,
        [](const GameStats& s) { return s.farmInSeason; } });
    achievements_.push_back(Achievement{ "crop_rotator", "Crop Rotator", "Change the Farm's crop 3 times in one life.", 100.0, false,
        [](const GameStats& s) { return s.cropChangeCount >= 3; } });
    achievements_.push_back(Achievement{ "minigame_pro", "Minigame Pro", "Win 5 minigames (fishing, mining, or chopping) in one life.", 150.0, false,
        [](const GameStats& s) { return s.minigameHitCount >= 5; } });
    achievements_.push_back(Achievement{ "harbor_pioneer", "Harbor Pioneer", "Own a business in the Harbor District.", 75.0, false,
        [](const GameStats& s) { return s.anyHarborBusinessOwned; } });
    achievements_.push_back(Achievement{ "highlands_settler", "Highlands Settler", "Own a business in the Highlands District.", 75.0, false,
        [](const GameStats& s) { return s.anyHighlandsBusinessOwned; } });
    achievements_.push_back(Achievement{ "season_cycle", "Full Cycle", "Live through Spring, Summer, Autumn, and Winter in one life.", 100.0, false,
        [](const GameStats& s) { return s.allSeasonsWitnessed; } });
    achievements_.push_back(Achievement{ "master_farmer", "Master Farmer", "Grow every crop during its favorite season, in one life.", 250.0, false,
        [](const GameStats& s) { return s.allCropsWitnessedInSeason; } });

    // Construction system + Port/Fisher's Isle.
    achievements_.push_back(Achievement{ "groundbreaking", "Groundbreaking", "Complete your first construction project.", 50.0, false,
        [](const GameStats& s) { return s.anyConstructionCompleted; } });
    achievements_.push_back(Achievement{ "master_builder", "Master Builder", "Have 5 or more constructed businesses standing at once.", 300.0, false,
        [](const GameStats& s) { return s.constructionsCompletedCount >= 5; } });
    achievements_.push_back(Achievement{ "harbormaster", "Harbormaster", "Build the Port.", 400.0, false,
        [](const GameStats& s) { return s.portBuilt; } });
    achievements_.push_back(Achievement{ "shipshape", "Shipshape", "Commission a ship at the Port.", 150.0, false,
        [](const GameStats& s) { return s.hasIslandShip; } });
    achievements_.push_back(Achievement{ "set_sail", "Set Sail", "Sail to Fisher's Isle.", 100.0, false,
        [](const GameStats& s) { return s.hasVisitedIsland; } });
    achievements_.push_back(Achievement{ "island_explorer", "Island Explorer", "Own every business on Fisher's Isle.", 350.0, false,
        [](const GameStats& s) { return s.allIslandBusinessesOwned; } });
    achievements_.push_back(Achievement{ "market_row_regular", "Market Row Regular", "Own every stall in Market Row.", 250.0, false,
        [](const GameStats& s) { return s.allMarketRowBusinessesOwned; } });
    achievements_.push_back(Achievement{ "full_stock", "Full Stock", "Hold 10 or more different goods in the warehouse at once.", 150.0, false,
        [](const GameStats& s) { return s.distinctGoodsInStock >= 10; } });
}

std::vector<const Achievement*> AchievementManager::checkAndUnlock(const GameStats& stats, double& money) {
    std::vector<const Achievement*> newly;
    for (auto& a : achievements_) {
        if (a.unlocked) continue;
        if (a.condition(stats)) {
            a.unlocked = true;
            money += a.cashReward;
            newly.push_back(&a);
        }
    }
    return newly;
}
