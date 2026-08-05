#include "Market.h"
#include "Format.h"
#include "Localization.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <iomanip>

namespace {
    constexpr double kReversionRate = 0.03; // pull toward base price per step
    constexpr double kPriceFloorFrac = 0.15; // price never drops below this fraction of base
    // Trading nudges the price by kImpactPerUnit per sqrt(quantity) rather than
    // per raw unit, so bulk trades (hundreds/thousands of units, common once
    // idle production ramps up) don't instantly crater or spike the price —
    // impact still grows with size, just sub-linearly instead of blowing up.
    constexpr double kImpactPerUnit = 0.002;

    // Production/consumption price pressure: the fraction of (current stock +
    // a baseline) that this tick's amount represents, capped at 1 so a single
    // very long fast-forward/offline catch-up can't overshoot past the max
    // move below. The baseline (kPressurePivot units) keeps early-game
    // stockpiles (near zero) from swinging wildly on the very first tick, and
    // naturally self-limits once stock is already huge -- the same amount
    // barely moves price further once it's a small slice of a big pile.
    constexpr double kProductionPriceImpact = 0.35;  // max single-tick fractional price drop
    constexpr double kConsumptionPriceImpact = 0.35; // max single-tick fractional price rise
    constexpr double kPressurePivot = 50.0;
}

Market::Market() : rng_(std::random_device{}()) {
    // Tier 1 raw goods.
    goods_.push_back(Good{ "wheat", "Wheat",    4.0,  4.0,  0.05 });
    goods_.push_back(Good{ "ore",   "Iron Ore", 18.0, 18.0, 0.07 });
    goods_.push_back(Good{ "wood",  "Wood",     3.0,  3.0,  0.05 });
    goods_.push_back(Good{ "stone", "Stone",    5.0,  5.0,  0.05 });

    // Tier 2 processed goods.
    goods_.push_back(Good{ "bread",      "Bread",      12.0, 12.0, 0.06 });
    goods_.push_back(Good{ "iron_ingot", "Iron Ingot", 40.0, 40.0, 0.07 });
    goods_.push_back(Good{ "planks",     "Planks",     8.0,  8.0,  0.05 });
    goods_.push_back(Good{ "bricks",     "Bricks",     14.0, 14.0, 0.05 });
    goods_.push_back(Good{ "gem",        "Gemstone",   120.0, 120.0, 0.10 });

    // Tier 3 advanced goods.
    goods_.push_back(Good{ "tools",     "Tools",     150.0, 150.0, 0.09 });
    goods_.push_back(Good{ "furniture", "Furniture", 100.0, 100.0, 0.08 });

    // Sheep/textile and fishing chains.
    goods_.push_back(Good{ "wool",        "Wool",        6.0,  6.0,  0.05 });
    goods_.push_back(Good{ "fish",        "Fish",        7.0,  7.0,  0.06 });
    goods_.push_back(Good{ "cloth",       "Cloth",       18.0, 18.0, 0.06 });
    goods_.push_back(Good{ "smoked_fish", "Smoked Fish", 21.0, 21.0, 0.07 });
    goods_.push_back(Good{ "clothing",    "Clothing",    80.0, 80.0, 0.08 });

    // Orchard/apothecary/goldsmithing/vineyard chains (Valley District).
    goods_.push_back(Good{ "fruit",     "Fruit",     5.0,   5.0,   0.05 });
    goods_.push_back(Good{ "preserves", "Preserves", 15.0,  15.0,  0.06 });
    goods_.push_back(Good{ "herbs",     "Herbs",     8.0,   8.0,   0.06 });
    goods_.push_back(Good{ "medicine",  "Medicine",  24.0,  24.0,  0.07 });
    goods_.push_back(Good{ "elixir",    "Elixir",    110.0, 110.0, 0.10 });
    goods_.push_back(Good{ "gold_ore",  "Gold Ore",  35.0,  35.0,  0.08 });
    goods_.push_back(Good{ "gold_bars", "Gold Bars", 95.0,  95.0,  0.09 });
    goods_.push_back(Good{ "jewelry",   "Jewelry",   220.0, 220.0, 0.11 });
    goods_.push_back(Good{ "grapes",    "Grapes",    6.0,   6.0,   0.05 });
    goods_.push_back(Good{ "wine",      "Wine",      20.0,  20.0,  0.07 });

    // Harbor District: salt/pearl raw goods, plus the shipyard/cannery chains.
    goods_.push_back(Good{ "salt",           "Salt",          6.0,   6.0,   0.05 });
    goods_.push_back(Good{ "pearls",         "Pearls",        40.0,  40.0,  0.09 });
    goods_.push_back(Good{ "ships",          "Ships",         140.0, 140.0, 0.08 });
    goods_.push_back(Good{ "canned_fish",    "Canned Fish",   22.0,  22.0,  0.07 });
    goods_.push_back(Good{ "pearl_jewelry",  "Pearl Jewelry", 180.0, 180.0, 0.10 });

    // Highlands District: dairy, apiary, trapping, and tea/flax chains.
    goods_.push_back(Good{ "milk",       "Milk",       5.0,  5.0,  0.05 });
    goods_.push_back(Good{ "cheese",     "Cheese",     16.0, 16.0, 0.06 });
    goods_.push_back(Good{ "honey",      "Honey",      9.0,  9.0,  0.06 });
    goods_.push_back(Good{ "mead",       "Mead",       19.0, 19.0, 0.07 });
    goods_.push_back(Good{ "pelts",      "Pelts",      7.0,  7.0,  0.06 });
    goods_.push_back(Good{ "leather",    "Leather",    17.0, 17.0, 0.06 });
    goods_.push_back(Good{ "tea_leaves", "Tea Leaves", 8.0,  8.0,  0.06 });
    goods_.push_back(Good{ "tea",        "Tea",        18.0, 18.0, 0.07 });
    goods_.push_back(Good{ "flax",       "Flax",       4.0,  4.0,  0.05 });
    goods_.push_back(Good{ "linen",      "Linen",      15.0, 15.0, 0.06 });

    // Farm's selectable crops (see Business.h's CropType) + their processors.
    goods_.push_back(Good{ "strawberry",           "Strawberry",           6.0,  6.0,  0.05 });
    goods_.push_back(Good{ "corn",                 "Corn",                 4.0,  4.0,  0.05 });
    goods_.push_back(Good{ "watermelon",           "Watermelon",           5.0,  5.0,  0.05 });
    goods_.push_back(Good{ "pumpkin",              "Pumpkin",              5.0,  5.0,  0.05 });
    goods_.push_back(Good{ "sweetpotato",          "Sweet Potato",         5.0,  5.0,  0.05 });
    goods_.push_back(Good{ "cabbage",              "Cabbage",              4.0,  4.0,  0.05 });
    goods_.push_back(Good{ "strawberry_jam",       "Strawberry Jam",       16.0, 16.0, 0.06 });
    goods_.push_back(Good{ "popcorn",              "Popcorn",              13.0, 13.0, 0.06 });
    goods_.push_back(Good{ "watermelon_juice",     "Watermelon Juice",     14.0, 14.0, 0.06 });
    goods_.push_back(Good{ "pumpkin_pie",          "Pumpkin Pie",          17.0, 17.0, 0.06 });
    goods_.push_back(Good{ "roasted_sweet_potato", "Roasted Sweet Potato", 13.0, 13.0, 0.06 });
    goods_.push_back(Good{ "sauerkraut",           "Sauerkraut",           12.0, 12.0, 0.06 });
}

