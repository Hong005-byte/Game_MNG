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

// One material's build requirement alongside how much of it is actually
// sitting in the warehouse right now -- lets a UI show "have X / need Y"
// (and gray out Start Construction) without a second round-trip. Also
// reused by BusinessInfo::inputs for an ongoing-production recipe's
// "need vs have" list (there `required` is a per-output ratio, not a
// one-time cost, but the shape -- and the UI treatment -- is the same).
struct BuildMaterialInfo {
    std::string goodId;
    double required = 0.0;
    double have = 0.0;
};

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
    // Primary input (inputGoodId/inputPerOutput above) plus any
    // BusinessType::extraInputs, combined into one live "need vs have" list
    // for a UI -- reuses BuildMaterialInfo's shape (required = per-output
    // ratio here, not a one-time cost) since it's the same "need/have" idea.
    // Empty for a raw producer with no inputs at all.
    std::vector<BuildMaterialInfo> inputs;
    double nextCost = 0.0;
    bool locked = false;
    int workers = 0;        // hired workers boosting just this business (see Game::tryHireWorker)
    double workerCost = 0.0; // cost of the next worker, 0 if already at the cap
    std::string cropId;      // only meaningful for "farm" -- its currently-active CropType id
    bool seasonBonusActive = false; // only meaningful for "farm" -- true if cropId's favorite season is current
    // 2026-08-11 ("在每个商店的右下角加一个显示当前数量" / the manual
    // pause request -- see Business::autoProcessPaused's own comment):
    // `outputStock` is the live warehouse stock of this business's own
    // output good (0 for Storefront, whose output is cash not a good) --
    // a UI reading BusinessInfo no longer has to separately look the good
    // up in goodInfos() just to show "how much have I actually made".
    // `paused` mirrors Business::autoProcessPaused for the same UI.
    double outputStock = 0.0;
    bool paused = false;
};

// Read-only snapshot of a business's first-build construction state (see
// Business::constructionDaysRemaining / BusinessManager::requiresConstruction).
// Returned even for businesses that don't need construction at all or are
// already built -- `requiresConstruction` false means the rest of the
// fields aren't meaningful (see Game::businessConstructionInfo).
struct ConstructionInfo {
    bool requiresConstruction = false;
    bool inProgress = false; // true once construction has actually started (daysRemaining > 0)
    double daysRemaining = 0.0;
    int totalDays = 0;
    std::vector<BuildMaterialInfo> materials; // only meaningful pre-construction (level 0, not yet started)
};

// One good the player can eat (see Game::foodOptions/tryEat and Game.cpp's
// kFoodDefs table) -- `stock` is filled in live from the market, everything
// else is that food's fixed definition.
struct FoodOption {
    std::string goodId;
    double hungerRestorePerUnit = 0.0;
    double stock = 0.0;
};

// One selectable room tier at the Inn (see Game::innTiers/trySleep) --
// `nameKey` is the room's own localized name (distinct from the "sleep"
// building's own floating label, now "Inn"), `cost` is what a single
// night in that room costs, and `extraWellRestedHours`/`extraWellRestedBonus`
// stack ON TOP OF (not instead of) whatever the player's own permanent
// Bedroom upgrade level already grants -- the room tier is a repeatable,
// optional splurge; the Bedroom is a one-time investment in your own bed.
struct InnTierInfo {
    std::string nameKey;
    double cost = 0.0;
    double extraWellRestedHours = 0.0;
    double extraWellRestedBonus = 0.0;
};

