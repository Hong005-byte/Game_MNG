#pragma once
#include <string>
#include <vector>
#include <random>

// One tradeable commodity. Price drifts each tick (mean-reverting random walk)
// and nudges further whenever the player buys or sells it.
struct Good {
    std::string id;
    std::string name;
    double basePrice;
    double price;
    double volatility; // fraction of basePrice used as per-step noise
    double stock = 0.0; // amount currently held by the player
};

// The trading system: a small basket of goods with fluctuating prices that
// the player can buy low / sell high, or feed with output from businesses.
class Market {
public:
    Market();

    std::vector<Good>& goods() { return goods_; }
    const std::vector<Good>& goods() const { return goods_; }

    Good* find(const std::string& id);

    // Advances every good's price by `steps` random-walk ticks.
    void advance(int steps);

    // Returns false (no-op) if the player can't afford it / doesn't have the stock.
    bool buy(const std::string& id, double quantity, double& money);
    bool sell(const std::string& id, double quantity, double& money);

    // Business production/consumption also leans on price, not just manual
    // trades: producing a lot relative to what's already stocked nudges the
    // price down (oversupply), consuming a lot of an input relative to its
    // stock nudges it up (scarcity). Both read the good's CURRENT stock, so
    // call before applying the corresponding stock change, not after.
    void applyProductionPressure(const std::string& id, double amountAdded);
    void applyConsumptionPressure(const std::string& id, double amountConsumed);

    void print() const;

private:
    std::vector<Good> goods_;
    std::mt19937 rng_;
};
