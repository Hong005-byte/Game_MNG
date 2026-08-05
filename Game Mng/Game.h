#pragma once
#include <string>
#include <vector>
#include <algorithm>
#include "Market.h"
#include "Business.h"
#include "Staff.h"
#include "Events.h"
#include "Achievements.h"
#include "Life.h"

// Read-only snapshot of one business, for a UI to render without touching
// Game's internals directly.
struct BusinessInfo {
    std::string id;
    int level = 0;
    int tier = 1; // 1 = raw, 2 = processor, 3 = advanced (0 would mean "service", but none currently use tier 0)
    double ratePerSecond = 0.0;
    std::string outputGoodId; // empty => produces cash directly
    std::string inputGoodId;  // empty => no input required
    double inputPerOutput = 0.0;
    double nextCost = 0.0;
    bool locked = false;
    int workers = 0;        // hired workers boosting just this business (see Game::tryHireWorker)
    double workerCost = 0.0; // cost of the next worker, 0 if already at the cap
    std::string cropId;      // only meaningful for "farm" -- its currently-active CropType id
    bool seasonBonusActive = false; // only meaningful for "farm" -- true if cropId's favorite season is current
};

// Read-only snapshot of one market good.
struct GoodInfo {
    std::string id;
    double price = 0.0;
    double stock = 0.0;
};

// Read-only snapshot of one achievement.
struct AchievementInfo {
    std::string id;
    bool unlocked = false;
    double reward = 0.0;
};

// One past generation's summary, recorded at the moment of death for the
// Legacy screen's history list. `cause` is already the localized death-cause
// sentence (frozen in whichever language was active at the time -- a minor
// cosmetic tradeoff for not needing a separate code->text lookup later).
struct GenerationRecord {
    int generation = 0;
    double peakMoney = 0.0;
    double ageYears = 0.0;
    std::string cause;
};

// A signed price-lock contract: `lockedPrice` was the good's market price at
// the moment it was signed. Fulfilling it later sells all current stock of
// that good at `lockedPrice` regardless of where the market price has since
// moved -- a hedge against a crash, at the cost of missing out if the price
// instead rises.
struct ContractInfo {
    std::string goodId;
    double lockedPrice = 0.0;
};

// Result of a state-mutating action performed outside the console (e.g. from
// a graphical UI): whether it worked, and — on failure — a localization key
// describing why, so the caller can show its own feedback instead of Game
// printing to the console. `amount` carries a generic number relevant to the
// outcome (cost paid, revenue earned, cost still needed, etc.). `count`
// carries a generic integer count where relevant (e.g. levels bought in a
// bulk upgrade); unused (0) for actions that don't need it. `goodId` names
// which market good `amount` refers to, for actions (foraging, minigame
// bonuses) whose good isn't otherwise implied by which business/id the
// caller already passed in.
struct ActionResult {
    bool success = false;
    std::string messageKey;
    double amount = 0.0;
    int count = 0;
    std::string goodId;
    bool rare = false; // tryMinigameBonus only -- true if a seasonal "rare catch" roll doubled `amount`
};

// Result of any time-advancing action that could cause death (background
// ticking, sleeping, fast-forwarding). `deathMessage` is already the
// localized cause string (see Game::simulateElapsed), ready to show as-is.
struct TickOutcome {
    bool died = false;
    std::string deathMessage;
    int generation = 0;
};

// Top-level game object: owns the player's cash, the market, the businesses,
// staff, and achievements, and drives the console menu loop. Time-based
// ("idle/AFK") progress is simulated both on load (offline progress) and
// between menu actions during a live session.
//
// Death is caused only by neglect — untreated illness or prolonged
// starvation — never simply by reaching old age (see Life.h). Either one
// resets the run to a fresh character; achievements carry over.
//
// Two parallel interfaces exist for the same underlying state: the classic
// console menu* methods (print prompts, block on std::cin, print results),
// and a non-console data/action API (businessInfos(), tryUpgradeBusiness(),
// etc.) for the graphical world's in-window overlay UI. Both mutate the same
// state; neither one is aware of the other.
class Game {
public:
    Game();