// Read-only snapshot of the Storefront's auto-sell config (see
// Business::autoSellGoodId/autoSellThreshold, Game::storefrontAutoSellInfo,
// and GameWorld::drawAutoSellOverlay). `capacityPerDay` is already resolved
// from the Storefront's current level (see Game::autoSellCapacityForLevel)
// so the UI doesn't need to duplicate that table.
struct StorefrontAutoSellInfo {
    bool built = false;          // false if the Storefront itself isn't built yet (level 0)
    int level = 0;
    std::string goodId;          // empty = auto-sell not configured/disabled
    double threshold = 0.0;
    double capacityPerDay = 0.0; // units of goodId it can sell per in-game day once triggered
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

// What happened while the player was away, captured by startSession()'s
// offline catch-up so a graphical front end (GameWorld's Welcome Back
// overlay) can show it instead of it only ever going to std::cout, which is
// invisible once a GUI window covers the console. `elapsedSeconds <= 0`
// means either this is a brand new save or no real time passed since the
// last tick (nothing to report).
struct WelcomeBackInfo {
    long long elapsedSeconds = 0;
    std::string elapsedFormatted; // e.g. "2d 3h 15m 42s" -- already formatted, see formatDuration in Game.cpp
    double idleEarnings = 0.0;
    std::vector<std::string> eventLog; // same lines printEventLog would otherwise print to std::cout
    // True if offline neglect (untreated illness or starvation) would have
    // been fatal during this catch-up -- see Game::kOfflineSafetyMarginDays.
    // The character survives regardless (offline death is capped, not
    // prevented outright: unlike a live sleep/fast-forward, the player had no
    // chance to react while the app was closed), but comes back one bad day
    // away from actually dying, so this needs to be surfaced loudly rather
    // than buried in the eventLog.
    bool nearFatalWhileAway = false;
    // 2026-08-12 ("那个季节变化能不能做成就是用户挂机很久了回来也会显示"
    // -- can the season-change transition also show for a player who was
    // away a long time, not just live in-session): true if currentSeason()
    // at the end of this catch-up differs from what it was right before it
    // ran (see startSession()) -- an offline stretch long enough to cross a
    // season boundary silently used to just never show the same color-wash
    // transition/particle switch a LIVE season change gets (GameWorld's own
    // tick loop only compares consecutive frames, and by the time GameWorld
    // even exists this catch-up has already happened). `seasonBecame` is
    // whichever season it ended up as -- GameWorld plays the same
    // transition it would for a live change, targeting this. Naturally
    // fires at most once per real season boundary (whenever this next
    // differs from the season recorded at the START of the next
    // startSession() call), which is the "once a year per season" the
    // request asked for -- nothing extra needs tracking/persisting for that
    // on top of what currentSeason() already derives from ageDays.
    bool seasonChangedWhileAway = false;
    Season seasonBecame = Season::Spring;
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
    // tryEat only -- true if this meal was a different good than the previous
    // one eaten this life, earning the "varied diet" hunger-restore bonus (see
    // Game::kVarietyBonusMultiplier). Lets the UI call out the bonus instead of
    // the player only ever noticing the restored amount was a bit higher.
    bool varietyBonus = false;
};

// Result of any time-advancing action that could cause death (background
// ticking, sleeping, fast-forwarding). `deathMessage` is already the
// localized cause string (see Game::simulateElapsed), ready to show as-is.
struct TickOutcome {
    // False only for trySleep() rejecting an unaffordable room tier (no
    // other TickOutcome producer ever fails outright, so this defaults true
    // and every existing call site is unaffected). When false, nothing else
    // in this struct is meaningful -- no day passed, no money moved.
    bool success = true;
    bool died = false;
    std::string deathMessage;
    int generation = 0;
    // Flavor/disaster event lines rolled during this tick (price surges/
    // crashes, windfalls, theft, spoilage, market-wide crashes/booms,
    // warehouse disasters -- see EventSystem::roll). Game::tickBackground()
    // used to only ever std::cout these (fine for the console UI, invisible
    // once the SFML window covers it) -- GameWorld now queues them as toasts
    // instead, the same fix already applied to the offline "Welcome Back"
    // report (see WelcomeBackInfo).
    std::vector<std::string> eventLog;
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
    // Whatever startSession() found on this most recent call (empty/zeroed
    // if nothing happened -- see WelcomeBackInfo). GameWorld reads this once
    // right after construction to decide whether to open its Welcome Back
    // overlay.
    const WelcomeBackInfo& lastWelcomeBack() const { return lastWelcomeBack_; }

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
    // Whole in-game days lived so far this generation (life_.ageDays starts
    // at kStartingAgeYears * kDaysPerYear and advances by however much real
    // time trySleep()/tryFastForward()/tickToNow() actually simulated --
    // see Life::advanceReal; trySleep() specifically now always lands
    // exactly on next-day 8am, see its own comment, not a flat +1.0). Exposed
    // as its own HUD figure since ageYears() alone (1 decimal place) barely
    // visibly moves for a handful of days, making a few sleeps look like
    // "nothing happened" even though the underlying math already advanced
    // correctly.
    int totalDaysElapsed() const { return static_cast<int>(life_.ageDays); }
    // Current in-game clock hour (2026-08-07, added alongside trySleep()'s
    // next-day-8am fix so the HUD can actually show *why* sleeping moved
    // time forward) -- the fractional part of life_.ageDays IS the time of
    // day (0.0 = midnight, 0.5 = noon), scaled to 0..24. This is also now
    // what GameWorld's own dayNightTint()/nightFactor() key their sky off
    // of directly (a same-day follow-up removed GameWorld's old, purely
    // cosmetic real-time sky timer once it was clearly the wrong clock to
    // be reading -- see GameWorld.h's own comment) -- so this one getter is
    // the single source of truth for "what time is it" everywhere now.
    double timeOfDayHours() const {
        // Truncation instead of std::floor (avoids a <cmath> include here) --
        // safe since life_.ageDays is always positive (starts at
        // kStartingAgeYears * kDaysPerYear, only ever increases).
        double frac = life_.ageDays - static_cast<double>(static_cast<long long>(life_.ageDays));
        return frac * 24.0;
    }
    double energy() const { return life_.energy; }
    double hunger() const { return life_.hunger; }
    bool isSick() const { return life_.sick; }
    double sickDays() const { return life_.sickDays; }
    double starvingDays() const { return life_.starvingDays; }
    double sicknessDeathDays() const { return Life::kSicknessDeathDays; }
    double starvationDeathDays() const { return Life::kStarvationDeathDays; }
    double ageEfficiency() const { return life_.ageEfficiency(); }
    double lifeProductionMultiplier() const { return life_.productionMultiplier(); }

