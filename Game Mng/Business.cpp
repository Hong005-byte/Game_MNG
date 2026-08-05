#include "Business.h"
#include "Format.h"
#include "Localization.h"
#include <cmath>
#include <iostream>
#include <iomanip>
#include <sstream>

double Business::nextCost(const BusinessType& type) const {
    return type.baseCost * std::pow(type.costGrowth, static_cast<double>(level));
}

double Business::ratePerSecond(const BusinessType& type) const {
    return type.baseRate * static_cast<double>(level);
}

BusinessManager::BusinessManager() {
    // Tier 1: raw producers, always buildable.
    types_.push_back(BusinessType{ "farm",   "Wheat Farm",  "wheat", 50.0,  1.15, 0.50 });
    types_.push_back(BusinessType{ "mine",   "Ore Mine",    "ore",   400.0, 1.18, 0.08 });
    types_.push_back(BusinessType{ "lumber", "Lumber Camp", "wood",  120.0, 1.16, 0.30 });
    types_.push_back(BusinessType{ "quarry", "Quarry",      "stone", 250.0, 1.17, 0.20 });
    types_.push_back(BusinessType{ "storefront", "Storefront", "", 150.0, 1.16, 0.30 });

    // Tier 2: single-input processors, locked until their tier-1 source is built.
    types_.push_back(BusinessType{ "bakery",  "Bakery",       "bread",      300.0,  1.18, 0.10, "wheat", 3.0, "farm",   2 });
    types_.push_back(BusinessType{ "smelter", "Smelter",      "iron_ingot", 600.0,  1.19, 0.06, "ore",   3.0, "mine",   2 });
    types_.push_back(BusinessType{ "sawmill", "Sawmill",      "planks",     350.0,  1.18, 0.15, "wood",  2.0, "lumber", 2 });
    types_.push_back(BusinessType{ "mason",   "Mason",        "bricks",     450.0,  1.18, 0.12, "stone", 2.5, "quarry", 2 });
    types_.push_back(BusinessType{ "gemshop", "Gem Workshop", "gem",        3000.0, 1.20, 0.03, "ore",   2.0, "mine",   2 });

    // Tier 3: advanced goods, locked until their tier-2 source is built.
    types_.push_back(BusinessType{ "blacksmith", "Blacksmith", "tools",     2000.0, 1.20, 0.04, "iron_ingot", 2.0, "smelter", 3 });
    types_.push_back(BusinessType{ "carpenter",  "Carpenter",  "furniture", 1500.0, 1.19, 0.05, "planks",     2.5, "sawmill", 3 });

    // Sheep -> wool -> cloth -> clothing chain, and a shorter fishing chain.
    types_.push_back(BusinessType{ "sheep",      "Sheep Farm",   "wool",        180.0,  1.16, 0.25 });
    types_.push_back(BusinessType{ "fishing",    "Fishing Dock", "fish",        300.0,  1.17, 0.18 });
    types_.push_back(BusinessType{ "textile",    "Textile Mill", "cloth",       400.0,  1.18, 0.12, "wool", 2.0, "sheep",     2 });
    types_.push_back(BusinessType{ "smokehouse", "Smokehouse",   "smoked_fish", 500.0,  1.18, 0.10, "fish", 2.5, "fishing",   2 });
    types_.push_back(BusinessType{ "tailor",     "Tailor",       "clothing",    1800.0, 1.19, 0.045,"cloth", 2.0, "textile",  3 });

    // Valley District: orchard/apothecary, goldsmithing, and vineyard chains.
    types_.push_back(BusinessType{ "orchard",    "Orchard",     "fruit",    140.0, 1.16, 0.28 });
    types_.push_back(BusinessType{ "herbgarden", "Herb Garden", "herbs",    90.0,  1.15, 0.45 });
    types_.push_back(BusinessType{ "goldmine",   "Gold Mine",   "gold_ore", 800.0, 1.19, 0.05 });
    types_.push_back(BusinessType{ "vineyard",   "Vineyard",    "grapes",   200.0, 1.16, 0.22 });

    types_.push_back(BusinessType{ "preserve",   "Preserve",   "preserves", 350.0,  1.18, 0.10, "fruit",    2.5, "orchard",    2 });
    types_.push_back(BusinessType{ "apothecary", "Apothecary", "medicine",  450.0,  1.18, 0.08, "herbs",    3.0, "herbgarden", 2 });
    types_.push_back(BusinessType{ "goldsmith",  "Goldsmith",  "gold_bars", 1200.0, 1.19, 0.04, "gold_ore", 2.0, "goldmine",   2 });
    types_.push_back(BusinessType{ "winery",     "Winery",     "wine",      500.0,  1.18, 0.09, "grapes",   3.0, "vineyard",   2 });

    types_.push_back(BusinessType{ "alchemist", "Alchemist", "elixir",  2200.0, 1.20, 0.035, "medicine",  2.0, "apothecary", 3 });
    types_.push_back(BusinessType{ "jeweler",   "Jeweler",   "jewelry", 3500.0, 1.21, 0.025, "gold_bars", 2.0, "goldsmith",  3 });

    // Harbor District: salt/pearl raw producers, plus the fishing dock's
    // downstream shipyard/cannery chains (fishing itself lives in this zone
    // now -- see GameWorld's buildZones()).
    types_.push_back(BusinessType{ "seasalt",   "Salt Flats", "salt",   180.0, 1.16, 0.28 });
    types_.push_back(BusinessType{ "pearlfarm", "Pearl Farm", "pearls", 800.0, 1.19, 0.05 });

    types_.push_back(BusinessType{ "shipyard",     "Shipyard",     "ships",         550.0,  1.19, 0.07,  "planks", 2.5, "fishing",   2 });
    types_.push_back(BusinessType{ "cannery",      "Cannery",      "canned_fish",   500.0,  1.18, 0.10,  "fish",   2.5, "fishing",   2 });
    types_.push_back(BusinessType{ "pearlatelier", "Pearl Atelier","pearl_jewelry", 1200.0, 1.19, 0.04,  "pearls", 2.0, "pearlfarm", 2 });

    // Highlands District: dairy, apiary, trapping, and tea/flax chains.
    types_.push_back(BusinessType{ "dairyfarm", "Dairy Farm",    "milk",       160.0, 1.15, 0.30 });
    types_.push_back(BusinessType{ "beehive",   "Apiary",        "honey",      100.0, 1.15, 0.40 });
    types_.push_back(BusinessType{ "trapper",   "Trapper's Camp","pelts",      140.0, 1.16, 0.28 });
    types_.push_back(BusinessType{ "teafield",  "Tea Field",     "tea_leaves", 110.0, 1.15, 0.42 });
    types_.push_back(BusinessType{ "flaxfield", "Flax Field",    "flax",       120.0, 1.16, 0.30 });

    types_.push_back(BusinessType{ "creamery",  "Creamery",  "cheese", 300.0, 1.18, 0.10, "milk",       3.0, "dairyfarm", 2 });
    types_.push_back(BusinessType{ "meadery",   "Meadery",   "mead",   480.0, 1.18, 0.09, "honey",      2.5, "beehive",   2 });
    types_.push_back(BusinessType{ "tannery",   "Tannery",   "leather",350.0, 1.18, 0.10, "pelts",      2.5, "trapper",   2 });
    types_.push_back(BusinessType{ "teahouse",  "Tea House", "tea",    450.0, 1.18, 0.08, "tea_leaves", 3.0, "teafield",  2 });
    types_.push_back(BusinessType{ "linenmill", "Linen Mill","linen",  400.0, 1.18, 0.12, "flax",       2.0, "flaxfield", 2 });

    // Farm crop processors: the Farm itself stays a single "farm" BusinessType
    // (its displayed output good/rate depend on which CropType is active --
    // see crops_ below and Game::simulateElapsed's farm special-case), but
    // each crop's downstream processor is an ordinary tier-2 BusinessType
    // like any other, gated on "farm" being built at all (not on which crop
    // is currently planted -- a processor with no matching crop growing just
    // sits idle with nothing to consume, same as any other input-starved
    // processor already does).
    types_.push_back(BusinessType{ "jamkitchen",    "Jam Kitchen",     "strawberry_jam",       320.0, 1.18, 0.10, "strawberry", 2.5, "farm", 2 });
    types_.push_back(BusinessType{ "popcornstand",  "Popcorn Stand",   "popcorn",              300.0, 1.18, 0.11, "corn",       3.0, "farm", 2 });
    types_.push_back(BusinessType{ "juicebar",      "Juice Bar",       "watermelon_juice",     320.0, 1.18, 0.10, "watermelon", 3.0, "farm", 2 });
    types_.push_back(BusinessType{ "pieshop",       "Pie Shop",        "pumpkin_pie",          350.0, 1.18, 0.09, "pumpkin",    2.5, "farm", 2 });
    types_.push_back(BusinessType{ "roaststand",    "Roast Stand",     "roasted_sweet_potato", 300.0, 1.18, 0.10, "sweetpotato",2.5, "farm", 2 });
    types_.push_back(BusinessType{ "picklinghouse", "Pickling House",  "sauerkraut",           280.0, 1.18, 0.11, "cabbage",    3.0, "farm", 2 });

    for (const auto& t : types_) {
        businesses_.push_back(Business{ t.id, 0 });
    }

    // The Farm's selectable crops -- wheat is the default/starting one (and
    // the only one that existed before this system), each of the 6 new ones
    // pairs with the processor added just above. rateMultiplier is a small
    // flavor variance (none strictly better outside of matching its season);
    // favoriteSeason grants Game::kSeasonBonusMultiplier while active.
    crops_.push_back(CropType{ "wheat",      "Wheat",       1.00, Season::Autumn });
    crops_.push_back(CropType{ "strawberry", "Strawberry",  0.90, Season::Spring });
    crops_.push_back(CropType{ "corn",       "Corn",        1.15, Season::Summer });
    crops_.push_back(CropType{ "watermelon", "Watermelon",  0.85, Season::Summer });
    crops_.push_back(CropType{ "pumpkin",    "Pumpkin",     1.05, Season::Autumn });
    crops_.push_back(CropType{ "sweetpotato","Sweet Potato",1.10, Season::Autumn });
    crops_.push_back(CropType{ "cabbage",    "Cabbage",     0.95, Season::Winter });
}