Good* Market::find(const std::string& id) {
    for (auto& g : goods_) {
        if (g.id == id) return &g;
    }
    return nullptr;
}

void Market::advance(int steps) {
    if (steps <= 0) return;
    std::uniform_real_distribution<double> noise(-1.0, 1.0);
    for (auto& g : goods_) {
        double floor = g.basePrice * kPriceFloorFrac;
        for (int i = 0; i < steps; ++i) {
            double reversion = kReversionRate * (g.basePrice - g.price);
            double shock = noise(rng_) * g.volatility * g.basePrice;
            g.price += reversion + shock;
            if (g.price < floor) g.price = floor;
        }
    }
}

bool Market::buy(const std::string& id, double quantity, double& money) {
    Good* g = find(id);
    if (!g || quantity <= 0) return false;
    double cost = g->price * quantity;
    if (cost > money) return false;

    money -= cost;
    g->stock += quantity;
    g->price *= (1.0 + kImpactPerUnit * std::sqrt(quantity));
    return true;
}

bool Market::sell(const std::string& id, double quantity, double& money) {
    Good* g = find(id);
    if (!g || quantity <= 0 || quantity > g->stock + 1e-9) return false;
    double revenue = g->price * quantity;

    g->stock -= quantity;
    money += revenue;
    double floor = g->basePrice * kPriceFloorFrac;
    g->price *= (1.0 - kImpactPerUnit * std::sqrt(quantity));
    if (g->price < floor) g->price = floor;
    return true;
}

void Market::applyProductionPressure(const std::string& id, double amountAdded) {
    if (amountAdded <= 0.0) return;
    Good* g = find(id);
    if (!g) return;
    double frac = std::min(1.0, amountAdded / (g->stock + kPressurePivot));
    g->price *= (1.0 - kProductionPriceImpact * frac);
    double floor = g->basePrice * kPriceFloorFrac;
    if (g->price < floor) g->price = floor;
}

void Market::applyConsumptionPressure(const std::string& id, double amountConsumed) {
    if (amountConsumed <= 0.0) return;
    Good* g = find(id);
    if (!g) return;
    double frac = std::min(1.0, amountConsumed / (g->stock + kPressurePivot));
    g->price *= (1.0 + kConsumptionPriceImpact * frac);
}

void Market::print() const {
    std::cout << std::left << std::setw(4) << Localization::t("col_hash")
        << std::setw(14) << Localization::t("col_good")
        << std::right << std::setw(12) << Localization::t("col_price")
        << std::setw(14) << Localization::t("col_hold") << "\n";
    int idx = 1;
    for (const auto& g : goods_) {
        std::cout << std::left << std::setw(4) << idx++
            << std::setw(14) << Localization::t(g.id)
            << std::right << std::setw(12) << formatNumber(g.price)
            << std::setw(14) << formatNumber(g.stock) << "\n";
    }
}