    // ---- "Well-rested": a small, temporary production bonus granted every
    // time trySleep()/menuSleep() successfully restores energy to 100 -- makes
    // sleeping (previously just a chore to undo the energy penalty) an
    // actively good thing to do before a grinding session, not merely neutral.
    // Decays with in-game time in simulateElapsed, same clock as everything
    // else here; not persisted across save/load, same as dailyYieldVariance_. ----
    static constexpr double kWellRestedHours = 8.0;          // in-game hours the bonus lasts
    static constexpr double kWellRestedProductionBonus = 0.10; // +10% production while active
    double wellRestedHoursRemaining() const { return wellRestedHoursRemaining_; }

    // ---- Inn room tiers (2026-08-07): the "sleep" building is now the Inn,
    // not a free/instant bedroom -- every night costs whichever room tier
    // the player picks, cheapest to priciest, and a pricier room grants a
    // longer/stronger well-rested buff on top of the Bedroom upgrade's own
    // baseline (see trySleep). Kept deliberately affordable end to end
    // (cheapest tier is a token fee, priciest still undercuts a single
    // Doctor visit) -- this is meant to be a flavorful daily habit, not a
    // toll gate on the core sleep loop. Fixed table (not player-scaling),
    // same convention as kFoodDefs. ----
    static constexpr int kInnTierCount = 5;
    std::vector<InnTierInfo> innTiers() const;

