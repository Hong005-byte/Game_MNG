#include "Life.h"
#include <algorithm>

void Life::advanceReal(double realSeconds, double energyDrainMult, double hungerDrainMult) {
    if (realSeconds <= 0.0) return;
    double gameSeconds = realSeconds * kTimeCompression;
    double gameHours = gameSeconds / 3600.0;

    double energyDrainPerHour = kEnergyDrainPerGameHour * energyDrainMult;
    double hungerDrainPerHour = kHungerDrainPerGameHour * hungerDrainMult;

    energy = std::max(0.0, energy - energyDrainPerHour * gameHours);

    // Figure out how much of this interval was spent at zero hunger (closed
    // form, since the drain rate is constant): hunger hits the floor after
    // hoursToZeroHunger hours, and every hour beyond that counts toward
    // starvingDays. If hunger never bottoms out this interval, the streak
    // resets to zero — eating clears it, and it should only ever count
    // *consecutive* days at zero.
    double hoursToZeroHunger = (hunger > 0.0) ? hunger / hungerDrainPerHour : 0.0;
    double hoursAtZeroHunger = std::max(0.0, gameHours - hoursToZeroHunger);
    hunger = std::max(0.0, hunger - hungerDrainPerHour * gameHours);

    if (hoursAtZeroHunger > 0.0) {
        starvingDays += hoursAtZeroHunger / 24.0;
    } else {
        starvingDays = 0.0;
    }

    ageDays += gameSeconds / kGameSecondsPerDay;
}

double Life::productionMultiplier() const {
    double mult = 1.0;
    if (energy <= 0.0) mult *= 0.5;
    if (hunger <= 0.0) mult *= 0.5;
    if (sick) mult *= 0.7;
    return mult;
}

double Life::ageEfficiency() const {
    double age = ageYears();
    if (age >= kPrimeAgeStart && age <= kPrimeAgeEnd) return 1.0;

    double distance;
    double span;
    if (age < kPrimeAgeStart) {
        distance = kPrimeAgeStart - age;
        span = kPrimeAgeStart;
    } else {
        distance = age - kPrimeAgeEnd;
        span = 50.0; // decline stretched over 50 years past the prime, then floors out
    }
    double t = std::min(1.0, distance / span);
    return 1.0 - (1.0 - kAgeEfficiencyFloor) * t;
}