    // Which file load()/save() read and write. Must be called before
    // startSession()/run() -- set by the save-slot picker in main() once the
    // player has chosen "New Game" or an existing slot. Defaults to
    // "save.txt" in the working directory so code that predates the slot
    // picker (or a stray direct Game instance) still works.
    void setSaveFile(const std::string& path) { saveFilePath_ = path; }
    const std::string& saveFilePath() const { return saveFilePath_; }

    // 0 = Easy, 1 = Normal, 2 = Hardcore. Only meaningful to call for a brand
    // new save, before startSession() -- it sets starting cash immediately,
    // which load() (called from startSession()) would otherwise be the only
    // thing allowed to touch. Loading an existing save restores its own
    // difficulty from disk instead; don't call this for that path.
    void setDifficulty(int d);
    int difficulty() const { return difficulty_; }
    double upkeepDifficultyMultiplier() const;
    double disasterChanceMultiplier() const;

    // Full classic console experience: session setup + the interactive menu loop.
    void run();

    // Loads the save, applies offline catch-up, and runs the initial
    // achievement check — everything run() does before its menu loop starts.
    // Callers driving their own loop (e.g. a graphical view) call this once
    // up front, then tickBackground() every frame instead of runConsoleLoop().
    void startSession();

    // The classic interactive text menu loop (assumes startSession() already ran).
    void runConsoleLoop();

    // Advances the economy by whatever real time has passed since the last
    // tick, printing any event/achievement lines but no full status screen —
    // for callers (like a graphical world view) that render their own HUD
    // instead of the console status block. Handles death itself (including
    // the console announcement); the returned outcome lets a graphical
    // caller also show its own death notice.
    //
    // `weatherMult` is a live-only nudge to the Farm's crop output (rain
    // outside Winter helps it, snow during Winter hurts it -- see
    // GameWorld's per-frame call, which is the only caller that ever passes
    // anything but the default). Left at 1.0 for the console build (no
    // weather concept there) and for offline catch-up (simulateElapsed via
    // load()), which stays weather-neutral rather than trying to guess what
    // the weather was while the game was closed.
    TickOutcome tickBackground(double weatherMult = 1.0);

    // Dispatches to the console menu action for a world-view building id.
    // The 12 production-tree business ids all open the shared business
    // manager screen; the rest map 1:1 to their console menu.
    void interact(const std::string& buildingId);

    // Ticks once more and saves, mirroring the console "Save & Quit" action.
    void exitAndSave();

    // Saves immediately without ticking or quitting -- the Pause overlay's
    // "Save" button (see exitAndSave() above for the quit-and-save variant).
    void saveNow() const { save(); }

    // True if `businessId` still needs its prerequisite built (and isn't
    // already owned) — lets a graphical view show a lock indicator without
    // walking up to check.
    bool isBusinessLocked(const std::string& businessId) const;

    // How many achievements are currently unlocked — a graphical view can
    // poll this each frame and compare to the previous value to notice a
    // fresh unlock (e.g. to play a sound) without Game exposing an event bus.
    int unlockedAchievementCount() const;

    double money() const { return money_; }
    int generation() const { return generation_; }
    double ageYears() const { return life_.ageYears(); }
    double energy() const { return life_.energy; }
    double hunger() const { return life_.hunger; }
    bool isSick() const { return life_.sick; }
    double sickDays() const { return life_.sickDays; }
    double starvingDays() const { return life_.starvingDays; }
    double sicknessDeathDays() const { return Life::kSicknessDeathDays; }
    double starvationDeathDays() const { return Life::kStarvationDeathDays; }
    double ageEfficiency() const { return life_.ageEfficiency(); }
    double lifeProductionMultiplier() const { return life_.productionMultiplier(); }
    double hungerRestorePerUnit() const { return Life::kHungerRestorePerUnit; }

