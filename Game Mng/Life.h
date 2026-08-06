#pragma once
#include <algorithm>

// Tracks the player-character's passage of time: in-game age, two needs
// (energy from sleep, hunger from food) that cause a soft production penalty
// when neglected, and two conditions (illness, starvation) that are genuinely
// fatal if neglected for too long — unlike age, which is *not* a death
// condition here, just a milestone (see the Centenarian achievement).
//
// Game time runs faster than real time: kTimeCompression game-seconds pass
// per real second, calibrated so 6 real minutes = 1 in-game hour, and 144
// real minutes = 1 in-game day.
class Life {
public:
    static constexpr double kTimeCompression = 10.0;
    static constexpr double kGameSecondsPerDay = 86400.0;
    // Calendar: 1 in-game month per season (see Game::kGameDaysPerSeason,
    // derived from kDaysPerYear below), 4 seasons per year -> a 4-month,
    // 120-day year instead of a real-world 12-month one, so a character's
    // whole life plays out over less real playtime -- reaching kMaxAgeYears
    // (100) no longer takes an unreasonable amount of sleeping/fast-forwarding.
    // kDaysPerYear is the one source of truth both this class's ageYears()
    // and Game's season cycle read from.
    static constexpr double kDaysPerMonth = 30.0;
    static constexpr double kMonthsPerYear = 4.0;
    static constexpr double kDaysPerYear = kDaysPerMonth * kMonthsPerYear; // 120 -- a season is exactly one 30-day month
    static constexpr double kMaxAgeYears = 100.0;
    // Wheat's own hunger-restore value specifically -- see Game::foodOptions/
    // tryEat for the full per-good table (bread, cake, ... each restore
    // more). Wheat is the cheapest/easiest good in the game to have lying
    // around (any Farm produces it automatically), so it's deliberately the
    // worst "food" -- a fallback snack, not a real meal.
    static constexpr double kHungerRestorePerUnit = 3.0;

    // New characters (and every reborn generation -- see Game::handleDeath's
    // `life_ = Life();`) start as young adults rather than newborns.
    static constexpr double kStartingAgeYears = 21.0;

    // Age-based efficiency curve: full effectiveness during the prime working
    // years, tapering gently below/above that toward a floor — never crippling,
    // just a flavor reminder that a toddler or a 90-year-old works a bit slower.
    static constexpr double kPrimeAgeStart = 12.0;
    static constexpr double kPrimeAgeEnd = 65.0;
    static constexpr double kAgeEfficiencyFloor = 0.6;

    // Illness onset chance (rolled continuously, scaled to elapsed game-days)
    // and the fatal thresholds for prolonged neglect.
    static constexpr double kSicknessChancePerGameDay = 0.02;
    static constexpr double kSicknessDeathDays = 10.0;    // untreated illness this long is fatal
    static constexpr double kStarvationDeathDays = 5.0;   // this many consecutive days at 0 hunger is fatal

    double ageDays = kStartingAgeYears * kDaysPerYear;
    double energy = 100.0; // 0-100, drains while awake, restored by sleeping
    double hunger = 100.0; // 0-100 ("fullness"), drains over time, restored by eating

    bool sick = false;
    double sickDays = 0.0;      // consecutive in-game days sick without treatment
    double starvingDays = 0.0;  // consecutive in-game days spent at zero hunger

    double ageYears() const { return ageDays / kDaysPerYear; }

    // Advances the life clock by `realSeconds` of elapsed real time: ages the
    // character, drains energy/hunger, and (if hunger has bottomed out)
    // accumulates starvingDays. Does not touch illness — that's driven by
    // EventSystem, which needs its own chunked loop for the random onset roll.
    // `energyDrainMult`/`hungerDrainMult` are seasonal nudges from Game::
    // simulateElapsed (e.g. summer heat drains energy faster, winter cold
    // drains hunger faster) -- left at 1.0 anywhere season doesn't apply.
    void advanceReal(double realSeconds, double energyDrainMult = 1.0, double hungerDrainMult = 1.0);

    // Combined penalty multiplier applied to all business output while energy,
    // hunger, or illness is a problem. Never zero — a soft penalty, not death.
    double productionMultiplier() const;

    // Age-based efficiency multiplier: 1.0 during the prime years, tapering
    // toward kAgeEfficiencyFloor outside that range. See kPrimeAgeStart/End.
    double ageEfficiency() const;

    // Read-only forecasts for "what would advanceReal(hours*3600, ..., hungerDrainMult)
    // do to hunger/starvingDays" without actually mutating state -- lets a UI
    // (Game::menuSleep/GameWorld's Sleep overlay) warn the player *before* they
    // commit to a long sleep that would run their hunger out overnight, instead
    // of them finding out only after waking up (or not waking up at all). Mirrors
    // advanceReal's own math exactly (including its closed-form "hours spent at
    // zero hunger" calc) so the prediction and the real outcome never disagree.
    double predictedHungerAfter(double hours, double hungerDrainMult = 1.0) const {
        double hungerDrainPerHour = kHungerDrainPerGameHour * hungerDrainMult;
        return std::max(0.0, hunger - hungerDrainPerHour * hours);
    }
    double predictedStarvingDaysAfter(double hours, double hungerDrainMult = 1.0) const {
        double hungerDrainPerHour = kHungerDrainPerGameHour * hungerDrainMult;
        double hoursToZeroHunger = (hunger > 0.0) ? hunger / hungerDrainPerHour : 0.0;
        double hoursAtZeroHunger = std::max(0.0, hours - hoursToZeroHunger);
        return (hoursAtZeroHunger > 0.0) ? (starvingDays + hoursAtZeroHunger / 24.0) : 0.0;
    }

private:
    static constexpr double kEnergyDrainPerGameHour = 100.0 / 24.0; // empty after 1 day awake
    static constexpr double kHungerDrainPerGameHour = 100.0 / 48.0; // empty after 2 days without food
};