    // ---- Bedroom: a cash-only comfort upgrade reached from the Sleep menu
    // (see menuSleep()/GameWorld::drawSleepOverlay) -- makes the well-rested
    // buff above longer and stronger, and gives a small standing cut to how
    // often illness strikes at all (a comfier bed -> a healthier sleep).
    // Capped like the Legacy tracks rather than scaling forever -- meant to
    // feel like furnishing one room, not another endless grind. Resets to 0
    // on death/rebirth, same as warehouseLevel_ (an heir keeps the cash and a
    // business foothold, not the fixtures). ----
    static constexpr int kBedroomMaxLevel = 5;
    static constexpr double kBedroomExtraHoursPerLevel = 1.5;          // added to kWellRestedHours, per level
    static constexpr double kBedroomExtraBonusPerLevel = 0.02;         // added to kWellRestedProductionBonus, per level
    static constexpr double kBedroomSicknessReductionPerLevel = 0.05; // relative cut to sickness chance, per level
    int bedroomLevel() const { return bedroomLevel_; }
    double bedroomNextCost() const;
    // How long/strong the well-rested buff is at the *current* bedroom level
    // -- what trySleep()/menuSleep() actually grant on waking up.
    double bedroomWellRestedHours() const { return kWellRestedHours + static_cast<double>(bedroomLevel_) * kBedroomExtraHoursPerLevel; }
    double bedroomWellRestedBonus() const { return kWellRestedProductionBonus + static_cast<double>(bedroomLevel_) * kBedroomExtraBonusPerLevel; }
    ActionResult tryUpgradeBedroom();

    // How close to the fatal threshold offline neglect is allowed to land the
    // character (see simulateElapsed's `allowDeath` param) -- half an
    // in-game day of margin, close enough that reopening the app still feels
    // urgent rather than risk-free, far enough that it's never a hair-trigger
    // false alarm from float rounding.
    static constexpr double kOfflineSafetyMarginDays = 0.5;

    // ---- Pre-sleep forecast (see Life::predictedHungerAfter/predictedStarvingDaysAfter):
    // what hunger/starvingDays would look like after committing to a full
    // in-game day of sleep, given today's season's hunger-drain multiplier --
    // lets menuSleep/GameWorld's Sleep overlay warn *before* the player sleeps
    // through their own starvation instead of only finding out afterward. ----
    double predictedHungerAfterSleep() const;
    double predictedStarvingDaysAfterSleep() const;

    // Every good the player can eat (see Game.cpp's kFoodDefs), each with its
    // own hunger-restore value and current warehouse stock -- wheat is
    // always first (cheapest/most available) and everything else is an
    // actual prepared dish worth more per unit. Replaces the old
    // wheat-only hungerRestorePerUnit() accessor.
    std::vector<FoodOption> foodOptions() const;

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

    // ---- First-build construction (see Business::constructionDaysRemaining
    // and BusinessManager::requiresConstruction/buildMaterialsFor/
    // buildDaysFor). Only meaningful for a business still at level 0 that
    // requiresConstruction() -- everything else (the 4 free starters, or any
    // business already at level >= 1) keeps using tryUpgradeBusinessBulk
    // above untouched. ----
    // Read-only snapshot for a UI: whether this id needs construction at
    // all, whether it's already under way, and (if not yet started) the
    // material shopping list with current warehouse stock alongside it.
    ConstructionInfo businessConstructionInfo(const std::string& businessId) const;

    // ---- Storefront auto-sell (see Business::autoSellGoodId/
    // autoSellThreshold and simulateElapsed's Pass 3). Continuously sells a
    // chosen good from the warehouse once its market price reaches a
    // player-set threshold -- capacity (units/in-game day) scales with the
    // Storefront's own level, 3 at level 1 doubling up to 48 at level 5,
    // capped there even if the Storefront is leveled further for its normal
    // cash income (see kAutoSellMaxLevel/autoSellCapacityForLevel). ----
    StorefrontAutoSellInfo storefrontAutoSellInfo() const;
    // Pass an empty goodId to disable auto-sell entirely (keeps whatever
    // threshold was last set, in case they re-enable the same good later).
    ActionResult trySetStorefrontAutoSell(const std::string& goodId, double threshold);
    static constexpr int kAutoSellMaxLevel = 5;
    double autoSellCapacityForLevel(int level) const;