    // ---- Seasons: derived from life_.ageDays (already persisted, already
    // resets each generation) rather than tracked separately. Drives the
    // Farm's favorite-season yield bonus (see kSeasonBonusMultiplier below
    // and the farm special-case in simulateElapsed). ----
    // 2 in-game months per season (see Life::kDaysPerMonth/kMonthsPerYear,
    // the one source of truth for the calendar) -- 4 seasons cycle over
    // exactly one Life::kDaysPerYear, so the season clock and the age clock
    // never drift apart the way they used to (season used to cycle every
    // 120 days while a "year" for age purposes was a separate 365).
    static constexpr int kGameDaysPerSeason = static_cast<int>(Life::kDaysPerYear) / 4;
    Season currentSeason() const {
        int dayInCycle = static_cast<int>(life_.ageDays) % (4 * kGameDaysPerSeason);
        return static_cast<Season>(dayInCycle / kGameDaysPerSeason);
    }
    // In-game days remaining until the season changes -- lets a UI show a
    // countdown so the player can plan ahead (replant before Winter, etc.)
    // instead of the season just changing out from under them.
    int daysUntilNextSeason() const {
        int dayInCycle = static_cast<int>(life_.ageDays) % (4 * kGameDaysPerSeason);
        int dayWithinSeason = dayInCycle % kGameDaysPerSeason;
        return kGameDaysPerSeason - dayWithinSeason;
    }

    int staffLevel() const { return staff_.level; }
    double staffMultiplier() const { return staff_.multiplier(); }
    double staffNextCost() const { return staff_.nextCost(); }
    double doctorTreatmentCost() const;

    // ---- Staff specialization: free to reassign, no separate currency.
    // Whichever owned business is focused gets an extra staff-scaled
    // multiplier on top of the normal global bonus every business already
    // gets -- a strategic choice of where to concentrate the workforce
    // rather than another thing to grind for. ----
    const std::string& staffFocusBusinessId() const { return staffFocusBusinessId_; }
    double staffFocusBonusPercentPerLevel() const { return kStaffFocusBonusPerLevel * 100.0; }
    // Pass "" to clear the focus. Fails only if businessId names a real,
    // currently-owned (level > 0) business and isn't found as such.
    ActionResult trySetStaffFocus(const std::string& businessId);
    double upkeepPerSecond() const; // current total upkeep drain across all owned business levels

    // ---- Legacy: a prestige currency that survives death (unlike money,
    // the market, and businesses, which all reset -- see handleDeath()).
    // Earned once per death based on that life's final cash, spent on small
    // permanent bonuses that apply to every future generation. ----
    int legacyPoints() const { return legacyPoints_; }
    int legacyCashLevel() const { return legacyCashLevel_; }
    int legacyProdLevel() const { return legacyProdLevel_; }
    int legacyCashUpgradeCost() const { return legacyCashLevel_ + 1; }
    int legacyProdUpgradeCost() const { return legacyProdLevel_ + 1; }
    double legacyCashBonusPerLevel() const { return kLegacyCashBonusPerLevel; }
    double legacyProdBonusPercentPerLevel() const { return kLegacyProdBonusPerLevel * 100.0; }
    ActionResult tryBuyLegacyCashLevel();
    ActionResult tryBuyLegacyProdLevel();

    // A third permanent Legacy track: each level cancels a slice of Winter's
    // extra hunger-drain/sickness penalty (see kWinter*Multiplier in the
    // Season section below) -- capped at 5 levels, which cancels it entirely.
    // Doesn't touch Winter's Farm-only snow penalty (kSnowPenaltyMultiplier);
    // that one's weather, not a life stat, so it stays as a real seasonal cost.
    static constexpr int kLegacySeasonMaxLevel = 5;
    int legacySeasonLevel() const { return legacySeasonLevel_; }
    int legacySeasonUpgradeCost() const { return legacySeasonLevel_ + 1; }
    double legacySeasonBonusPercentPerLevel() const { return kLegacySeasonBonusPerLevel * 100.0; }
    ActionResult tryBuyLegacySeasonLevel();
    // How much of Winter's extra penalty is currently cancelled (0..1) -- used
    // by simulateElapsed to soften kWinterHungerDrainMultiplier/kWinterSicknessMultiplier.
    double legacySeasonNegation() const { return std::min(1.0, static_cast<double>(legacySeasonLevel_) * kLegacySeasonBonusPerLevel); }

    // Highest cash this life has reached (resets each generation, unlike
    // Legacy points) and a bounded log of past generations' summaries --
    // both purely informational, shown on the Legacy screen and the death notice.
    double peakMoney() const { return peakMoney_; }
    std::vector<GenerationRecord> generationHistory() const { return generationHistory_; }

