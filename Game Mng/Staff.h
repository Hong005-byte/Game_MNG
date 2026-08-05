#pragma once

// A simple workforce upgrade: each level costs more than the last and grants
// a global production multiplier applied to every business's output.
struct Staff {
    int level = 0;
    double baseCost = 200.0;
    double costGrowth = 1.25;
    double boostPerLevel = 0.05; // +5% output per level, applies to all businesses

    double nextCost() const;
    double multiplier() const;
};