    // ---- Manual production pause (see Business::autoProcessPaused's own
    // comment). A paused business is skipped entirely by simulateElapsed's
    // Pass 1/2 -- it neither consumes its input nor produces its output --
    // while everything else about it (level, workers, upgrades) is
    // untouched. Meant for freeing up a shared raw good for something else
    // (e.g. pausing the Sawmill so Wood stops being drawn into Planks)
    // rather than as a way to "save" production for later -- paused time
    // just doesn't produce, it isn't banked. ----
    ActionResult trySetBusinessPaused(const std::string& businessId, bool paused);
    // Spends the cash cost (same BusinessType::baseCost as a normal first
    // level) plus every required material from the warehouse, then starts
    // the countdown -- level stays 0 until simulateElapsed ticks it to
    // completion. Fails if already built/under construction, locked, or
    // short on cash/materials (messageKey says which).
    ActionResult tryStartConstruction(const std::string& businessId);
    // Refunds half of whatever was already spent (cash + each material) and
    // resets constructionDaysRemaining to 0, back to an empty plot. Fails if
    // nothing is actually under construction there.
    ActionResult tryCancelConstruction(const std::string& businessId);
    // Newly-completed construction ids since the last drain (see
    // simulateElapsed's Pass 0) -- for a UI to toast "X built!" even when the
    // player isn't standing next to it. Not persisted; drains to empty.
    std::vector<std::string> drainCompletedConstructions();

    // ---- Port -> Fisher's Isle (see GameWorld's Zone 7 and the Port's
    // Sail/Commission Ship buttons in drawBusinessesOverlay). The ship is a
    // one-time purchase (not consumed per trip); once built, sailing back
    // and forth is free and unlimited. ----
    bool hasIslandShip() const { return hasIslandShip_; }
    bool hasVisitedIsland() const { return hasVisitedIsland_; }
    static constexpr double kShipCommissionCash = 1000.0;
    static constexpr double kShipCommissionShips = 2.0;
    // Consumes kShipCommissionShips worth of the "ships" good plus
    // kShipCommissionCash -- fails if the Port isn't built yet, a ship's
    // already been commissioned, or the player is short on cash/ships.
    ActionResult tryCommissionShip();
    // Called once by GameWorld right after the player arrives on the isle
    // (see the Sail button) -- drives the "set_sail" achievement.
    void markIslandVisited() { hasVisitedIsland_ = true; }

    // ---- Achievement unlock popups (see Achievements.h/AchievementManager::
    // checkAndUnlock) -- ids newly unlocked since the last drain, for a
    // Minecraft-style toast (see GameWorld::drawAchievementToast). Not
    // persisted; an already-unlocked achievement from a loaded save never
    // ends up in here (see checkAchievements(), which only appends what
    // checkAndUnlock itself just flipped). ----
    std::vector<std::string> drainNewlyUnlockedAchievements();

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