    std::vector<BusinessInfo> businessInfos() const;
    std::vector<GoodInfo> goodInfos() const;
    std::vector<AchievementInfo> achievementInfos() const;
    std::vector<std::string> productionTreeLines() const;

    // ---- Non-console actions for a graphical UI. Each mutates state and
    // returns a result instead of printing/reading console I/O. ----
    ActionResult tryUpgradeBusiness(const std::string& businessId);
    // Buys up to `maxLevels` levels in one go, stopping early if cash runs
    // out; `maxLevels <= 0` means "buy as many as currently affordable".
    // result.count is how many levels were actually bought, result.amount is
    // the total spent. Fails (no levels bought) the same way a single
    // upgrade would: locked, or can't afford even one more level.
    ActionResult tryUpgradeBusinessBulk(const std::string& businessId, int maxLevels);

    // ---- Per-business worker hiring: separate from the global Staff Office
    // and the foreman focus (staffFocusBusinessId_ above), this boosts just
    // one business's own output. Priced off that business's own cost, with a
    // cap and a steep growth curve so it adds depth without trivializing the
    // economy. ----
    static constexpr int kMaxWorkersPerBusiness = 5;
    double workerBoostPercentPerWorker() const { return kWorkerBoostPerWorker * 100.0; }
    double workerNextCost(const std::string& businessId) const;
    ActionResult tryHireWorker(const std::string& businessId);

    // ---- Farm crop selection (see Business.h's CropType/Business::cropId).
    // A flat fee rather than free/instant so switching to chase whichever
    // season is active is a real choice, not a costless reflex. ----
    static constexpr double kSeasonBonusMultiplier = 1.5;
    // Live weather nudges to the Farm only (see tickBackground's
    // `weatherMult` param) -- rain helps crops outside Winter, snow during
    // Winter hurts them. Deliberately small; this is flavor, not a lever.
    static constexpr double kRainBonusMultiplier = 1.10;
    static constexpr double kSnowPenaltyMultiplier = 0.90;

    // ---- Season-wide life effects (beyond the Farm-only bonuses above):
    // each season nudges a different stat, so all four feel distinct rather
    // than just "winter is worse". Applied in simulateElapsed via the
    // `season` it already computes for the Farm bonus. ----
    static constexpr double kSpringSicknessMultiplier = 0.6;   // mild season -- less likely to fall ill
    static constexpr double kSummerEnergyDrainMultiplier = 1.3; // heat tires you out faster
    static constexpr double kAutumnProductionMultiplier = 1.08; // harvest season -- a small boost across every business
    static constexpr double kWinterHungerDrainMultiplier = 1.3; // cold burns through food faster
    static constexpr double kWinterSicknessMultiplier = 1.4;    // cold season -- more likely to fall ill
    double cropSwitchCost() const { return kCropSwitchCost; }
    std::vector<CropType> cropOptions() const { return businessManager_.crops(); }
    ActionResult tryChangeCrop(const std::string& cropId);

    // ---- Minigames (graphical-only -- see GameWorld's F key). `hit` is
    // whether the player succeeded (the timing bar's indicator was inside
    // the target zone, or the chopping click target was reached in time);
    // either way something is produced, just more on a hit. Shared by the
    // Fishing Dock, Mine/Gold Mine, and Lumber Camp -- `businessId` picks
    // which one's own output good gets the bonus. result.amount is how much
    // was added. ----
    ActionResult tryMinigameBonus(const std::string& businessId, bool hit);
    int minigameHitCount() const { return minigameHitCount_; }

    // ---- Highlands foraging (graphical-only): a small random bonus of one
    // of the Highlands' flavor goods, from walking up to a collectible spot
    // rather than a building. result.amount is how much was added,
    // result.messageKey (on success) names which good. ----
    ActionResult tryForage();

    // ---- Traveling merchant deals (graphical-only -- see GameWorld's
    // npc_trader). A flat-price bundle buy, distinct from tryBuyGood in that
    // the price is whatever the NPC already quoted (a discount off market
    // price at the moment the deal was rolled), not the market's live price. ----
    ActionResult tryBuyDeal(const std::string& goodId, double qty, double totalPrice);

