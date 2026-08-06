#pragma once
#include <string>
#include <vector>

// The four-season cycle (see Game::currentSeason()) that gives the Farm's
// selectable crops (see CropType below) a favorite-season yield bonus.
enum class Season { Spring, Summer, Autumn, Winter };

// One thing the Farm can be planted with. `id` doubles as the Market Good id
// it produces (see Market.cpp) -- the Farm's own BusinessType::outputGoodId
// is ignored in favor of whichever crop is currently active (see
// Business::cropId and Game::simulateElapsed's farm special-case).
struct CropType {
    std::string id;
    std::string name;
    double rateMultiplier; // relative to the Farm's own BusinessType::baseRate
    Season favoriteSeason; // active crop gets Game::kSeasonBonusMultiplier while this season is current
};

// One additional (beyond BusinessType::inputGoodId, the "primary" input)
// good a multi-input recipe needs -- see BusinessType::extraInputs.
struct InputRequirement {
    std::string goodId;
    double perOutput; // units of goodId consumed per unit of output produced, same meaning as inputPerOutput
};

// Static definition of a kind of business the player can build.
struct BusinessType {
    std::string id;
    std::string name;
    std::string outputGoodId; // empty => produces money directly (no market good involved)
    double baseCost;
    double costGrowth;   // cost multiplier applied per level already owned
    double baseRate;     // output per second contributed by each level

    std::string inputGoodId;    // empty => raw producer, no input consumed
    double inputPerOutput = 0.0; // units of inputGoodId consumed per unit of output produced

    std::string prerequisiteTypeId; // empty => always buildable; otherwise that
                                     // business must be built (level >= 1) first
    int tier = 1; // 1 = raw producer, 2 = processor, 3 = advanced good

    // Extra goods a recipe needs beyond inputGoodId (the "primary" input) --
    // empty for every single-input business (the vast majority). See
    // Game::simulateElapsed's Pass 2 for how a multi-input recipe's actual
    // output is bottlenecked by whichever input (primary or extra) is
    // shortest, and BusinessInfo::inputs (Game.h) for the combined live
    // "have vs need" list a UI reads to display all of them together.
    std::vector<InputRequirement> extraInputs;
};

// The player's actual instance of a business (level 0 = not built yet).
struct Business {
    std::string typeId;
    int level = 0;
    // Hired workers, separate from the global Staff Office: each one boosts
    // just this business's output (see Game::kWorkerBoostPerWorker), capped
    // at Game::kMaxWorkersPerBusiness. 0 for a freshly-built business.
    int workers = 0;
    // Only meaningful for the "farm" business -- which CropType (see above)
    // it's currently planted with. Ignored by every other business.
    std::string cropId = "wheat";

    // Only meaningful for the "storefront" business -- its auto-sell config
    // (see Game::trySetStorefrontAutoSell/simulateElapsed's Pass 3 and
    // GameWorld::drawAutoSellOverlay). Empty goodId = auto-sell disabled.
    // Continuously sells autoSellGoodId from the warehouse, but only once
    // its current market price has reached this threshold -- the player
    // still has to judge the market themselves, same as signing a Contract.
    std::string autoSellGoodId;
    double autoSellThreshold = 0.0;

    // ---- First-build construction (see BusinessManager::requiresConstruction/
    // buildMaterialsFor/buildDaysFor and Game::tryStartConstruction). Only
    // meaningful while level == 0: 0 = an empty, unbuilt plot; > 0 = a
    // construction site counting down in-game days (see Game::simulateElapsed).
    // Once it reaches 0 the business becomes level 1 -- there is no separate
    // "under construction" flag, the three world-map states (empty plot /
    // site / finished) are derived from level + this field together. Doesn't
    // apply at all to businesses BusinessManager::requiresConstruction()
    // says don't need it (farm/lumber/quarry/mine) -- those still go
    // straight from 0 to 1 via the plain cash purchase path. ----
    double constructionDaysRemaining = 0.0;

    double nextCost(const BusinessType& type) const;
    double ratePerSecond(const BusinessType& type) const;
};

// One material good + quantity required to start construction on a business
// (see BusinessManager::buildMaterialsFor). Consumed from the warehouse (not
// purchased) the moment construction starts, on top of the usual cash cost.
struct BuildMaterialCost {
    std::string goodId;
    double amount = 0.0;
};

// The management system: the catalogue of business types plus the player's
// current levels in each. Building/upgrading costs money; higher level means
// more output per second, feeding either the market (goods) or cash directly.
// Types form a production tree via prerequisiteTypeId — a processor is locked
// until its upstream source has been built at least once.
class BusinessManager {
public:
    BusinessManager();

    const std::vector<BusinessType>& types() const { return types_; }
    std::vector<Business>& businesses() { return businesses_; }
    const std::vector<Business>& businesses() const { return businesses_; }

    Business* find(const std::string& id);
    const Business* find(const std::string& id) const;
    const BusinessType* findType(const std::string& id) const;

    // The Farm's selectable crops (see CropType above) -- static data, no
    // "current season" dependency here; that lives in Game.
    const std::vector<CropType>& crops() const { return crops_; }
    const CropType* findCrop(const std::string& id) const;

    // True if `type` still needs its prerequisite built before it can be constructed.
    bool isLocked(const BusinessType& type) const;

    // ---- First-build construction (see Business::constructionDaysRemaining
    // and Game::tryStartConstruction/businessConstructionInfo). ----
    // False only for the four always-free starters (farm/lumber/quarry/mine)
    // -- everything else needs buildMaterialsFor() gathered up front and
    // buildDaysFor() days of waiting before its first level completes.
    bool requiresConstruction(const std::string& typeId) const;
    // Computed from `type.baseCost` rather than 40 hand-authored recipes (see
    // the comment above the definition for the exact formula/ratios).
    // sawmill/mason/smelter -- the businesses that turn raw goods into the
    // planks/bricks/iron_ingot everyone else builds with -- ask for wood+stone
    // instead, so building the material source never needs the material itself.
    std::vector<BuildMaterialCost> buildMaterialsFor(const BusinessType& type) const;
    // 3-5 in-game days, biased by tier with a small per-id jitter so same-tier
    // buildings don't all take an identical number of days.
    int buildDaysFor(const BusinessType& type) const;

    void print() const;

    // Draws the production tree (root resources down through what they unlock),
    // showing each business's current level or lock status.
    void printTree() const;

    // Same content as printTree(), as lines instead of printed directly —
    // for a UI (e.g. the graphical world's tree overlay) to render itself.
    std::vector<std::string> treeLines() const;

    // type.outputGoodId, except for "farm" -- which instead reports whatever
    // its currently-active crop produces (see Business::cropId). Every
    // display site (print(), collectTreeLines(), and Game::businessInfos())
    // needs this same substitution, so it's centralized here (public since
    // Game::businessInfos() needs it too).
    std::string resolvedOutputGoodId(const Business& b, const BusinessType& type) const;

private:
    void collectTreeLines(const BusinessType& type, int depth, std::vector<std::string>& out) const;

    std::vector<BusinessType> types_;
    std::vector<Business> businesses_;
    std::vector<CropType> crops_;
};