const CropType* BusinessManager::findCrop(const std::string& id) const {
    for (const auto& c : crops_) {
        if (c.id == id) return &c;
    }
    return nullptr;
}

Business* BusinessManager::find(const std::string& id) {
    for (auto& b : businesses_) {
        if (b.typeId == id) return &b;
    }
    return nullptr;
}

const Business* BusinessManager::find(const std::string& id) const {
    for (const auto& b : businesses_) {
        if (b.typeId == id) return &b;
    }
    return nullptr;
}

const BusinessType* BusinessManager::findType(const std::string& id) const {
    for (const auto& t : types_) {
        if (t.id == id) return &t;
    }
    return nullptr;
}

bool BusinessManager::isLocked(const BusinessType& type) const {
    if (type.prerequisiteTypeId.empty()) return false;
    const Business* prereq = find(type.prerequisiteTypeId);
    return !prereq || prereq->level == 0;
}

std::string BusinessManager::resolvedOutputGoodId(const Business& b, const BusinessType& type) const {
    if (b.typeId == "farm") {
        if (const CropType* crop = findCrop(b.cropId)) return crop->id;
    }
    return type.outputGoodId;
}

void BusinessManager::print() const {
    std::cout << std::left << std::setw(4) << Localization::t("col_hash")
        << std::setw(16) << Localization::t("col_business")
        << std::right << std::setw(7) << Localization::t("col_level")
        << std::setw(12) << Localization::t("col_rate")
        << std::left << std::setw(13) << ("  " + Localization::t("col_good"))
        << std::setw(17) << ("  " + Localization::t("col_needs"))
        << std::right << std::setw(14) << Localization::t("col_cost") << "\n";
    int idx = 1;
    for (const auto& b : businesses_) {
        const BusinessType* t = findType(b.typeId);
        if (!t) continue;
        std::string outputGoodId = resolvedOutputGoodId(b, *t);
        std::string outputLabel = outputGoodId.empty() ? Localization::t("cash_label") : Localization::t(outputGoodId);
        std::string needsLabel = "-";
        if (!t->inputGoodId.empty()) {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1) << t->inputPerOutput << " " << Localization::t(t->inputGoodId);
            needsLabel = oss.str();
        }
        std::string costLabel = (b.level == 0 && isLocked(*t)) ? Localization::t("locked_label") : formatNumber(b.nextCost(*t));
        std::cout << std::left << std::setw(4) << idx++
            << std::setw(16) << Localization::t(t->id)
            << std::right << std::setw(7) << b.level
            << std::fixed << std::setprecision(3) << std::setw(12) << b.ratePerSecond(*t)
            << std::left << std::setw(13) << ("  " + outputLabel)
            << std::setw(17) << ("  " + needsLabel)
            << std::right << std::setw(14) << costLabel << "\n";
    }
    std::cout << Localization::t("rate_note") << "\n";
}