    ActionResult tryBuyGood(const std::string& goodId, double qty);
    ActionResult trySellGood(const std::string& goodId, double qty);
    ActionResult tryHireStaff();
    ActionResult tryEat(double qty);
    ActionResult tryVisitDoctor();
    TickOutcome trySleep();
    TickOutcome tryFastForward(double minutes);

    // ---- Bank: a separate cash pool. Theft/recession disasters and the
    // theft flavor event only ever touch money_ (see EventSystem::roll),
    // never bankBalance_, so parking cash here is a genuine hedge -- at the
    // cost of a withdrawal fee, so it's not a strictly-better place to keep
    // every dollar. ----
    double bankBalance() const { return bankBalance_; }
    double bankWithdrawFeeRate() const { return kBankWithdrawFeeRate; }
    ActionResult tryBankDeposit(double amount);
    ActionResult tryBankWithdraw(double amount);

    // ---- Warehouse: raises the per-good stock cap. Production and buying
    // are both clamped to this so goods can't be hoarded without limit. ----
    int warehouseLevel() const { return warehouseLevel_; }
    double warehouseNextCost() const;
    double maxStockPerGood() const;
    ActionResult tryUpgradeWarehouse();

    // ---- Contracts: sign one to lock a good's current price; fulfill it
    // later to sell all of that good's current stock at the locked price. ----
    static constexpr int kMaxContracts = 3;
    std::vector<ContractInfo> contracts() const { return contracts_; }
    ActionResult trySignContract(const std::string& goodId);
    ActionResult tryFulfillContract(int index);

    // ---- NPC fetch quests (graphical world only -- the quests themselves
    // live in GameWorld's Npc state, not here; this just performs the
    // "hand over goods, receive cash" trade both sides agreed to). ----
    ActionResult tryFulfillQuest(const std::string& goodId, double qty, double reward);

private:
    static constexpr int kMaxHistoryEntries = 8;

    // Legacy bonus tuning: kept as constants (not save-file-editable) so
    // load() can always recompute what a given level *means* even if these
    // ever change between versions -- only the levels themselves persist.
    static constexpr double kLegacyCashBonusPerLevel = 50.0;   // added to starting cash, per level
    static constexpr double kLegacyProdBonusPerLevel = 0.02;   // +2% production multiplier, per level
    static constexpr double kLegacySeasonBonusPerLevel = 0.20; // cancels 20% of Winter's extra hunger/sickness penalty, per level (see kLegacySeasonMaxLevel)
    static constexpr double kLegacyPointsPerCash = 1.0 / 1500.0; // final cash -> legacy points on death

    // Upkeep: a small ongoing cash drain per business level, per second, so a
    // sprawling idle empire costs something to keep running instead of
    // compounding for free forever -- money can't go below $0 from this alone.
    static constexpr double kUpkeepPerLevelPerSecond = 0.003;

    static constexpr double kStaffFocusBonusPerLevel = 0.03; // extra multiplier on the focused business, per staff level

    // Worker-hiring tuning (see kMaxWorkersPerBusiness above, which is public
    // so a UI can show "N / cap"). Next-worker cost is
    // type.baseCost * kWorkerBaseCostFrac * kWorkerCostGrowth^workers, i.e.
    // scaled off that business's own price tag (cheap businesses hire cheap
    // workers, expensive ones hire expensive workers) and growing faster
    // than a business's own level-upgrade cost (typically ~1.15-1.21) so
    // stacking workers gets expensive quickly. Each worker is worth +8% to
    // that one business only, capped at 5 => max +40% -- comparable to, not
    // dominant over, the foreman-focus bonus above.
    static constexpr double kWorkerBaseCostFrac = 0.75;
    static constexpr double kWorkerCostGrowth = 1.35;
    static constexpr double kWorkerBoostPerWorker = 0.08;

    static constexpr double kCropSwitchCost = 20.0; // flat fee to replant the Farm with a different crop

    static constexpr double kMinigameHitBonus = 15.0;  // good stock added on a hit (fishing/mining timing bar, lumber chopping)
    static constexpr double kMinigameMissBonus = 3.0;  // good stock added on a miss -- never a total whiff

