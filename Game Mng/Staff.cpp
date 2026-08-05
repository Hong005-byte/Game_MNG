#include "Staff.h"
#include <cmath>

double Staff::nextCost() const {
    return baseCost * std::pow(costGrowth, static_cast<double>(level));
}

double Staff::multiplier() const {
    return 1.0 + boostPerLevel * static_cast<double>(level);
}
