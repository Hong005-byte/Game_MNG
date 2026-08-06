#include "Business.h"
#include "Format.h"
#include "Localization.h"
#include <algorithm>
#include <cmath>
#include <functional>
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
    // Tier-based construction cash cost (see requiresConstruction/
    // buildMaterialsFor below) -- every non-free-starter business's baseCost
    // is one of these three, tier1 -> tier2 -> tier3 growing x5 per tier, so
    // reaching the back of the production tree is a real climb instead of a
    // quick early unlock. costGrowth (the per-level upgrade multiplier
    // that's unrelated to this) still varies per business as before.
    constexpr double kTier1Cost = 1200.0, kTier2Cost = 6000.0, kTier3Cost = 30000.0;

    // Tier 1: raw producers, always buildable.
    types_.push_back(BusinessType{ "farm",   "Wheat Farm",  "wheat", 50.0,  1.15, 0.50 });
    types_.push_back(BusinessType{ "mine",   "Ore Mine",    "ore",   400.0, 1.18, 0.08 });
    types_.push_back(BusinessType{ "lumber", "Lumber Camp", "wood",  120.0, 1.16, 0.30 });
    types_.push_back(BusinessType{ "quarry", "Quarry",      "stone", 250.0, 1.17, 0.20 });
    types_.push_back(BusinessType{ "storefront", "Storefront", "", kTier1Cost, 1.16, 0.30 });

    // Tier 2: single-input processors, locked until their tier-1 source is built.
    types_.push_back(BusinessType{ "bakery",  "Bakery",       "bread",      kTier2Cost,  1.18, 0.10, "wheat", 3.0, "farm",   2 });
    types_.push_back(BusinessType{ "smelter", "Smelter",      "iron_ingot", kTier2Cost,  1.19, 0.06, "ore",   3.0, "mine",   2 });
    types_.push_back(BusinessType{ "sawmill", "Sawmill",      "planks",     kTier2Cost,  1.18, 0.15, "wood",  2.0, "lumber", 2 });
    types_.push_back(BusinessType{ "mason",   "Mason",        "bricks",     kTier2Cost,  1.18, 0.12, "stone", 2.5, "quarry", 2 });
    types_.push_back(BusinessType{ "gemshop", "Gem Workshop", "gem",        kTier2Cost,  1.20, 0.03, "ore",   2.0, "mine",   2 });

    // Tier 3: advanced goods, locked until their tier-2 source is built.
    types_.push_back(BusinessType{ "blacksmith", "Blacksmith", "tools",     kTier3Cost, 1.20, 0.04, "iron_ingot", 2.0, "smelter", 3 });
    types_.push_back(BusinessType{ "carpenter",  "Carpenter",  "furniture", kTier3Cost, 1.19, 0.05, "planks",     2.5, "sawmill", 3 });

    // Sheep -> wool -> cloth -> clothing chain, and the Fishing Dock (its
    // own downstream chains -- Smokehouse/Cannery/Deep Sea Fishing/Sushi
    // Bar -- live on Fisher's Isle now, see the Harbor/Fisher's Isle block below).
    types_.push_back(BusinessType{ "sheep",      "Sheep Farm",   "wool",        kTier1Cost,  1.16, 0.25 });
    types_.push_back(BusinessType{ "fishing",    "Fishing Dock", "fish",        kTier1Cost,  1.17, 0.18 });
    types_.push_back(BusinessType{ "textile",    "Textile Mill", "cloth",       kTier2Cost,  1.18, 0.12, "wool", 2.0, "sheep",     2 });
    types_.push_back(BusinessType{ "tailor",     "Tailor",       "clothing",    kTier3Cost,  1.19, 0.045,"cloth", 2.0, "textile",  3 });

    // Valley District: orchard/apothecary, goldsmithing, and vineyard chains.
    types_.push_back(BusinessType{ "orchard",    "Orchard",     "fruit",    kTier1Cost, 1.16, 0.28 });
    types_.push_back(BusinessType{ "herbgarden", "Herb Garden", "herbs",    kTier1Cost, 1.15, 0.45 });
    types_.push_back(BusinessType{ "goldmine",   "Gold Mine",   "gold_ore", kTier1Cost, 1.19, 0.05 });
    types_.push_back(BusinessType{ "vineyard",   "Vineyard",    "grapes",   kTier1Cost, 1.16, 0.22 });

    types_.push_back(BusinessType{ "preserve",   "Preserve",   "preserves", kTier2Cost,  1.18, 0.10, "fruit",    2.5, "orchard",    2 });
    types_.push_back(BusinessType{ "apothecary", "Apothecary", "medicine",  kTier2Cost,  1.18, 0.08, "herbs",    3.0, "herbgarden", 2 });
    types_.push_back(BusinessType{ "goldsmith",  "Goldsmith",  "gold_bars", kTier2Cost,  1.19, 0.04, "gold_ore", 2.0, "goldmine",   2 });
    types_.push_back(BusinessType{ "winery",     "Winery",     "wine",      kTier2Cost,  1.18, 0.09, "grapes",   3.0, "vineyard",   2 });

    types_.push_back(BusinessType{ "alchemist", "Alchemist", "elixir",  kTier3Cost, 1.20, 0.035, "medicine",  2.0, "apothecary", 3 });
    types_.push_back(BusinessType{ "jeweler",   "Jeweler",   "jewelry", kTier3Cost, 1.21, 0.025, "gold_bars", 2.0, "goldsmith",  3 });

    // Harbor District: salt/pearl raw producers, plus the fishing dock's
    // downstream shipyard chain (fishing itself lives in this zone now --
    // see GameWorld's buildZones()). Kept deliberately "basic" -- the more
    // advanced fish processing (cannery/smokehouse) now lives on Fisher's
    // Isle instead (see the block below), reached through the Port.
    types_.push_back(BusinessType{ "seasalt",   "Salt Flats", "salt",   kTier1Cost, 1.16, 0.28 });
    types_.push_back(BusinessType{ "pearlfarm", "Pearl Farm", "pearls", kTier1Cost, 1.19, 0.05 });

    types_.push_back(BusinessType{ "shipyard",     "Shipyard",     "ships",         kTier2Cost,  1.19, 0.07,  "planks", 2.5, "fishing",   2 });
    types_.push_back(BusinessType{ "pearlatelier", "Pearl Atelier","pearl_jewelry", kTier2Cost,  1.19, 0.04,  "pearls", 2.0, "pearlfarm", 2 });

    // The Port: a one-time, expensive gate (not a production chain) that
    // unlocks sailing to Fisher's Isle once built (see Game::tryCommissionShip
    // and GameWorld's Zone 7) -- prerequisite is the Shipyard, since you
    // need shipbuilding capability before a proper port makes sense. A small
    // symbolic direct-cash rate (like storefront) rather than zero, so
    // leveling it past 1 isn't completely pointless.
    types_.push_back(BusinessType{ "port", "Port", "", kTier3Cost, 1.2, 0.05, "", 0.0, "shipyard", 3 });

    // Fisher's Isle: reached by sailing from the Port, not by walking (see
    // GameWorld::buildZones()'s Zone 7, which has no north/south/east/west
    // links at all). cannery and smokehouse moved here from Harbor/Mining
    // respectively -- smokehouse in particular used to sit in the Mining
    // District despite having nothing to do with mining, a leftover
    // placement mistake this finally fixes. deepsea/sushibar are new,
    // pricier than anything Harbor produces -- "more, better fish" out on
    // the isle instead of at the mainland dock.
    types_.push_back(BusinessType{ "cannery",   "Cannery",        "canned_fish", kTier2Cost,  1.18, 0.10,  "fish",   2.5, "fishing",   2 });
    types_.push_back(BusinessType{ "smokehouse","Smokehouse",     "smoked_fish", kTier2Cost,  1.18, 0.10,  "fish",   2.5, "fishing",   2 });
    types_.push_back(BusinessType{ "deepsea",   "Deep Sea Fishing","tuna",       kTier1Cost,  1.19, 0.06 });
    types_.push_back(BusinessType{ "sushibar",  "Sushi Bar",      "sushi",       kTier2Cost,  1.19, 0.05,  "tuna",   2.0, "deepsea",   2 });

    // Fisherman's Platter: a second multi-input recipe (see Cake Shop/
    // Artisan Bakery above) -- primary canned_fish, extras smoked_fish +
    // salt, combining Cannery/Smokehouse (both on the isle) with Salt Flats
    // (Harbor) into one premium dish, no new intermediate good besides the
    // final output.
    BusinessType fishermanPlatter{ "fishermanplatter", "Fisherman's Platter", "seafood_platter", kTier3Cost, 1.19, 0.05, "canned_fish", 2.0, "cannery", 3 };
    fishermanPlatter.extraInputs = { { "smoked_fish", 1.5 }, { "salt", 1.0 } };
    types_.push_back(fishermanPlatter);

    // Highlands District: dairy, apiary, trapping, and tea/flax chains.
    types_.push_back(BusinessType{ "dairyfarm", "Dairy Farm",    "milk",       kTier1Cost, 1.15, 0.30 });
    types_.push_back(BusinessType{ "beehive",   "Apiary",        "honey",      kTier1Cost, 1.15, 0.40 });
    types_.push_back(BusinessType{ "trapper",   "Trapper's Camp","pelts",      kTier1Cost, 1.16, 0.28 });
    types_.push_back(BusinessType{ "teafield",  "Tea Field",     "tea_leaves", kTier1Cost, 1.15, 0.42 });
    types_.push_back(BusinessType{ "flaxfield", "Flax Field",    "flax",       kTier1Cost, 1.16, 0.30 });

    types_.push_back(BusinessType{ "creamery",  "Creamery",  "cheese", kTier2Cost, 1.18, 0.10, "milk",       3.0, "dairyfarm", 2 });
    types_.push_back(BusinessType{ "meadery",   "Meadery",   "mead",   kTier2Cost, 1.18, 0.09, "honey",      2.5, "beehive",   2 });
    types_.push_back(BusinessType{ "tannery",   "Tannery",   "leather",kTier2Cost, 1.18, 0.10, "pelts",      2.5, "trapper",   2 });
    types_.push_back(BusinessType{ "teahouse",  "Tea House", "tea",    kTier2Cost, 1.18, 0.08, "tea_leaves", 3.0, "teafield",  2 });
    types_.push_back(BusinessType{ "linenmill", "Linen Mill","linen",  kTier2Cost, 1.18, 0.12, "flax",       2.0, "flaxfield", 2 });

    // Country Gift Basket: a third multi-input recipe -- primary cheese,
    // extras honey + tea, all three sourced from within the Highlands
    // District itself (Dairy Farm/Creamery, Apiary, Tea Field/Tea House)
    // rather than crossing districts like the Market Row/Fisher's Isle ones.
    BusinessType giftBasket{ "giftbasket", "Country Gift Basket", "gift_basket", kTier3Cost, 1.19, 0.05, "cheese", 2.0, "creamery", 3 };
    giftBasket.extraInputs = { { "honey", 1.5 }, { "tea", 1.0 } };
    types_.push_back(giftBasket);

    // Farm crop processors: the Farm itself stays a single "farm" BusinessType
    // (its displayed output good/rate depend on which CropType is active --
    // see crops_ below and Game::simulateElapsed's farm special-case), but
    // each crop's downstream processor is an ordinary tier-2 BusinessType
    // like any other, gated on "farm" being built at all (not on which crop
    // is currently planted -- a processor with no matching crop growing just
    // sits idle with nothing to consume, same as any other input-starved
    // processor already does).
    types_.push_back(BusinessType{ "jamkitchen",    "Jam Kitchen",     "strawberry_jam",       kTier2Cost, 1.18, 0.10, "strawberry", 2.5, "farm", 2 });
    types_.push_back(BusinessType{ "popcornstand",  "Popcorn Stand",   "popcorn",              kTier2Cost, 1.18, 0.11, "corn",       3.0, "farm", 2 });
    types_.push_back(BusinessType{ "juicebar",      "Juice Bar",       "watermelon_juice",     kTier2Cost, 1.18, 0.10, "watermelon", 3.0, "farm", 2 });
    types_.push_back(BusinessType{ "pieshop",       "Pie Shop",        "pumpkin_pie",          kTier2Cost, 1.18, 0.09, "pumpkin",    2.5, "farm", 2 });
    types_.push_back(BusinessType{ "roaststand",    "Roast Stand",     "roasted_sweet_potato", kTier2Cost, 1.18, 0.10, "sweetpotato",2.5, "farm", 2 });
    types_.push_back(BusinessType{ "picklinghouse", "Pickling House",  "sauerkraut",           kTier2Cost, 1.18, 0.11, "cabbage",    3.0, "farm", 2 });

    // Honey Refinery -> Cake Shop / Artisan Bakery: the first multi-input
    // recipes in the game (see BusinessType::extraInputs) -- each combines
    // goods from more than one existing production chain instead of just
    // one. All three live in Market Row (see GameWorld::buildZones()'s Zone
    // 6, now a 3x3 grid) alongside the farm-crop stalls, since they're the
    // same "food processing" theme.
    types_.push_back(BusinessType{ "honeyrefinery", "Honey Refinery", "honey_syrup", kTier2Cost, 1.18, 0.10, "honey", 2.5, "beehive", 2 });
    // Cake Shop: primary input honey_syrup, extra inputs milk + wheat (the
    // game has no separate "flour" good -- Bakery already bakes straight
    // from wheat, so Cake Shop does too).
    BusinessType cakeShop{ "cakeshop", "Cake Shop", "cake", kTier3Cost, 1.19, 0.05, "honey_syrup", 2.0, "honeyrefinery", 3 };
    cakeShop.extraInputs = { { "milk", 2.0 }, { "wheat", 2.0 } };
    types_.push_back(cakeShop);
    // Artisan Bakery: primary input bread, extra inputs fruit + honey --
    // combines three separate chains (Farm/Bakery, Orchard, Apiary) with no
    // new intermediate good needed besides the final output.
    BusinessType artisanBakery{ "artisanbakery", "Artisan Bakery", "fruit_bread", kTier3Cost, 1.19, 0.05, "bread", 2.0, "bakery", 3 };
    artisanBakery.extraInputs = { { "fruit", 2.0 }, { "honey", 1.5 } };
    types_.push_back(artisanBakery);

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