    // ---- Seasonal nudge to the Fishing Dock/Mine/Gold Mine timing-bar
    // minigame only (not Lumber's chopping, which isn't seasonal) -- Summer
    // is the plentiful season (bigger catches, rare hauls more likely),
    // Winter the leanest. Spring/Autumn stay at the baseline. ----
    static constexpr double kMinigameSummerMultiplier = 1.3;
    static constexpr double kMinigameWinterMultiplier = 0.7;
    static constexpr double kMinigameRareChanceBaseline = 0.08;
    static constexpr double kMinigameRareChanceSummer = 0.15;
    static constexpr double kMinigameRareChanceWinter = 0.03;
    static constexpr double kMinigameRareBonusMultiplier = 2.0; // on top of the season multiplier above
    static constexpr double kForageMinAmount = 2.0;
    static constexpr double kForageMaxAmount = 6.0;

    static constexpr double kBankWithdrawFeeRate = 0.03; // charged on withdrawal only, not deposit

    static constexpr double kWarehouseBaseCost = 300.0;
    static constexpr double kWarehouseCostGrowth = 1.25;
    static constexpr double kBaseStorageCap = 2000.0;      // per good, before any warehouse levels
    static constexpr double kStorageCapPerLevel = 1500.0;

    std::string saveFilePath_ = "save.txt";
    int difficulty_ = 1; // 0=Easy, 1=Normal, 2=Hardcore
    double money_ = 200.0;
    double bankBalance_ = 0.0;
    int warehouseLevel_ = 0;
    std::vector<ContractInfo> contracts_;
    double peakMoney_ = 200.0;                        // resets each generation
    std::vector<GenerationRecord> generationHistory_; // persists across deaths, bounded to kMaxHistoryEntries
    Market market_;
    BusinessManager businessManager_;
    Staff staff_;
    std::string staffFocusBusinessId_; // empty = no specialization assigned
    EventSystem events_;
    AchievementManager achievements_;
    Life life_;
    double totalTradeRevenue_ = 0.0; // cumulative $ earned from selling goods
    int cropChangeCount_ = 0;   // this life's successful tryChangeCrop() calls -- resets each generation, like totalTradeRevenue_
    int minigameHitCount_ = 0;  // this life's successful (hit) minigame results -- same reset pattern
    int seasonsWitnessedMask_ = 0; // bit i set once currentSeason() == Season(i) has been observed this life -- same reset pattern, drives the "Full Cycle" achievement
    std::vector<std::string> cropsInSeasonWitnessed_; // crop ids that have grown during their favorite season at least once this life -- drives "Master Farmer"
    long long lastTickEpoch_ = 0;
    std::string deathCause_; // set by simulateElapsed right before it returns true
    int generation_ = 1; // incremented on each death/rebirth; persists across lives

    // Legacy state -- unlike everything above (except generation_ and
    // achievements), this is NOT reset in handleDeath(): it's the whole
    // point of a prestige currency that it survives the reset.
    int legacyPoints_ = 0;
    int legacyCashLevel_ = 0;
    int legacyProdLevel_ = 0;
    int legacySeasonLevel_ = 0;

    // Advances businesses' production, market prices, random events, and the
    // life clock by `seconds` of simulated time. Used for both offline
    // catch-up and manual fast-forward. Any event flavor text is appended to
    // `eventLog`. Returns true if the character died of neglect (untreated
    // illness or starvation) during this call — see deathCause_ for which.
    bool simulateElapsed(long long seconds, std::vector<std::string>& eventLog, double weatherMult = 1.0);

    // Ticks the clock forward to "now" based on real elapsed time. Returns
    // true if the character died during this call.
    bool tickToNow(std::vector<std::string>& eventLog, double weatherMult = 1.0);

    // Announces death, resets the run to a fresh character (achievements persist).
    void handleDeath();

    void checkAchievements();
    void printEventLog(const std::vector<std::string>& log) const;

    void printStatus() const;
    void menuBusinesses();
    void menuTree() const;
    void menuMarket();
    void menuStaff();
    void menuSleep();
    void menuEat();
    void menuDoctor();
    void menuFastForward();
    void menuAchievements() const;
    void menuLegacy();
    void menuBank();
    void menuWarehouse();
    void menuContracts();

    void load();
    void save() const;
};
