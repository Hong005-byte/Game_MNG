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

    double nextCost(const BusinessType& type) const;
    double ratePerSecond(const BusinessType& type) const;
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
