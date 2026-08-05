#include "Game.h"
#include "Format.h"
#include "Localization.h"
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <chrono>
#include <algorithm>
#include <limits>
#include <filesystem>
#include <cmath>
#include <cstdlib>

namespace {
    // saveFilePath_ is UTF-8 (it may come from a player-typed Chinese save
    // name via SaveManager), but std::ifstream/ofstream's narrow-string
    // constructors assume the Windows ANSI code page. Routing through
    // std::filesystem::path via std::u8string sidesteps that -- same trick
    // SaveManager uses when it builds the path in the first place.
    std::filesystem::path toFsPath(const std::string& utf8) {
        return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
    }

    const char* seasonKey(Season s) {
        switch (s) {
        case Season::Spring: return "season_spring";
        case Season::Summer: return "season_summer";
        case Season::Autumn: return "season_autumn";
        default:             return "season_winter";
        }
    }

    // A handful of goods sell for a bit more during their in-demand season --
    // gives the Market a seasonal dimension too, not just the Farm. Flavor,
    // not a lever: same "deliberately small" spirit as kRainBonusMultiplier.
    // Applied only to selling (see Game::trySellGood), never to buying --
    // the underlying Good::price random walk is untouched either way.
    double seasonalGoodSellMultiplier(const std::string& goodId, Season season) {
        if (goodId == "fish")  return season == Season::Summer ? 1.15 : (season == Season::Winter ? 0.90 : 1.0);
        if (goodId == "wool")  return season == Season::Winter ? 1.15 : 1.0;
        if (goodId == "pelts") return season == Season::Autumn ? 1.15 : 1.0;
        if (goodId == "honey") return season == Season::Summer ? 1.15 : 1.0;
        if (goodId == "milk")  return season == Season::Spring ? 1.10 : 1.0;
        return 1.0;
    }

    // No cap on offline catch-up: aging/energy/hunger and the economy all
    // continue advancing however long the game was closed, so a character
    // genuinely keeps aging while you're away. Market and event simulation
    // already cap their own internal iteration counts, so this stays cheap
    // to compute even after a very long absence.
    constexpr long long kMarketStepSeconds = 5; // one price-noise step per 5 simulated seconds
    constexpr size_t kMaxEventLinesShown = 8;
    constexpr double kDoctorCost = 75.0;
    constexpr double kInheritanceCashFraction = 0.10; // fraction of final cash the next generation starts with

