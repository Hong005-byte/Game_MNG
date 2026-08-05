#pragma once
#include <string>
#include <vector>
#include <random>
#include "Market.h"
#include "Life.h"

// A pool of flavorful random events that can fire during simulated time,
// giving the idle loop some variance (market shocks, windfalls, mishaps),
// plus the illness onset/progression roll (see Life::sick).
class EventSystem {
public:
    EventSystem();

    // Rolls for events across `seconds` of simulated time, checked in a fixed
    // number of chunks whose duration scales to cover the whole span (so a
    // long offline gap still gets proportionally sampled instead of being
    // diluted by a fixed check interval). Flavor events nudge money/market;
    // illness onset/progression updates `life.sick`/`life.sickDays` directly.
    // Human-readable messages are appended to `log`. `disasterChanceMult`
    // scales just the rare economy-wide disaster roll (see Events.cpp),
    // letting difficulty settings make them more/less frequent without
    // touching the ordinary single-good flavor events. `sicknessChanceMult`
    // similarly scales just the illness-onset roll -- Game::simulateElapsed
    // passes a seasonal nudge (winter harsher, spring gentler).
    void roll(long long seconds, double& money, Market& market, Life& life, std::vector<std::string>& log, double disasterChanceMult = 1.0, double sicknessChanceMult = 1.0);

private:
    std::mt19937 rng_;
};