void BusinessManager::printTree() const {
    for (const auto& line : treeLines()) {
        std::cout << line << "\n";
    }
}

std::vector<std::string> BusinessManager::treeLines() const {
    std::vector<std::string> lines;
    lines.push_back(Localization::t("tree_header"));
    lines.push_back("============================================================");
    for (const auto& root : types_) {
        if (!root.prerequisiteTypeId.empty()) continue;
        collectTreeLines(root, 0, lines);
    }
    return lines;
}

void BusinessManager::collectTreeLines(const BusinessType& type, int depth, std::vector<std::string>& out) const {
    const Business* b = find(type.id);
    int level = b ? b->level : 0;

    std::string status;
    if (level > 0) status = Localization::t("tree_level_prefix") + std::to_string(level);
    else if (isLocked(type)) status = Localization::t("locked_label");
    else status = Localization::t("not_built_label");

    std::string outputGoodId = b ? resolvedOutputGoodId(*b, type) : type.outputGoodId;
    std::string outputLabel = outputGoodId.empty() ? Localization::t("cash_label") : Localization::t(outputGoodId);
    std::string indent(static_cast<size_t>(depth) * 3, ' ');
    std::string connector = depth == 0 ? "" : "|__ ";
    out.push_back(indent + connector + Localization::t(type.id) + " -> " + outputLabel + " [" + status + "]");

    for (const auto& child : types_) {
        if (child.prerequisiteTypeId == type.id) {
            collectTreeLines(child, depth + 1, out);
        }
    }
}