    // ---- Daily yield variance (see simulateElapsed): a small once-per-day
    // reroll applied to every business's output so two otherwise-identical
    // days don't produce the exact same amount. Kept narrow -- flavor, not
    // a real balance lever. ----
    static constexpr double kDailyYieldVarianceMin = 0.90;
    static constexpr double kDailyYieldVarianceMax = 1.15;
    // ---- Overall economy pace: a flat multiplier on every business's
    // output (goods and direct cash alike, see simulateElapsed's `mult`) --
    // paired with the tier-based construction cost formula (see
    // BusinessManager's kTier1Cost/kTier2Cost/kTier3Cost in Business.cpp) so
    // raising prices doesn't get compounded by an unchanged income rate. ----
    static constexpr double kEconomyPaceMultiplier = 0.7;
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
    // Cheap single-field lookup for the world view's building label (see
    // GameWorld::drawBuilding) -- avoids copying the whole businessInfos()
    // vector just to read one field every frame.
    std::string farmCropId() const {
        const Business* farm = businessManager_.find("farm");
        return farm ? farm->cropId : std::string("wheat");
    }

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
    // Silently clamps qty down to however much is actually useful (never
    // eats past 100 hunger, wasting food for zero benefit) -- same
    // "clamped, not rejected" philosophy tryBuyGood already uses for
    // warehouse room. result.amount is the actual quantity eaten, which the
    // caller should report instead of whatever qty it originally asked for.
    ActionResult tryEat(const std::string& goodId, double qty);
    ActionResult tryVisitDoctor();
    // `tier` indexes into innTiers() (clamped into range) -- charges that
    // tier's cost up front (see innTiers' own comment on why before, not
    // after, the night's events), same "clamped, not rejected" convention
    // as tryEat. `TickOutcome::success` is false (with no other side
    // effects at all -- money untouched, no day passes) when the player
    // can't afford the chosen tier.
    TickOutcome trySleep(int tier);
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

    // ---- "Varied diet" (see tryEat): eating a different food than whatever
    // was eaten last restores extra hunger, a small nudge to actually use the
    // higher-tier foods a production chain unlocks instead of just living off
    // whichever one is cheapest. Not a penalty for repeating -- repeating just
    // forfeits the bonus, same "carrot, not stick" spirit as kWellRestedHours. ----
    static constexpr double kVarietyBonusMultiplier = 1.15;

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
    static constexpr double kBedroomBaseCost = 400.0;
    static constexpr double kBedroomCostGrowth = 1.8; // steeper than Warehouse -- capped at kBedroomMaxLevel, so it doesn't need to stay affordable forever
    static constexpr double kBaseStorageCap = 2000.0;      // per good, before any warehouse levels
    static constexpr double kStorageCapPerLevel = 1500.0;

    std::string saveFilePath_ = "save.txt";
    int difficulty_ = 1; // 0=Easy, 1=Normal, 2=Hardcore
    double money_ = 200.0;
    double bankBalance_ = 0.0;
    int warehouseLevel_ = 0;
    int bedroomLevel_ = 0;
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
    // Port -> Fisher's Isle (see tryCommissionShip/markIslandVisited above) --
    // reset each generation like the rest of this life's progress, since the
    // Port itself resets to an unbuilt plot along with every other business.
    bool hasIslandShip_ = false;
    bool hasVisitedIsland_ = false;

    // ---- Daily yield variance (see kDailyYieldVarianceMin/Max and
    // simulateElapsed): rerolled once per in-game day so production isn't
    // perfectly identical every day. Deliberately not saved -- it just
    // rerolls itself the next time simulateElapsed notices the day changed. ----
    double dailyYieldVariance_ = 1.0;
    long long dailyYieldVarianceDay_ = -1;

    // ---- Market price-noise carry-over (2026-08-10 bugfix, "价钱会一直跳
    // 动...为什么我这个版本好像没有了" -- price jitter seemed to have
    // stopped happening at all during live play): simulateElapsed's own
    // `seconds` param used to compute market steps as `seconds /
    // kMarketStepSeconds` (5) from THAT CALL ALONE, with no memory between
    // calls -- fine for a single big offline/sleep catch-up, but Game::
    // tickBackground (called every render frame) passes real elapsed
    // seconds since the last call, which is almost always exactly 1 (whole-
    // second-resolution nowEpoch(), checked far more than once per second)
    // -- `1 / 5 == 0` in integer division, so live play never accumulated
    // enough in a single call to ever take a price-noise step, and the
    // leftover second was silently discarded every single tick instead of
    // carrying toward the next one. This accumulator fixes that by
    // tracking leftover real seconds across calls instead of deriving
    // steps from one call's own `seconds` in isolation -- see its use in
    // simulateElapsed. Deliberately not saved, same as dailyYieldVariance_
    // above -- losing a few seconds of partial progress across a save/load
    // is harmless. ----
    long long marketStepRemainderSeconds_ = 0;