bool BusinessManager::requiresConstruction(const std::string& typeId) const {
    // The only four businesses that stay "just pay cash, built instantly" --
    // everything downstream of them (literally, in the production tree) has
    // to be put up as a proper construction project instead. Kept as a
    // small fixed list rather than e.g. "tier == 1 && no prerequisite" so
    // it's obvious at a glance and doesn't silently change if BusinessType
    // data shifts around later.
    static const std::vector<std::string> kFreeStarters = { "farm", "lumber", "quarry", "mine" };
    return std::find(kFreeStarters.begin(), kFreeStarters.end(), typeId) == kFreeStarters.end();
}

std::vector<BuildMaterialCost> BusinessManager::buildMaterialsFor(const BusinessType& type) const {
    if (!requiresConstruction(type.id)) return {}; // farm/lumber/quarry/mine -- cash only, see above

    // Unit prices mirror Market.cpp's starting prices (see the Good entries
    // there) -- only used here to turn a cash "value" into a plausible item
    // count for a one-time build recipe, not to track live market prices.
    constexpr double kWoodPrice = 3.0, kStonePrice = 5.0, kPlanksPrice = 8.0, kBricksPrice = 14.0, kIronIngotPrice = 40.0;

    auto units = [](double value, double price) {
        return std::max(1.0, std::ceil(value / price));
    };

    // Half of the existing (already-tuned) cash cost is "spent again" in
    // materials -- an added hurdle on top of the cash price, not a
    // replacement for it (see the plan's balance notes).
    double value = type.baseCost * 0.5;

    // sawmill/mason/smelter turn raw wood/stone/ore into the planks/bricks/
    // iron_ingot every other business below builds with -- if they also
    // needed planks/bricks to build, nothing could ever get started, so
    // they ask for the raw goods instead.
    if (type.id == "sawmill" || type.id == "mason" || type.id == "smelter") {
        return {
            { "wood",  units(value * 0.6, kWoodPrice) },
            { "stone", units(value * 0.4, kStonePrice) },
        };
    }

    if (type.tier >= 3) {
        return {
            { "planks",     units(value * 0.4, kPlanksPrice) },
            { "bricks",     units(value * 0.3, kBricksPrice) },
            { "iron_ingot", units(value * 0.3, kIronIngotPrice) },
        };
    }
    return {
        { "planks", units(value * 0.55, kPlanksPrice) },
        { "bricks", units(value * 0.45, kBricksPrice) },
    };
}

int BusinessManager::buildDaysFor(const BusinessType& type) const {
    int base = 3 + (type.tier - 1); // tier1=3, tier2=4, tier3=5
    // Deterministic +-1 jitter from the id so a whole tier doesn't all take
    // an identical number of days, without needing to hand-pick 40 numbers.
    int jitter = static_cast<int>(std::hash<std::string>{}(type.id) % 3) - 1; // -1, 0, or +1
    return std::clamp(base + jitter, 3, 5);
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