    long long nowEpoch() {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::string formatDuration(long long seconds) {
        long long d = seconds / 86400;
        long long h = (seconds % 86400) / 3600;
        long long m = (seconds % 3600) / 60;
        long long s = seconds % 60;
        std::ostringstream oss;
        if (d > 0) oss << d << "d ";
        if (d > 0 || h > 0) oss << h << "h ";
        if (d > 0 || h > 0 || m > 0) oss << m << "m ";
        oss << s << "s";
        return oss.str();
    }

    // Reads an integer robustly; returns false (and clears the stream) on bad input.
    bool readInt(int& out) {
        if (!(std::cin >> out)) {
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            return false;
        }
        return true;
    }

    bool readDouble(double& out) {
        if (!(std::cin >> out)) {
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            return false;
        }
        return true;
    }
}

Game::Game() {
    lastTickEpoch_ = nowEpoch();
}

bool Game::simulateElapsed(long long seconds, std::vector<std::string>& eventLog, double weatherMult) {
    if (seconds <= 0) return false;

    double legacyMult = 1.0 + static_cast<double>(legacyProdLevel_) * kLegacyProdBonusPerLevel;
    double mult = staff_.multiplier() * life_.productionMultiplier() * life_.ageEfficiency() * legacyMult;

    // The focused business (see trySetStaffFocus) gets an extra staff-scaled
    // multiplier stacked on top of the shared `mult` every business gets.
    // Hired workers (see tryHireWorker) stack on top of that too, but only
    // for the one business they were hired into.
    auto businessMult = [&](const Business& biz) {
        double m = mult * (1.0 + static_cast<double>(biz.workers) * kWorkerBoostPerWorker);
        if (staffFocusBusinessId_.empty() || biz.typeId != staffFocusBusinessId_) return m;
        return m * (1.0 + static_cast<double>(staff_.level) * kStaffFocusBonusPerLevel);
    };

    // Pass 1: raw producers (no input required) — farms, mines, storefront.
    Season season = currentSeason();
    seasonsWitnessedMask_ |= (1 << static_cast<int>(season)); // drives the "Full Cycle" achievement (see GameStats::allSeasonsWitnessed)

    // ---- Season-wide life/economy nudges (see the k*Multiplier constants
    // in Game.h): each season affects a different stat so all four feel
    // distinct, on top of the Farm-only bonuses computed inline below. ----
    // Winter's two penalties are softened by the "season" Legacy track (see
    // legacySeasonNegation()) -- a permanent, cross-generation counter to
    // Winter's harshness rather than a one-life fix.
    double winterNegation = legacySeasonNegation();
    double seasonProdMult = (season == Season::Autumn) ? kAutumnProductionMultiplier : 1.0;
    double seasonEnergyDrainMult = (season == Season::Summer) ? kSummerEnergyDrainMultiplier : 1.0;
    double seasonHungerDrainMult = (season == Season::Winter)
        ? (1.0 + (kWinterHungerDrainMultiplier - 1.0) * (1.0 - winterNegation)) : 1.0;
    double seasonSicknessMult = 1.0;
    if (season == Season::Winter) seasonSicknessMult = 1.0 + (kWinterSicknessMultiplier - 1.0) * (1.0 - winterNegation);
    else if (season == Season::Spring) seasonSicknessMult = kSpringSicknessMultiplier;

    for (auto& b : businessManager_.businesses()) {
        if (b.level <= 0) continue;
        const BusinessType* type = businessManager_.findType(b.typeId);
        if (!type || !type->inputGoodId.empty()) continue;

        // The Farm is the one raw producer whose output good/rate isn't
        // fixed by its BusinessType -- it depends on whichever CropType is
        // currently planted (see Business::cropId), plus a bonus while the
        // current season matches that crop's favorite.
        std::string outputGoodId = type->outputGoodId;
        double cropMult = 1.0;
        if (b.typeId == "farm") {
            if (const CropType* crop = businessManager_.findCrop(b.cropId)) {
                outputGoodId = crop->id;
                bool inSeason = (crop->favoriteSeason == season);
                cropMult = crop->rateMultiplier * (inSeason ? kSeasonBonusMultiplier : 1.0) * weatherMult;
                // Drives the "Master Farmer" achievement (see GameStats::
                // allCropsWitnessedInSeason) -- records that this crop has
                // been actively growing during its favorite season at least
                // once this life, regardless of how briefly.
                if (inSeason && std::find(cropsInSeasonWitnessed_.begin(), cropsInSeasonWitnessed_.end(), crop->id) == cropsInSeasonWitnessed_.end()) {
                    cropsInSeasonWitnessed_.push_back(crop->id);
                }
            }
        }

        double amount = b.ratePerSecond(*type) * businessMult(b) * cropMult * seasonProdMult * static_cast<double>(seconds);
        if (outputGoodId.empty()) {
            money_ += amount;
        } else if (Good* g = market_.find(outputGoodId)) {
            market_.applyProductionPressure(outputGoodId, amount); // reads stock before the line below changes it
            g->stock = std::min(g->stock + amount, maxStockPerGood()); // warehouse cap: excess production is simply not collected
        }
    }

    // Pass 2: processors that consume another good (e.g. Gem Workshop needs ore).
    // Runs after pass 1 so goods produced this same tick are available to feed in.
    for (auto& b : businessManager_.businesses()) {
        if (b.level <= 0) continue;
        const BusinessType* type = businessManager_.findType(b.typeId);
        if (!type || type->inputGoodId.empty()) continue;
        Good* input = market_.find(type->inputGoodId);
        if (!input) continue;
        Good* output = type->outputGoodId.empty() ? nullptr : market_.find(type->outputGoodId);

        double desiredOutput = b.ratePerSecond(*type) * businessMult(b) * seasonProdMult * static_cast<double>(seconds);
        double desiredInput = desiredOutput * type->inputPerOutput;
        double actualInput = std::min(desiredInput, input->stock);
        double actualOutput = type->inputPerOutput > 0.0 ? actualInput / type->inputPerOutput : desiredOutput;

        market_.applyConsumptionPressure(type->inputGoodId, actualInput); // reads stock before the line below changes it
        input->stock -= actualInput;
        if (output) {
            market_.applyProductionPressure(type->outputGoodId, actualOutput);
            output->stock = std::min(output->stock + actualOutput, maxStockPerGood());
        } else {
            money_ += actualOutput;
        }
    }

    // Upkeep: charged after this interval's production, so a business's very
    // first tick of income can help cover its own keep rather than the tax
    // applying to money that hasn't been earned yet.
    money_ = std::max(0.0, money_ - upkeepPerSecond() * static_cast<double>(seconds));

    long long steps = seconds / kMarketStepSeconds;
    if (steps > 0) market_.advance(static_cast<int>(std::min<long long>(steps, 100000)));

    // Rolls flavor events and progresses illness onset/duration (see Life::sick).
    events_.roll(seconds, money_, market_, life_, eventLog, disasterChanceMultiplier(), seasonSicknessMult);

    // Applied last so this interval's production used last interval's energy/hunger —
    // keeps the "did I sleep/eat recently" penalty one step behind, same as everything
    // else here being a single big-step calculation rather than a per-second loop.
    life_.advanceReal(static_cast<double>(seconds), seasonEnergyDrainMult, seasonHungerDrainMult);

    // Death is caused only by neglect, never simply by reaching old age: an
    // untreated illness or a long enough stretch without food. Note this is
    // an end-of-interval check, not a precise clamp like a deterministic
    // condition would allow — for a single very long fast-forward/offline
    // jump, production still runs for the whole interval even if neglect
    // would have been fatal partway through. Acceptable simplification for
    // a prototype; flag if you want it clamped precisely like age used to be.
    if (life_.sickDays >= Life::kSicknessDeathDays) {
        deathCause_ = Localization::t("death_illness");
        return true;
    }
    if (life_.starvingDays >= Life::kStarvationDeathDays) {
        deathCause_ = Localization::t("death_starvation");
        return true;
    }
    return false;
}

bool Game::tickToNow(std::vector<std::string>& eventLog, double weatherMult) {
    long long now = nowEpoch();
    long long elapsed = now - lastTickEpoch_;
    if (elapsed > 0) {
        bool died = simulateElapsed(elapsed, eventLog, weatherMult);
        lastTickEpoch_ = now;
        return died;
    }
    return false;
}

void Game::handleDeath() {
    // Check achievements first, while stats (age included) still reflect the
    // moment of death — otherwise the reset below would erase the age before
    // "Centenarian" ever got a chance to fire.
    checkAchievements();

    // Legacy: the next generation doesn't start from absolute zero. They
    // inherit a slice of the estate plus a foothold in whichever business was
    // furthest along — captured now, before everything gets reset. The
    // estate counts bank savings too, not just cash on hand: the bank is
    // meant to protect money from *disasters* during a life, not shrink what
    // the family ultimately inherits from it.
    double totalWealth = money_ + bankBalance_;
    double inheritedCash = totalWealth * kInheritanceCashFraction;
    std::string bestBusinessId;
    std::string bestTypeId;
    int bestLevel = 0;
    for (const auto& b : businessManager_.businesses()) {
        if (b.level > bestLevel) {
            bestLevel = b.level;
            bestTypeId = b.typeId;
            bestBusinessId = b.typeId;
        }
    }

    std::cout << "\n============================================================\n";
    std::cout << Localization::t("death_age_prefix") << std::fixed << std::setprecision(1) << life_.ageYears()
        << Localization::t("death_age_suffix") << (deathCause_.empty() ? Localization::t("death_generic") : deathCause_) << ".\n";
    std::cout << Localization::t("death_final_estate") << formatNumber(totalWealth)
        << Localization::t("death_staff_level") << staff_.level << ".\n";
    std::cout << Localization::t("death_peak_prefix") << formatNumber(peakMoney_)
        << Localization::t("death_achievements_prefix") << unlockedAchievementCount()
        << Localization::t("death_achievements_suffix") << "\n";
    if (bestLevel > 0) {
        std::cout << Localization::t("death_family_inherits") << formatNumber(inheritedCash)
            << Localization::t("death_foothold_mid") << Localization::t(bestBusinessId)
            << Localization::t("death_foothold_suffix") << "\n";
    } else {
        std::cout << Localization::t("death_family_inherits") << formatNumber(inheritedCash) << ".\n";
    }
    // Recorded before generation_ is incremented, so this entry describes
    // the life that just ended, not the one about to begin.
    generationHistory_.insert(generationHistory_.begin(),
        GenerationRecord{ generation_, peakMoney_, life_.ageYears(), deathCause_.empty() ? Localization::t("death_generic") : deathCause_ });
    if (generationHistory_.size() > static_cast<size_t>(kMaxHistoryEntries)) generationHistory_.resize(kMaxHistoryEntries);

    generation_ += 1;
    std::cout << Localization::t("generation_prefix") << generation_ << Localization::t("generation_suffix") << "\n";

    // Legacy points: a second, permanent form of inheritance on top of the
    // cash/business foothold above -- earned from this life's final cash,
    // spent (see menuLegacy()/tryBuyLegacy*) on small bonuses that apply to
    // every generation from here on, not just the next one.
    int earnedLegacyPoints = std::max(1, static_cast<int>(totalWealth * kLegacyPointsPerCash));
    legacyPoints_ += earnedLegacyPoints;
    std::cout << Localization::t("legacy_earned_prefix") << earnedLegacyPoints << Localization::t("legacy_earned_suffix") << legacyPoints_ << ").\n";
    std::cout << "============================================================\n";

    money_ = 200.0 + inheritedCash + legacyCashLevel_ * kLegacyCashBonusPerLevel;
    peakMoney_ = money_;
    bankBalance_ = 0.0;    // resets like everything else this life built up; only Legacy survives death
    warehouseLevel_ = 0;
    contracts_.clear();
    market_ = Market();
    businessManager_ = BusinessManager();
    if (!bestTypeId.empty()) {
        if (Business* b = businessManager_.find(bestTypeId)) b->level = 1;
    }
    staff_ = Staff();
    staffFocusBusinessId_.clear();
    life_ = Life();
    totalTradeRevenue_ = 0.0;
    cropChangeCount_ = 0;
    seasonsWitnessedMask_ = 0;
    cropsInSeasonWitnessed_.clear();
    minigameHitCount_ = 0;
    deathCause_.clear();
    lastTickEpoch_ = nowEpoch();
    save();
}

void Game::checkAchievements() {
    if (money_ > peakMoney_) peakMoney_ = money_; // called after essentially every money-changing action

    // Harbor/Highlands district membership, for the ownership achievements
    // below -- fishing counts as Harbor even though it predates that zone,
    // since that's where it actually lives now (see GameWorld::buildZones()).
    static const std::vector<std::string> kHarborIds = { "seasalt", "pearlfarm", "shipyard", "cannery", "pearlatelier", "fishing" };
    static const std::vector<std::string> kHighlandsIds = { "dairyfarm", "creamery", "beehive", "meadery", "trapper", "tannery", "teafield", "teahouse", "flaxfield", "linenmill" };

    GameStats stats;
    stats.money = money_;
    int totalLevels = 0, distinctTypes = 0, highestTier = 0, tier2Owned = 0;
    bool anyFullyStaffed = false, anyHarbor = false, anyHighlands = false;
    for (const auto& b : businessManager_.businesses()) {
        totalLevels += b.level;
        if (b.level > 0) {
            distinctTypes++;
            if (const BusinessType* t = businessManager_.findType(b.typeId)) {
                if (t->tier > highestTier) highestTier = t->tier;
                if (t->tier == 2) tier2Owned++;
            }
            if (std::find(kHarborIds.begin(), kHarborIds.end(), b.typeId) != kHarborIds.end()) anyHarbor = true;
            if (std::find(kHighlandsIds.begin(), kHighlandsIds.end(), b.typeId) != kHighlandsIds.end()) anyHighlands = true;
        }
        if (b.workers >= kMaxWorkersPerBusiness) anyFullyStaffed = true;
    }
    stats.totalBusinessLevels = totalLevels;
    stats.distinctBusinessTypesOwned = distinctTypes;
    stats.totalBusinessTypesAvailable = static_cast<int>(businessManager_.types().size());
    stats.highestTierOwned = highestTier;
    stats.tier2BusinessesOwned = tier2Owned;
    stats.anyBusinessFullyStaffed = anyFullyStaffed;
    stats.anyHarborBusinessOwned = anyHarbor;
    stats.anyHighlandsBusinessOwned = anyHighlands;
    stats.totalTradeRevenue = totalTradeRevenue_;
    stats.staffLevel = staff_.level;
    stats.ageYears = life_.ageYears();
    stats.cropChangeCount = cropChangeCount_;
    stats.minigameHitCount = minigameHitCount_;
    stats.allSeasonsWitnessed = (seasonsWitnessedMask_ == 0xF); // all 4 season bits set
    stats.allCropsWitnessedInSeason = cropsInSeasonWitnessed_.size() >= businessManager_.crops().size();
    if (const Business* farm = businessManager_.find("farm")) {
        if (const CropType* crop = businessManager_.findCrop(farm->cropId)) {
            stats.farmInSeason = (crop->favoriteSeason == currentSeason());
        }
    }

    for (const Achievement* a : achievements_.checkAndUnlock(stats, money_)) {
        std::cout << Localization::t("achievement_prefix") << Localization::t("ach_" + a->id + "_name")
            << " - " << Localization::t("ach_" + a->id + "_desc")
            << " (+$" << formatNumber(a->cashReward) << ")\n";
    }
}

void Game::printEventLog(const std::vector<std::string>& log) const {
    if (log.empty()) return;
    size_t shown = std::min(log.size(), kMaxEventLinesShown);
    for (size_t i = 0; i < shown; ++i) std::cout << log[i] << "\n";
    if (log.size() > shown) {
        std::cout << Localization::t("more_events_prefix") << (log.size() - shown) << Localization::t("more_events_suffix") << "\n";
    }
}

void Game::printStatus() const {
    std::cout << "\n==================== " << Localization::t("status_title_prefix") << generation_
        << Localization::t("status_title_suffix") << " ====================\n";
    std::cout << Localization::t("status_cash") << formatNumber(money_)
        << Localization::t("status_staff_lv") << staff_.level
        << Localization::t("status_staff_mult_prefix") << std::fixed << std::setprecision(2) << staff_.multiplier()
        << Localization::t("status_staff_mult_suffix");
    if (double upkeep = upkeepPerSecond(); upkeep > 0.0) {
        std::cout << Localization::t("status_upkeep_prefix") << std::fixed << std::setprecision(3) << upkeep
            << Localization::t("status_upkeep_suffix");
    }
    std::cout << Localization::t("hud_season_prefix") << Localization::t(seasonKey(currentSeason())) << "   ";
    std::cout << Localization::t("status_age") << std::fixed << std::setprecision(1) << life_.ageYears()
        << " / " << Life::kMaxAgeYears << Localization::t("status_yrs");
    if (life_.ageEfficiency() < 1.0) {
        std::cout << Localization::t("status_age_eff_prefix") << std::fixed << std::setprecision(2) << life_.ageEfficiency()
            << Localization::t("status_age_eff_suffix");
    }
    std::cout << Localization::t("status_energy") << std::fixed << std::setprecision(0) << life_.energy << "/100"
        << Localization::t("status_hunger") << std::fixed << std::setprecision(0) << life_.hunger << "/100";
    if (life_.productionMultiplier() < 1.0) {
        std::cout << Localization::t("status_prod_penalty_prefix") << std::fixed << std::setprecision(2) << life_.productionMultiplier()
            << Localization::t("status_prod_penalty_suffix");
    }
    std::cout << "\n";
    if (life_.sick) {
        std::cout << Localization::t("status_sick_prefix") << std::fixed << std::setprecision(1) << life_.sickDays
            << "/" << Life::kSicknessDeathDays << Localization::t("status_sick_suffix");
    }
    if (life_.starvingDays > 0.0) {
        std::cout << Localization::t("status_starving_prefix") << std::fixed << std::setprecision(1) << life_.starvingDays
            << "/" << Life::kStarvationDeathDays << Localization::t("status_starving_suffix");
    }
    std::cout << Localization::t("section_businesses") << "\n";
    businessManager_.print();
    std::cout << Localization::t("section_market") << "\n";
    market_.print();
    std::cout << "=======================================================\n";
}

void Game::menuBusinesses() {
    const auto& businesses = businessManager_.businesses();
    std::cout << Localization::t("menu_businesses_header");
    businessManager_.print();
    std::cout << Localization::t("menu_businesses_prompt");
    int choice;
    if (!readInt(choice) || choice <= 0) return;
    if (choice < 1 || static_cast<size_t>(choice) > businesses.size()) {
        std::cout << Localization::t("invalid_business_number");
        return;
    }
    Business& b = businessManager_.businesses()[choice - 1];
    const BusinessType* type = businessManager_.findType(b.typeId);
    if (!type) return;

    if (b.level == 0 && businessManager_.isLocked(*type)) {
        std::cout << Localization::t("locked_prefix")
            << (!type->prerequisiteTypeId.empty() ? Localization::t(type->prerequisiteTypeId) : type->prerequisiteTypeId)
            << Localization::t("locked_suffix");
        return;
    }

    bool isFarm = (b.typeId == "farm");
    std::cout << Localization::t(isFarm ? "farm_action_prompt" : "business_action_prompt");
    int action;
    if (!readInt(action) || action == 0) return;
    if (action == 2) {
        ActionResult r = tryHireWorker(b.typeId);
        if (r.success) {
            std::cout << Localization::t("worker_hired_prefix") << Localization::t(type->id)
                << Localization::t("worker_hired_mid") << r.count << "/" << kMaxWorkersPerBusiness
                << Localization::t("worker_hired_suffix") << formatNumber(r.amount) << ".\n";
        } else if (r.messageKey == "not_enough_cash_prefix") {
            std::cout << Localization::t(r.messageKey) << formatNumber(r.amount) << ".\n";
        } else {
            std::cout << Localization::t(r.messageKey) << "\n";
        }
        return;
    }
    if (action == 3 && isFarm) {
        std::cout << Localization::t("crop_picker_header");
        std::vector<CropType> crops = cropOptions();
        for (size_t i = 0; i < crops.size(); ++i) {
            std::cout << (i + 1) << ") " << Localization::t(crops[i].id)
                << Localization::t("crop_favorite_prefix") << Localization::t(seasonKey(crops[i].favoriteSeason))
                << Localization::t("crop_favorite_suffix")
                << (crops[i].id == b.cropId ? " *" : "") << "\n";
        }
        std::cout << Localization::t("crop_choice_prompt");
        int cropChoice;
        if (!readInt(cropChoice) || cropChoice < 1 || static_cast<size_t>(cropChoice) > crops.size()) {
            std::cout << Localization::t("invalid_choice");
            return;
        }
        ActionResult r = tryChangeCrop(crops[static_cast<size_t>(cropChoice) - 1].id);
        if (r.success) {
            std::cout << Localization::t("crop_changed_prefix") << Localization::t(b.cropId) << " ($" << formatNumber(r.amount) << ").\n";
        } else if (r.messageKey == "not_enough_cash_prefix") {
            std::cout << Localization::t(r.messageKey) << formatNumber(r.amount) << ".\n";
        } else {
            std::cout << Localization::t(r.messageKey);
        }
        return;
    }
    if (action != 1) {
        std::cout << Localization::t("invalid_choice");
        return;
    }

    double cost = b.nextCost(*type);
    if (money_ < cost) {
        std::cout << Localization::t("not_enough_cash_prefix") << formatNumber(cost) << ".\n";
        return;
    }

    std::cout << Localization::t("bulk_upgrade_prompt");
    int qty;
    if (!readInt(qty)) qty = 1;
    int cap = qty > 0 ? qty : 1000000; // 0 or negative = as many as affordable

    int levelsBought = 0;
    double totalCost = 0.0;
    for (int i = 0; i < cap; ++i) {
        double c = b.nextCost(*type);
        if (money_ < c) break;
        money_ -= c;
        b.level += 1;
        totalCost += c;
        levelsBought++;
    }

    if (levelsBought == 1) {
        std::cout << Localization::t("upgraded_prefix") << Localization::t(type->id)
            << Localization::t("upgraded_mid") << b.level
            << Localization::t("upgraded_suffix") << formatNumber(totalCost) << ".\n";
    } else {
        std::cout << Localization::t("upgraded_prefix") << Localization::t(type->id)
            << Localization::t("bulk_upgraded_mid") << levelsBought
            << Localization::t("bulk_upgraded_levels_suffix") << b.level
            << Localization::t("bulk_upgraded_cost_suffix") << formatNumber(totalCost) << ".\n";
    }
}

void Game::menuTree() const {
    std::cout << "\n";
    businessManager_.printTree();
}

void Game::menuMarket() {
    std::cout << Localization::t("menu_market_header");
    market_.print();
    std::cout << Localization::t("menu_market_action");
    int action;
    if (!readInt(action) || action == 0) return;
    if (action == 3) { menuContracts(); return; }
    if (action != 1 && action != 2) {
        std::cout << Localization::t("invalid_choice");
        return;
    }

    std::cout << Localization::t("enter_good_number");
    int goodIdx;
    if (!readInt(goodIdx)) return;
    auto& goods = market_.goods();
    if (goodIdx < 1 || static_cast<size_t>(goodIdx) > goods.size()) {
        std::cout << Localization::t("invalid_good_number");
        return;
    }
    const Good& g = goods[goodIdx - 1];

    std::cout << Localization::t("enter_quantity");
    double qty;
    if (!readDouble(qty) || qty <= 0) {
        std::cout << Localization::t("invalid_quantity");
        return;
    }

    if (action == 1) {
        // Clamp to warehouse room before spending anything, so the player
        // never pays for units that would just be discarded by the cap.
        double room = std::max(0.0, maxStockPerGood() - g.stock);
        double buyQty = std::min(qty, room);
        if (buyQty <= 0.0) {
            std::cout << Localization::t("warehouse_full");
        } else {
            double cost = g.price * buyQty;
            if (!market_.buy(g.id, buyQty, money_)) {
                std::cout << Localization::t("cant_afford_prefix") << formatNumber(cost) << ".\n";
            } else {
                std::cout << Localization::t("bought_prefix") << formatNumber(buyQty) << " " << Localization::t(g.id)
                    << Localization::t("bought_mid") << formatNumber(cost) << ".\n";
            }
        }
    } else {
        double revenue = g.price * qty;
        if (!market_.sell(g.id, qty, money_)) {
            std::cout << Localization::t("dont_have_that_much_prefix") << Localization::t(g.id) << Localization::t("dont_have_that_much_suffix");
        } else {
            // Same seasonal demand nudge as trySellGood (the graphical
            // path) -- keeps the two interfaces' economies consistent.
            double seasonMult = seasonalGoodSellMultiplier(g.id, currentSeason());
            if (seasonMult != 1.0) {
                double bonus = revenue * (seasonMult - 1.0);
                money_ += bonus;
                revenue += bonus;
            }
            totalTradeRevenue_ += revenue;
            std::cout << Localization::t("sold_prefix") << formatNumber(qty) << " " << Localization::t(g.id)
                << Localization::t("sold_mid") << formatNumber(revenue) << ".\n";
        }
    }
}

void Game::menuContracts() {
    std::cout << Localization::t("menu_contracts_header");
    auto list = contracts();
    for (size_t i = 0; i < list.size(); ++i) {
        std::cout << (i + 1) << ") " << Localization::t(list[i].goodId) << " @ $" << formatNumber(list[i].lockedPrice) << "\n";
    }
    int signOption = static_cast<int>(list.size()) + 1;
    std::cout << signOption << ") " << Localization::t("contract_sign_option") << "\n";
    std::cout << Localization::t("legacy_back_option") << "\n"; // reuses "0) Back" text, same meaning here
    std::cout << Localization::t("contract_choice_prompt");

    int choice;
    if (!readInt(choice) || choice <= 0) return;

    if (choice >= 1 && static_cast<size_t>(choice) <= list.size()) {
        ActionResult r = tryFulfillContract(choice - 1);
        if (r.success) std::cout << Localization::t("contract_fulfilled_prefix") << formatNumber(r.amount) << ".\n";
        else std::cout << Localization::t(r.messageKey);
        return;
    }
    if (choice == signOption) {
        market_.print();
        std::cout << Localization::t("enter_good_number");
        int goodIdx;
        if (!readInt(goodIdx)) return;
        auto& goods = market_.goods();
        if (goodIdx < 1 || static_cast<size_t>(goodIdx) > goods.size()) {
            std::cout << Localization::t("invalid_good_number");
            return;
        }
        const std::string& goodId = goods[goodIdx - 1].id;
        ActionResult r = trySignContract(goodId);
        if (r.success) {
            std::cout << Localization::t("contract_signed_prefix") << Localization::t(goodId)
                << Localization::t("contract_signed_mid") << formatNumber(r.amount) << ".\n";
        } else {
            std::cout << Localization::t(r.messageKey);
        }
    }
}

void Game::menuStaff() {
    std::cout << Localization::t("menu_staff_header");
    std::cout << Localization::t("staff_current_prefix") << staff_.level
        << Localization::t("staff_current_suffix") << std::fixed << std::setprecision(2) << staff_.multiplier() << ")\n";
    std::cout << Localization::t("staff_focus_label")
        << (staffFocusBusinessId_.empty() ? Localization::t("staff_focus_none") : Localization::t(staffFocusBusinessId_))
        << Localization::t("staff_focus_suffix") << std::fixed << std::setprecision(0) << staffFocusBonusPercentPerLevel() << "%/lvl)\n";
    std::cout << Localization::t("staff_cost_prefix") << formatNumber(staff_.nextCost()) << "\n";
    std::cout << Localization::t("staff_hire_prompt");
    int choice;
    if (!readInt(choice)) return;

    if (choice == 2) {
        std::cout << Localization::t("staff_focus_pick_header");
        std::vector<BusinessInfo> owned;
        for (const auto& b : businessInfos()) {
            if (b.level > 0) owned.push_back(b);
        }
        for (size_t i = 0; i < owned.size(); ++i) {
            std::cout << (i + 1) << ") " << Localization::t(owned[i].id) << "\n";
        }
        std::cout << Localization::t("staff_focus_clear_option");
        std::cout << Localization::t("legacy_choice_prompt");
        int pick;
        if (!readInt(pick)) return;
        if (pick == 0) {
            trySetStaffFocus("");
            std::cout << Localization::t("staff_focus_cleared");
        } else if (pick >= 1 && static_cast<size_t>(pick) <= owned.size()) {
            ActionResult r = trySetStaffFocus(owned[static_cast<size_t>(pick) - 1].id);
            if (r.success) std::cout << Localization::t("staff_focus_set_prefix") << Localization::t(owned[static_cast<size_t>(pick) - 1].id) << ".\n";
            else std::cout << Localization::t(r.messageKey);
        }
        return;
    }
    if (choice != 1) return;

    double cost = staff_.nextCost();
    if (money_ < cost) {
        std::cout << Localization::t("not_enough_cash_prefix") << formatNumber(cost) << ".\n";
        return;
    }
    money_ -= cost;
    staff_.level += 1;
    std::cout << Localization::t("staff_hired_prefix") << staff_.level
        << Localization::t("staff_hired_suffix") << std::fixed << std::setprecision(2) << staff_.multiplier() << ".\n";
}

ActionResult Game::trySetStaffFocus(const std::string& businessId) {
    ActionResult result;
    if (businessId.empty()) {
        staffFocusBusinessId_.clear();
        result.success = true;
        return result;
    }
    Business* b = businessManager_.find(businessId);
    if (!b || b->level <= 0) {
        result.messageKey = "invalid_business_number";
        return result;
    }
    staffFocusBusinessId_ = businessId;
    result.success = true;
    return result;
}

void Game::menuLegacy() {
    std::cout << Localization::t("menu_legacy_header");
    std::cout << Localization::t("legacy_points_label") << legacyPoints_ << "\n";
    std::cout << "1) " << Localization::t("legacy_cash_option_prefix") << formatNumber(legacyCashLevel_ * kLegacyCashBonusPerLevel)
        << Localization::t("legacy_cash_option_mid") << legacyCashUpgradeCost() << Localization::t("legacy_points_suffix") << "\n";
    std::cout << "2) " << Localization::t("legacy_prod_option_prefix") << std::fixed << std::setprecision(0)
        << (static_cast<double>(legacyProdLevel_) * kLegacyProdBonusPerLevel * 100.0)
        << Localization::t("legacy_prod_option_mid") << legacyProdUpgradeCost() << Localization::t("legacy_points_suffix") << "\n";
    if (legacySeasonLevel_ >= kLegacySeasonMaxLevel) {
        std::cout << "3) " << Localization::t("legacy_season_option_prefix") << std::fixed << std::setprecision(0)
            << (legacySeasonNegation() * 100.0) << Localization::t("legacy_season_option_mid") << Localization::t("legacy_season_maxed_suffix") << "\n";
    } else {
        std::cout << "3) " << Localization::t("legacy_season_option_prefix") << std::fixed << std::setprecision(0)
            << (legacySeasonNegation() * 100.0) << Localization::t("legacy_season_option_mid")
            << legacySeasonUpgradeCost() << Localization::t("legacy_points_suffix") << "\n";
    }
    std::cout << Localization::t("legacy_back_option") << "\n";

    if (!generationHistory_.empty()) {
        std::cout << Localization::t("history_header");
        for (const auto& rec : generationHistory_) {
            std::cout << Localization::t("history_entry_prefix") << rec.generation
                << Localization::t("history_entry_mid1") << formatNumber(rec.peakMoney)
                << Localization::t("history_entry_mid2") << std::fixed << std::setprecision(1) << rec.ageYears
                << Localization::t("history_entry_mid3") << rec.cause << "\n";
        }
    }

    std::cout << Localization::t("legacy_choice_prompt");

    int choice;
    if (!readInt(choice)) return;
    if (choice == 1) {
        ActionResult r = tryBuyLegacyCashLevel();
        if (r.success) std::cout << Localization::t("legacy_bought_prefix") << r.count << ".\n";
        else std::cout << Localization::t(r.messageKey);
    } else if (choice == 2) {
        ActionResult r = tryBuyLegacyProdLevel();
        if (r.success) std::cout << Localization::t("legacy_bought_prefix") << r.count << ".\n";
        else std::cout << Localization::t(r.messageKey);
    } else if (choice == 3) {
        ActionResult r = tryBuyLegacySeasonLevel();
        if (r.success) std::cout << Localization::t("legacy_bought_prefix") << r.count << ".\n";
        else std::cout << Localization::t(r.messageKey);
    }
}

ActionResult Game::tryBuyLegacyCashLevel() {
    ActionResult result;
    int cost = legacyCashUpgradeCost();
    if (legacyPoints_ < cost) {
        result.messageKey = "legacy_not_enough_points";
        return result;
    }
    legacyPoints_ -= cost;
    legacyCashLevel_ += 1;
    result.success = true;
    result.count = legacyCashLevel_;
    return result;
}

ActionResult Game::tryBuyLegacyProdLevel() {
    ActionResult result;
    int cost = legacyProdUpgradeCost();
    if (legacyPoints_ < cost) {
        result.messageKey = "legacy_not_enough_points";
        return result;
    }
    legacyPoints_ -= cost;
    legacyProdLevel_ += 1;
    result.success = true;
    result.count = legacyProdLevel_;
    return result;
}

ActionResult Game::tryBuyLegacySeasonLevel() {
    ActionResult result;
    if (legacySeasonLevel_ >= kLegacySeasonMaxLevel) {
        result.messageKey = "legacy_season_maxed";
        return result;
    }
    int cost = legacySeasonUpgradeCost();
    if (legacyPoints_ < cost) {
        result.messageKey = "legacy_not_enough_points";
        return result;
    }
    legacyPoints_ -= cost;
    legacySeasonLevel_ += 1;
    result.success = true;
    result.count = legacySeasonLevel_;
    return result;
}

void Game::menuBank() {
    std::cout << Localization::t("menu_bank_header");
    std::cout << Localization::t("bank_cash_label") << formatNumber(money_) << "\n";
    std::cout << Localization::t("bank_balance_label") << formatNumber(bankBalance_)
        << Localization::t("bank_fee_note") << std::fixed << std::setprecision(0) << (kBankWithdrawFeeRate * 100.0) << "%)\n";
    std::cout << Localization::t("bank_action_prompt");

    int action;
    if (!readInt(action) || (action != 1 && action != 2)) return;

    std::cout << Localization::t("bank_amount_prompt");
    double amount;
    if (!readDouble(amount) || amount <= 0) {
        std::cout << Localization::t("invalid_amount");
        return;
    }

    if (action == 1) {
        ActionResult r = tryBankDeposit(amount);
        if (r.success) std::cout << Localization::t("bank_deposited_prefix") << formatNumber(r.amount) << ".\n";
        else std::cout << Localization::t(r.messageKey);
    } else {
        ActionResult r = tryBankWithdraw(amount);
        if (r.success) std::cout << Localization::t("bank_withdrew_prefix") << formatNumber(r.amount) << ".\n";
        else std::cout << Localization::t(r.messageKey);
    }
}

void Game::menuWarehouse() {
    std::cout << Localization::t("menu_warehouse_header");
    std::cout << Localization::t("warehouse_level_label") << warehouseLevel_
        << Localization::t("warehouse_cap_label") << formatNumber(maxStockPerGood()) << ")\n";
    std::cout << Localization::t("warehouse_cost_prefix") << formatNumber(warehouseNextCost()) << "\n";
    std::cout << Localization::t("warehouse_upgrade_prompt");

    int choice;
    if (!readInt(choice) || choice != 1) return;
    ActionResult r = tryUpgradeWarehouse();
    if (r.success) std::cout << Localization::t("warehouse_upgraded_prefix") << warehouseLevel_ << ".\n";
    else std::cout << Localization::t("not_enough_cash_prefix") << formatNumber(r.amount) << ".\n";
}

void Game::menuSleep() {
    static const long long kOneGameDaySeconds =
        static_cast<long long>(Life::kGameSecondsPerDay / Life::kTimeCompression);

    std::cout << Localization::t("menu_sleep_header");
    std::cout << Localization::t("sleep_desc_prefix") << formatDuration(kOneGameDaySeconds) << Localization::t("sleep_desc_suffix");
    std::cout << Localization::t("sleep_prompt");
    int choice;
    if (!readInt(choice) || choice != 1) return;

    std::vector<std::string> log;
    bool died = simulateElapsed(kOneGameDaySeconds, log);
    lastTickEpoch_ += kOneGameDaySeconds;
    printEventLog(log);

    if (died) {
        std::cout << Localization::t("sleep_died");
        handleDeath();
    } else {
        life_.energy = 100.0;
        std::cout << Localization::t("sleep_woke");
    }
}

void Game::menuEat() {
    Good* wheat = market_.find("wheat");
    std::cout << Localization::t("menu_eat_header");
    if (!wheat) {
        std::cout << Localization::t("no_food_source");
        return;
    }
    std::cout << Localization::t("hunger_label") << std::fixed << std::setprecision(0) << life_.hunger << "/100\n";
    std::cout << Localization::t("eat_have_prefix") << formatNumber(wheat->stock)
        << Localization::t("eat_have_mid") << std::fixed << std::setprecision(1) << Life::kHungerRestorePerUnit << Localization::t("eat_have_suffix");
    std::cout << Localization::t("eat_prompt");
    double qty;
    if (!readDouble(qty) || qty <= 0) return;
    if (qty > wheat->stock) {
        std::cout << Localization::t("dont_have_wheat");
        return;
    }
    wheat->stock -= qty;
    life_.hunger = std::min(100.0, life_.hunger + qty * Life::kHungerRestorePerUnit);
    std::cout << Localization::t("ate_prefix") << formatNumber(qty) << Localization::t("ate_suffix") << std::fixed << std::setprecision(0) << life_.hunger << "/100.\n";
}

void Game::menuDoctor() {
    std::cout << Localization::t("menu_doctor_header");
    if (!life_.sick) {
        std::cout << Localization::t("not_sick");
        return;
    }
    std::cout << Localization::t("sick_for_prefix") << std::fixed << std::setprecision(1) << life_.sickDays
        << Localization::t("sick_for_suffix") << Life::kSicknessDeathDays << Localization::t("sick_for_suffix2");
    std::cout << Localization::t("treatment_cost_prefix") << std::fixed << std::setprecision(2) << kDoctorCost << Localization::t("treatment_cost_suffix");
    int choice;
    if (!readInt(choice) || choice != 1) return;
    if (money_ < kDoctorCost) {
        std::cout << Localization::t("not_enough_cash_prefix") << std::fixed << std::setprecision(2) << kDoctorCost << ".\n";
        return;
    }
    money_ -= kDoctorCost;
    life_.sick = false;
    life_.sickDays = 0.0;
    std::cout << Localization::t("all_better");
}

void Game::menuFastForward() {
    std::cout << Localization::t("menu_fastforward_header");
    std::cout << Localization::t("minutes_to_simulate");
    double minutes;
    if (!readDouble(minutes) || minutes <= 0) {
        std::cout << Localization::t("invalid_amount");
        return;
    }
    long long seconds = static_cast<long long>(minutes * 60.0);

    double moneyBefore = money_;
    std::vector<double> stockBefore;
    for (const auto& g : market_.goods()) stockBefore.push_back(g.stock);

    std::vector<std::string> log;
    bool died = simulateElapsed(seconds, log);
    lastTickEpoch_ += seconds; // pretend the clock actually moved this far (overwritten by handleDeath() if died)

    std::cout << Localization::t("simulated_prefix") << formatDuration(seconds) << Localization::t("simulated_suffix");
    std::cout << Localization::t("cash_earned_prefix") << formatNumber(money_ - moneyBefore) << "\n";
    const auto& goods = market_.goods();
    for (size_t i = 0; i < goods.size(); ++i) {
        double delta = goods[i].stock - stockBefore[i];
        if (delta > 1e-6) {
            std::cout << "  +" << formatNumber(delta) << " " << Localization::t(goods[i].id) << "\n";
        } else if (delta < -1e-6) {
            std::cout << "  -" << formatNumber(-delta) << " " << Localization::t(goods[i].id) << "\n";
        }
    }
    printEventLog(log);
    if (died) handleDeath();
}

void Game::menuAchievements() const {
    std::cout << Localization::t("menu_achievements_header");
    for (const auto& a : achievements_.achievements()) {
        std::cout << (a.unlocked ? "[X] " : "[ ] ") << Localization::t("ach_" + a.id + "_name")
            << " - " << Localization::t("ach_" + a.id + "_desc");
        if (!a.unlocked) std::cout << " (" << Localization::t("reward_label") << formatNumber(a.cashReward) << ")";
        std::cout << "\n";
    }
}

bool Game::isBusinessLocked(const std::string& businessId) const {
    const BusinessType* type = businessManager_.findType(businessId);
    if (!type) return false;
    const Business* b = businessManager_.find(businessId);
    if (b && b->level > 0) return false;
    return businessManager_.isLocked(*type);
}

int Game::unlockedAchievementCount() const {
    int n = 0;
    for (const auto& a : achievements_.achievements()) {
        if (a.unlocked) n++;
    }
    return n;
}

double Game::doctorTreatmentCost() const {
    return kDoctorCost;
}

double Game::upkeepPerSecond() const {
    int totalLevels = 0;
    for (const auto& b : businessManager_.businesses()) totalLevels += b.level;
    return static_cast<double>(totalLevels) * kUpkeepPerLevelPerSecond * upkeepDifficultyMultiplier();
}

void Game::setDifficulty(int d) {
    difficulty_ = std::clamp(d, 0, 2);
    static constexpr double kStartingCashMult[3] = { 2.0, 1.0, 0.5 }; // Easy, Normal, Hardcore
    money_ = 200.0 * kStartingCashMult[difficulty_];
}

double Game::upkeepDifficultyMultiplier() const {
    static constexpr double m[3] = { 0.5, 1.0, 1.5 };
    return m[difficulty_];
}

double Game::disasterChanceMultiplier() const {
    static constexpr double m[3] = { 0.5, 1.0, 1.5 };
    return m[difficulty_];
}

std::vector<BusinessInfo> Game::businessInfos() const {
    std::vector<BusinessInfo> result;
    for (const auto& b : businessManager_.businesses()) {
        const BusinessType* t = businessManager_.findType(b.typeId);
        if (!t) continue;
        BusinessInfo info;
        info.id = b.typeId;
        info.level = b.level;
        info.tier = t->tier;
        info.ratePerSecond = b.ratePerSecond(*t);
        info.outputGoodId = businessManager_.resolvedOutputGoodId(b, *t);
        info.inputGoodId = t->inputGoodId;
        info.inputPerOutput = t->inputPerOutput;
        info.nextCost = b.nextCost(*t);
        info.locked = (b.level == 0 && businessManager_.isLocked(*t));
        info.workers = b.workers;
        info.workerCost = b.workers < kMaxWorkersPerBusiness ? workerNextCost(b.typeId) : 0.0;
        if (b.typeId == "farm") {
            info.cropId = b.cropId;
            if (const CropType* crop = businessManager_.findCrop(b.cropId)) {
                info.seasonBonusActive = (crop->favoriteSeason == currentSeason());
            }
        }
        result.push_back(info);
    }
    return result;
}

std::vector<GoodInfo> Game::goodInfos() const {
    std::vector<GoodInfo> result;
    for (const auto& g : market_.goods()) {
        result.push_back(GoodInfo{ g.id, g.price, g.stock });
    }
    return result;
}

std::vector<AchievementInfo> Game::achievementInfos() const {
    std::vector<AchievementInfo> result;
    for (const auto& a : achievements_.achievements()) {
        result.push_back(AchievementInfo{ a.id, a.unlocked, a.cashReward });
    }
    return result;
}

std::vector<std::string> Game::productionTreeLines() const {
    return businessManager_.treeLines();
}

ActionResult Game::tryUpgradeBusiness(const std::string& businessId) {
    return tryUpgradeBusinessBulk(businessId, 1);
}

ActionResult Game::tryUpgradeBusinessBulk(const std::string& businessId, int maxLevels) {
    ActionResult result;
    Business* b = businessManager_.find(businessId);
    const BusinessType* type = businessManager_.findType(businessId);
    if (!b || !type) {
        result.messageKey = "invalid_business_number";
        return result;
    }
    if (b->level == 0 && businessManager_.isLocked(*type)) {
        result.messageKey = "locked_prefix";
        return result;
    }

    // maxLevels <= 0 means "as many as currently affordable" -- the
    // affordability check inside the loop is the real stopping condition
    // either way, this cap just guards against looping forever.
    int cap = maxLevels > 0 ? maxLevels : 1000000;
    int levelsBought = 0;
    double totalCost = 0.0;
    for (int i = 0; i < cap; ++i) {
        double cost = b->nextCost(*type);
        if (money_ < cost) break;
        money_ -= cost;
        b->level += 1;
        totalCost += cost;
        levelsBought++;
    }

    if (levelsBought == 0) {
        result.messageKey = "not_enough_cash_prefix";
        result.amount = b->nextCost(*type);
        return result;
    }
    result.success = true;
    result.amount = totalCost;
    result.count = levelsBought;
    checkAchievements();
    return result;
}

double Game::workerNextCost(const std::string& businessId) const {
    const Business* b = businessManager_.find(businessId);
    const BusinessType* type = businessManager_.findType(businessId);
    if (!b || !type) return 0.0;
    return type->baseCost * kWorkerBaseCostFrac * std::pow(kWorkerCostGrowth, static_cast<double>(b->workers));
}

ActionResult Game::tryHireWorker(const std::string& businessId) {
    ActionResult result;
    Business* b = businessManager_.find(businessId);
    const BusinessType* type = businessManager_.findType(businessId);
    if (!b || !type) {
        result.messageKey = "invalid_business_number";
        return result;
    }
    if (b->level <= 0) {
        result.messageKey = "worker_needs_level";
        return result;
    }
    if (b->workers >= kMaxWorkersPerBusiness) {
        result.messageKey = "workers_maxed";
        return result;
    }
    double cost = workerNextCost(businessId);
    if (money_ < cost) {
        result.messageKey = "not_enough_cash_prefix";
        result.amount = cost;
        return result;
    }
    money_ -= cost;
    b->workers += 1;
    result.success = true;
    result.amount = cost;
    result.count = b->workers;
    checkAchievements();
    return result;
}

ActionResult Game::tryChangeCrop(const std::string& cropId) {
    ActionResult result;
    Business* b = businessManager_.find("farm");
    const CropType* crop = businessManager_.findCrop(cropId);
    if (!b || !crop) {
        result.messageKey = "invalid_crop";
        return result;
    }
    if (b->level <= 0) {
        result.messageKey = "crop_needs_farm";
        return result;
    }
    if (b->cropId == cropId) {
        result.messageKey = "crop_already_active";
        return result;
    }
    if (money_ < kCropSwitchCost) {
        result.messageKey = "not_enough_cash_prefix";
        result.amount = kCropSwitchCost;
        return result;
    }
    money_ -= kCropSwitchCost;
    b->cropId = cropId;
    cropChangeCount_++;
    result.success = true;
    result.amount = kCropSwitchCost;
    checkAchievements(); // catches Good Timing / Crop Rotator right when they become true, not just on the next unrelated action
    return result;
}

ActionResult Game::tryMinigameBonus(const std::string& businessId, bool hit) {
    ActionResult result;
    Business* b = businessManager_.find(businessId);
    const BusinessType* type = businessManager_.findType(businessId);
    if (!b || !type) {
        result.messageKey = "invalid_business_number";
        return result;
    }
    if (b->level <= 0) {
        result.messageKey = "minigame_needs_building";
        return result;
    }
    std::string outputGoodId = businessManager_.resolvedOutputGoodId(*b, *type);
    Good* good = outputGoodId.empty() ? nullptr : market_.find(outputGoodId);
    if (!good) {
        result.messageKey = "invalid_business_number";
        return result;
    }
    double amount = hit ? kMinigameHitBonus : kMinigameMissBonus;

    // Fishing/mining only (not Lumber's chopping -- that isn't seasonal):
    // Summer's a bigger haul with rarer catches more likely, Winter's the
    // leanest. Only ever rolled on an actual hit -- a miss stays the flat
    // consolation amount regardless of season.
    bool rare = false;
    if (hit && (businessId == "fishing" || businessId == "mine" || businessId == "goldmine")) {
        Season season = currentSeason();
        double seasonMult = 1.0;
        double rareChance = kMinigameRareChanceBaseline;
        if (season == Season::Summer) { seasonMult = kMinigameSummerMultiplier; rareChance = kMinigameRareChanceSummer; }
        else if (season == Season::Winter) { seasonMult = kMinigameWinterMultiplier; rareChance = kMinigameRareChanceWinter; }
        amount *= seasonMult;
        rare = (static_cast<double>(std::rand()) / static_cast<double>(RAND_MAX)) < rareChance;
        if (rare) amount *= kMinigameRareBonusMultiplier;
    }

    good->stock = std::min(good->stock + amount, maxStockPerGood());
    if (hit) { minigameHitCount_++; checkAchievements(); } // catches Minigame Pro right on the 5th win
    result.success = true;
    result.amount = amount;
    result.count = hit ? 1 : 0;
    result.goodId = outputGoodId;
    result.rare = rare;
    return result;
}

ActionResult Game::tryForage() {
    // A small flavor bonus unrelated to any particular business -- picks a
    // random Highlands-themed good rather than always the same one, so
    // wandering into a different forageable spot feels a little different.
    static const char* const kForageGoods[] = { "herbs", "honey", "milk", "pelts", "tea_leaves", "flax" };
    constexpr int kForageGoodCount = 6;
    ActionResult result;
    const char* goodId = kForageGoods[std::rand() % kForageGoodCount];
    Good* good = market_.find(goodId);
    if (!good) {
        result.messageKey = "invalid_business_number"; // shouldn't happen -- all six are always registered
        return result;
    }
    double amount = kForageMinAmount + static_cast<double>(std::rand()) / static_cast<double>(RAND_MAX) * (kForageMaxAmount - kForageMinAmount);
    good->stock = std::min(good->stock + amount, maxStockPerGood());
    result.success = true;
    result.amount = amount;
    result.goodId = goodId;
    return result;
}

ActionResult Game::tryBuyDeal(const std::string& goodId, double qty, double totalPrice) {
    ActionResult result;
    Good* good = market_.find(goodId);
    if (!good || qty <= 0.0) {
        result.messageKey = "invalid_business_number";
        return result;
    }
    if (money_ < totalPrice) {
        result.messageKey = "cant_afford_prefix";
        result.amount = totalPrice;
        return result;
    }
    double room = maxStockPerGood() - good->stock;
    double actualQty = std::min(qty, std::max(0.0, room)); // warehouse cap: buy what fits, refund the rest
    double actualCost = room >= qty ? totalPrice : totalPrice * (actualQty / qty);
    money_ -= actualCost;
    good->stock += actualQty;
    result.success = true;
    result.amount = actualCost;
    result.count = static_cast<int>(actualQty);
    result.goodId = goodId;
    return result;
}

ActionResult Game::tryBuyGood(const std::string& goodId, double qty) {
    ActionResult result;
    if (qty <= 0) {
        result.messageKey = "invalid_quantity";
        return result;
    }
    Good* g = market_.find(goodId);
    if (!g) {
        result.messageKey = "invalid_good_number";
        return result;
    }
    // Clamp to warehouse room before spending anything, so the player never
    // pays for units that would just be discarded by the cap.
    double room = std::max(0.0, maxStockPerGood() - g->stock);
    qty = std::min(qty, room);
    if (qty <= 0.0) {
        result.messageKey = "warehouse_full";
        return result;
    }
    double cost = g->price * qty;
    if (!market_.buy(goodId, qty, money_)) {
        result.messageKey = "cant_afford_prefix";
        result.amount = cost;
        return result;
    }
    result.success = true;
    result.amount = cost;
    checkAchievements();
    return result;
}

ActionResult Game::trySellGood(const std::string& goodId, double qty) {
    ActionResult result;
    if (qty <= 0) {
        result.messageKey = "invalid_quantity";
        return result;
    }
    Good* g = market_.find(goodId);
    if (!g) {
        result.messageKey = "invalid_good_number";
        return result;
    }
    double revenue = g->price * qty;
    if (!market_.sell(goodId, qty, money_)) {
        result.messageKey = "dont_have_that_much_prefix";
        return result;
    }
    // Seasonal demand bonus/penalty for a handful of goods (see
    // seasonalGoodSellMultiplier) -- added on top of what Market::sell
    // already paid, so the underlying price random-walk stays untouched.
    double seasonMult = seasonalGoodSellMultiplier(goodId, currentSeason());
    if (seasonMult != 1.0) {
        double bonus = revenue * (seasonMult - 1.0);
        money_ += bonus;
        revenue += bonus;
    }
    totalTradeRevenue_ += revenue;
    result.success = true;
    result.amount = revenue;
    checkAchievements();
    return result;
}

ActionResult Game::tryHireStaff() {
    ActionResult result;
    double cost = staff_.nextCost();
    if (money_ < cost) {
        result.messageKey = "not_enough_cash_prefix";
        result.amount = cost;
        return result;
    }
    money_ -= cost;
    staff_.level += 1;
    result.success = true;
    result.amount = cost;
    checkAchievements();
    return result;
}

ActionResult Game::tryBankDeposit(double amount) {
    ActionResult result;
    if (amount <= 0.0 || amount > money_) {
        result.messageKey = "bank_invalid_amount";
        return result;
    }
    money_ -= amount;
    bankBalance_ += amount;
    result.success = true;
    result.amount = amount;
    return result;
}

ActionResult Game::tryBankWithdraw(double amount) {
    ActionResult result;
    if (amount <= 0.0 || amount > bankBalance_) {
        result.messageKey = "bank_invalid_amount";
        return result;
    }
    double fee = amount * kBankWithdrawFeeRate;
    bankBalance_ -= amount;
    money_ += (amount - fee);
    result.success = true;
    result.amount = amount - fee;
    checkAchievements(); // also tracks peakMoney_
    return result;
}

double Game::warehouseNextCost() const {
    return kWarehouseBaseCost * std::pow(kWarehouseCostGrowth, static_cast<double>(warehouseLevel_));
}

double Game::maxStockPerGood() const {
    return kBaseStorageCap + static_cast<double>(warehouseLevel_) * kStorageCapPerLevel;
}

ActionResult Game::tryUpgradeWarehouse() {
    ActionResult result;
    double cost = warehouseNextCost();
    if (money_ < cost) {
        result.messageKey = "not_enough_cash_prefix";
        result.amount = cost;
        return result;
    }
    money_ -= cost;
    warehouseLevel_ += 1;
    result.success = true;
    result.amount = cost;
    return result;
}

ActionResult Game::trySignContract(const std::string& goodId) {
    ActionResult result;
    Good* g = market_.find(goodId);
    if (!g) {
        result.messageKey = "invalid_good_number";
        return result;
    }
    if (static_cast<int>(contracts_.size()) >= kMaxContracts) {
        result.messageKey = "contract_slots_full";
        return result;
    }
    contracts_.push_back(ContractInfo{ goodId, g->price });
    result.success = true;
    result.amount = g->price;
    return result;
}

ActionResult Game::tryFulfillContract(int index) {
    ActionResult result;
    if (index < 0 || index >= static_cast<int>(contracts_.size())) {
        result.messageKey = "invalid_business_number";
        return result;
    }
    ContractInfo c = contracts_[static_cast<size_t>(index)];
    Good* g = market_.find(c.goodId);
    if (!g || g->stock <= 0.0) {
        result.messageKey = "contract_no_stock";
        return result;
    }
    double revenue = g->stock * c.lockedPrice;
    g->stock = 0.0;
    money_ += revenue;
    totalTradeRevenue_ += revenue; // counts toward Savvy Trader like any other sale
    contracts_.erase(contracts_.begin() + index);
    result.success = true;
    result.amount = revenue;
    checkAchievements();
    return result;
}

ActionResult Game::tryFulfillQuest(const std::string& goodId, double qty, double reward) {
    ActionResult result;
    Good* g = market_.find(goodId);
    if (!g || g->stock < qty) {
        result.messageKey = "quest_not_enough_stock";
        return result;
    }
    g->stock -= qty;
    money_ += reward;
    result.success = true;
    result.amount = reward;
    checkAchievements();
    return result;
}

ActionResult Game::tryEat(double qty) {
    ActionResult result;
    Good* wheat = market_.find("wheat");
    if (!wheat) {
        result.messageKey = "no_food_source";
        return result;
    }
    if (qty <= 0 || qty > wheat->stock) {
        result.messageKey = "dont_have_wheat";
        return result;
    }
    wheat->stock -= qty;
    life_.hunger = std::min(100.0, life_.hunger + qty * Life::kHungerRestorePerUnit);
    result.success = true;
    result.amount = qty;
    return result;
}

ActionResult Game::tryVisitDoctor() {
    ActionResult result;
    if (!life_.sick) {
        result.messageKey = "not_sick";
        return result;
    }
    if (money_ < kDoctorCost) {
        result.messageKey = "not_enough_cash_prefix";
        result.amount = kDoctorCost;
        return result;
    }
    money_ -= kDoctorCost;
    life_.sick = false;
    life_.sickDays = 0.0;
    result.success = true;
    return result;
}

TickOutcome Game::trySleep() {
    static const long long kOneGameDaySeconds =
        static_cast<long long>(Life::kGameSecondsPerDay / Life::kTimeCompression);

    TickOutcome outcome;
    std::vector<std::string> log;
    bool died = simulateElapsed(kOneGameDaySeconds, log);
    lastTickEpoch_ += kOneGameDaySeconds;
    printEventLog(log);
    if (died) {
        outcome.died = true;
        outcome.deathMessage = deathCause_;
        handleDeath();
        outcome.generation = generation_;
    } else {
        life_.energy = 100.0;
    }
    checkAchievements();
    return outcome;
}

TickOutcome Game::tryFastForward(double minutes) {
    TickOutcome outcome;
    if (minutes <= 0) return outcome;
    long long seconds = static_cast<long long>(minutes * 60.0);

    std::vector<std::string> log;
    bool died = simulateElapsed(seconds, log);
    lastTickEpoch_ += seconds;
    printEventLog(log);
    if (died) {
        outcome.died = true;
        outcome.deathMessage = deathCause_;
        handleDeath();
        outcome.generation = generation_;
    }
    checkAchievements();
    return outcome;
}

void Game::load() {
    std::ifstream in(toFsPath(saveFilePath_));
    if (!in) {
        lastTickEpoch_ = nowEpoch();
        return;
    }

    std::unordered_map<std::string, std::string> kv;
    std::string line;
    while (std::getline(in, line)) {
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        kv[line.substr(0, pos)] = line.substr(pos + 1);
    }

    auto getD = [&](const std::string& k, double def) {
        auto it = kv.find(k);
        return it != kv.end() ? std::stod(it->second) : def;
    };
    auto getLL = [&](const std::string& k, long long def) {
        auto it = kv.find(k);
        return it != kv.end() ? std::stoll(it->second) : def;
    };
    auto getI = [&](const std::string& k, int def) {
        auto it = kv.find(k);
        return it != kv.end() ? std::stoi(it->second) : def;
    };
    auto getS = [&](const std::string& k, const std::string& def) {
        auto it = kv.find(k);
        return it != kv.end() ? it->second : def;
    };

    money_ = getD("money", money_);
    peakMoney_ = getD("peakMoney", money_);
    lastTickEpoch_ = getLL("lastTick", nowEpoch());
    generation_ = getI("generation", generation_);
    staff_.level = getI("staff.level", staff_.level);
    staffFocusBusinessId_ = getS("staff.focus", staffFocusBusinessId_);
    totalTradeRevenue_ = getD("stats.totalTradeRevenue", totalTradeRevenue_);
    cropChangeCount_ = getI("stats.cropChangeCount", cropChangeCount_);
    seasonsWitnessedMask_ = getI("stats.seasonsWitnessedMask", seasonsWitnessedMask_);
    cropsInSeasonWitnessed_.clear();
    if (std::string joined = getS("stats.cropsInSeason", ""); !joined.empty()) {
        std::stringstream ss(joined);
        std::string id;
        while (std::getline(ss, id, ',')) cropsInSeasonWitnessed_.push_back(id);
    }
    minigameHitCount_ = getI("stats.minigameHitCount", minigameHitCount_);
    life_.ageDays = getD("life.ageDays", life_.ageDays);
    life_.energy = getD("life.energy", life_.energy);
    life_.hunger = getD("life.hunger", life_.hunger);
    life_.sick = getI("life.sick", life_.sick ? 1 : 0) != 0;
    life_.sickDays = getD("life.sickDays", life_.sickDays);
    life_.starvingDays = getD("life.starvingDays", life_.starvingDays);
    difficulty_ = getI("difficulty", difficulty_);
    bankBalance_ = getD("bankBalance", bankBalance_);
    warehouseLevel_ = getI("warehouseLevel", warehouseLevel_);
    contracts_.clear();
    int contractCount = getI("contract.count", 0);
    for (int i = 0; i < contractCount && i < kMaxContracts; ++i) {
        std::string goodId = getS("contract." + std::to_string(i) + ".goodId", "");
        if (goodId.empty()) continue;
        contracts_.push_back(ContractInfo{ goodId, getD("contract." + std::to_string(i) + ".lockedPrice", 0.0) });
    }
    generationHistory_.clear();
    int historyCount = getI("history.count", 0);
    for (int i = 0; i < historyCount && i < kMaxHistoryEntries; ++i) {
        std::string prefix = "history." + std::to_string(i) + ".";
        GenerationRecord rec;
        rec.generation = getI(prefix + "generation", 0);
        rec.peakMoney = getD(prefix + "peakMoney", 0.0);
        rec.ageYears = getD(prefix + "ageYears", 0.0);
        rec.cause = getS(prefix + "cause", "");
        generationHistory_.push_back(rec);
    }
    legacyPoints_ = getI("legacy.points", legacyPoints_);
    legacyCashLevel_ = getI("legacy.cashLevel", legacyCashLevel_);
    legacyProdLevel_ = getI("legacy.prodLevel", legacyProdLevel_);
    legacySeasonLevel_ = getI("legacy.seasonLevel", legacySeasonLevel_);

    for (auto& g : market_.goods()) {
        g.price = getD("good." + g.id + ".price", g.price);
        g.stock = getD("good." + g.id + ".stock", g.stock);
    }
    for (auto& b : businessManager_.businesses()) {
        b.level = getI("business." + b.typeId + ".level", b.level);
        b.workers = getI("business." + b.typeId + ".workers", b.workers);
        b.cropId = getS("business." + b.typeId + ".crop", b.cropId);
    }
    for (auto& a : achievements_.achievements()) {
        a.unlocked = getI("achievement." + a.id + ".unlocked", a.unlocked ? 1 : 0) != 0;
    }
}

void Game::save() const {
    std::ofstream out(toFsPath(saveFilePath_), std::ios::trunc);
    out << "lastTick=" << lastTickEpoch_ << "\n";
    out << std::fixed << std::setprecision(6);
    out << "money=" << money_ << "\n";
    out << "peakMoney=" << peakMoney_ << "\n";
    out << "generation=" << generation_ << "\n";
    out << "staff.level=" << staff_.level << "\n";
    out << "staff.focus=" << staffFocusBusinessId_ << "\n";
    out << "stats.totalTradeRevenue=" << totalTradeRevenue_ << "\n";
    out << "stats.cropChangeCount=" << cropChangeCount_ << "\n";
    out << "stats.seasonsWitnessedMask=" << seasonsWitnessedMask_ << "\n";
    {
        std::string joined;
        for (size_t i = 0; i < cropsInSeasonWitnessed_.size(); ++i) {
            if (i > 0) joined += ",";
            joined += cropsInSeasonWitnessed_[i];
        }
        out << "stats.cropsInSeason=" << joined << "\n";
    }
    out << "stats.minigameHitCount=" << minigameHitCount_ << "\n";
    out << "life.ageDays=" << life_.ageDays << "\n";
    out << "life.energy=" << life_.energy << "\n";
    out << "life.hunger=" << life_.hunger << "\n";
    out << "life.sick=" << (life_.sick ? 1 : 0) << "\n";
    out << "life.sickDays=" << life_.sickDays << "\n";
    out << "life.starvingDays=" << life_.starvingDays << "\n";
    out << "difficulty=" << difficulty_ << "\n";
    out << "bankBalance=" << bankBalance_ << "\n";
    out << "warehouseLevel=" << warehouseLevel_ << "\n";
    out << "contract.count=" << contracts_.size() << "\n";
    for (size_t i = 0; i < contracts_.size(); ++i) {
        out << "contract." << i << ".goodId=" << contracts_[i].goodId << "\n";
        out << "contract." << i << ".lockedPrice=" << contracts_[i].lockedPrice << "\n";
    }
    out << "history.count=" << generationHistory_.size() << "\n";
    for (size_t i = 0; i < generationHistory_.size(); ++i) {
        std::string prefix = "history." + std::to_string(i) + ".";
        out << prefix << "generation=" << generationHistory_[i].generation << "\n";
        out << prefix << "peakMoney=" << generationHistory_[i].peakMoney << "\n";
        out << prefix << "ageYears=" << generationHistory_[i].ageYears << "\n";
        out << prefix << "cause=" << generationHistory_[i].cause << "\n";
    }
    out << "legacy.points=" << legacyPoints_ << "\n";
    out << "legacy.cashLevel=" << legacyCashLevel_ << "\n";
    out << "legacy.prodLevel=" << legacyProdLevel_ << "\n";
    out << "legacy.seasonLevel=" << legacySeasonLevel_ << "\n";
    for (const auto& g : market_.goods()) {
        out << "good." << g.id << ".price=" << g.price << "\n";
        out << "good." << g.id << ".stock=" << g.stock << "\n";
    }
    for (const auto& b : businessManager_.businesses()) {
        out << "business." << b.typeId << ".level=" << b.level << "\n";
        out << "business." << b.typeId << ".workers=" << b.workers << "\n";
        out << "business." << b.typeId << ".crop=" << b.cropId << "\n";
    }
    for (const auto& a : achievements_.achievements()) {
        out << "achievement." << a.id << ".unlocked=" << (a.unlocked ? 1 : 0) << "\n";
    }
}

void Game::run() {
    startSession();
    runConsoleLoop();
}

void Game::startSession() {
    load();

    long long now = nowEpoch();
    long long elapsed = std::max<long long>(0, now - lastTickEpoch_);
    if (elapsed > 0) {
        double moneyBefore = money_;
        std::vector<std::string> log;
        bool died = simulateElapsed(elapsed, log);
        std::cout << Localization::t("welcome_back_prefix") << formatDuration(elapsed) << Localization::t("welcome_back_suffix");
        std::cout << Localization::t("idle_earnings_prefix") << formatNumber(money_ - moneyBefore) << "\n";
        printEventLog(log);
        if (died) handleDeath();
        else lastTickEpoch_ = now;
    } else {
        lastTickEpoch_ = now;
    }
    checkAchievements();
}

TickOutcome Game::tickBackground(double weatherMult) {
    TickOutcome outcome;
    std::vector<std::string> log;
    bool died = tickToNow(log, weatherMult);
    printEventLog(log);
    if (died) {
        outcome.died = true;
        outcome.deathMessage = deathCause_;
        handleDeath();
        outcome.generation = generation_;
    }
    checkAchievements();
    return outcome;
}

void Game::interact(const std::string& buildingId) {
    if (buildingId == "market") { menuMarket(); return; }
    if (buildingId == "staff") { menuStaff(); return; }
    if (buildingId == "sleep") { menuSleep(); return; }
    if (buildingId == "eat") { menuEat(); return; }
    if (buildingId == "doctor") { menuDoctor(); return; }
    if (buildingId == "townhall") { menuTree(); menuAchievements(); return; }
    if (buildingId == "achievements") { menuAchievements(); return; }
    // Any of the 12 production-tree business ids (or an unrecognized id)
    // fall back to the shared business management screen.
    menuBusinesses();
}

void Game::exitAndSave() {
    std::vector<std::string> log;
    if (tickToNow(log)) handleDeath();
    printEventLog(log);
    save();
    std::cout << Localization::t("saved");
}

void Game::runConsoleLoop() {
    bool running = true;
    while (running) {
        std::vector<std::string> log;
        bool died = tickToNow(log);
        printEventLog(log);
        if (died) handleDeath();
        checkAchievements();
        printStatus();
        std::cout << "\n" << Localization::t("main_menu_1")
            << "\n" << Localization::t("main_menu_2")
            << "\n" << Localization::t("main_menu_3")
            << "\n" << Localization::t("main_menu_4")
            << "\n" << Localization::t("main_menu_5")
            << "\n" << Localization::t("main_menu_6")
            << "\n" << Localization::t("main_menu_7")
            << "\n" << Localization::t("main_menu_8")
            << "\n" << Localization::t("main_menu_9")
            << "\n" << Localization::t("main_menu_legacy")
            << "\n" << Localization::t("main_menu_bank")
            << "\n" << Localization::t("main_menu_warehouse")
            << "\n" << Localization::t("main_menu_10")
            << "\n" << Localization::t("main_menu_11")
            << "\n" << Localization::t("mode_prompt");

        int choice;
        if (!readInt(choice)) {
            if (std::cin.eof()) {
                std::cout << Localization::t("input_closed");
                std::vector<std::string> exitLog;
                if (tickToNow(exitLog)) handleDeath();
                printEventLog(exitLog);
                save();
                break;
            }
            std::cout << Localization::t("please_enter_number");
            continue;
        }
        switch (choice) {
            case 1: menuBusinesses(); break;
            case 2: menuTree(); break;
            case 3: menuMarket(); break;
            case 4: menuStaff(); break;
            case 5: menuSleep(); break;
            case 6: menuEat(); break;
            case 7: menuDoctor(); break;
            case 8: menuFastForward(); break;
            case 9: menuAchievements(); break;
            case 10: menuLegacy(); break;
            case 11: menuBank(); break;
            case 12: menuWarehouse(); break;
            case 13: {
                std::vector<std::string> l;
                if (tickToNow(l)) handleDeath();
                printEventLog(l);
                save();
                std::cout << Localization::t("saved");
                break;
            }
            case 14: {
                std::vector<std::string> l;
                if (tickToNow(l)) handleDeath();
                printEventLog(l);
                save();
                running = false;
                break;
            }
            default:
                std::cout << Localization::t("invalid_choice_menu");
        }
    }
    std::cout << Localization::t("final_farewell");
}