    // ---- Well-rested buff countdown (see kWellRestedHours/kWellRestedProductionBonus)
    // and last-eaten-food tracking (see kVarietyBonusMultiplier) -- both transient
    // flavor state, deliberately not saved, same as dailyYieldVariance_ above. ----
    double wellRestedHoursRemaining_ = 0.0;
    // The well-rested bonus % actually in effect while wellRestedHoursRemaining_
    // is counting down -- set once per trySleep() call to bedroomWellRestedBonus()
    // plus whichever Inn tier was chosen that night (see innTiers()), instead of
    // recomputing bedroomWellRestedBonus() alone at consumption time, since the
    // tier's own extra bonus only applies for the buff IT granted, not
    // retroactively to a buff a cheaper night already started.
    double activeWellRestedBonus_ = 0.0;
    std::string lastFoodEatenId_;

    // ---- Transient UI event queues (see drainNewlyUnlockedAchievements/
    // drainCompletedConstructions) -- never saved, just drained by GameWorld
    // once per frame to drive toasts. ----
    std::vector<std::string> pendingAchievementPopups_;
    std::vector<std::string> completedConstructionEvents_;
    long long lastTickEpoch_ = 0;
    WelcomeBackInfo lastWelcomeBack_; // populated by startSession(), read once by GameWorld -- see lastWelcomeBack()
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
    //
    // `allowDeath = false` is the offline safety net (see startSession(),
    // the only caller that ever passes it): neglect still accumulates
    // exactly as normal, but sickDays/starvingDays are clamped just short of
    // the fatal threshold instead of actually killing the character, since
    // -- unlike a live sleep/fast-forward the player explicitly chose --
    // they had no chance to react while the app was closed. Every other
    // caller (live ticking, sleeping, fast-forwarding) leaves this true, so
    // neglect during an active session is exactly as lethal as it always was.
    //
    // `grantProduction = false` (2026-08-10 bugfix, "睡觉了,用户可以直接领
    // 取满的仓库...睡觉只会影响时间和事件发生就ok了,不要帮忙加货" -- sleeping
    // shouldn't hand the player a full warehouse, just advance time and let
    // events happen) skips Pass 1/Pass 2 below (raw production and
    // input->output processing) entirely -- construction progress, market
    // price noise, random events, upkeep, and the life clock all still run
    // exactly as normal. Only trySleep() passes false; every other caller
    // (live ticking, fast-forwarding, offline catch-up) leaves this true,
    // so sleep is now the one time skip that doesn't also silently run
    // every business at max output for its whole duration.
    bool simulateElapsed(long long seconds, std::vector<std::string>& eventLog, double weatherMult = 1.0, bool allowDeath = true, bool grantProduction = true);

    // Ticks the clock forward to "now" based on real elapsed time. Returns
    // true if the character died during this call.
    bool tickToNow(std::vector<std::string>& eventLog, double weatherMult = 1.0);

    // Announces death, resets the run to a fresh character (achievements persist).
    void handleDeath();

    void checkAchievements();
    void printEventLog(const std::vector<std::string>& log) const;

    // Winter's hunger-drain multiplier (softened by the Legacy season track),
    // 1.0 every other season -- factored out of simulateElapsed so
    // predictedHungerAfterSleep()/predictedStarvingDaysAfterSleep() can use
    // the exact same number instead of drifting out of sync with it.
    double seasonHungerDrainMultiplier(Season season) const;

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
