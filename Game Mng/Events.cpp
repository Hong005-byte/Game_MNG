#include "Events.h"
#include "Localization.h"
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace {
    constexpr long long kCheckIntervalSeconds = 300; // roll once per simulated 5 minutes
    constexpr double kEventChance = 0.12;             // chance a flavor event fires on a given roll
    constexpr long long kMaxChecksPerCall = 500;       // chunk budget; chunk duration scales up beyond this

    // Separate, much rarer roll for economy-wide disasters/booms -- these hit
    // every good (or a big cut of one good's stock, or a chunk of current
    // cash) at once, unlike the single-good flavor events above. Independent
    // of the flavor-event roll, so the two can (rarely) land in the same chunk.
    constexpr double kDisasterChance = 0.025;
}

EventSystem::EventSystem() : rng_(std::random_device{}()) {}

void EventSystem::roll(long long seconds, double& money, Market& market, Life& life, std::vector<std::string>& log, double disasterChanceMult, double sicknessChanceMult) {
    if (seconds <= 0) return;
    long long checks = std::min(seconds / kCheckIntervalSeconds, kMaxChecksPerCall);
    if (checks <= 0) return;

    // Chunk duration scales up for very long spans instead of staying fixed at
    // kCheckIntervalSeconds, so illness progression (and flavor-event odds)
    // stay proportional to elapsed time even across a multi-month offline gap,
    // rather than being diluted by the fixed check-count cap.
    double chunkSeconds = static_cast<double>(seconds) / static_cast<double>(checks);
    double chunkGameDays = (chunkSeconds * Life::kTimeCompression) / Life::kGameSecondsPerDay;

    auto& goods = market.goods();
    std::uniform_real_distribution<double> chance(0.0, 1.0);
    std::uniform_int_distribution<int> pickGood(0, goods.empty() ? 0 : static_cast<int>(goods.size()) - 1);
    std::uniform_int_distribution<int> pickEvent(0, 4);
    std::uniform_int_distribution<int> pickDisaster(0, 3);

    for (long long i = 0; i < checks; ++i) {
        // Illness onset/progression: rolled every chunk regardless of the
        // flavor-event roll below, since it's tracking a persistent condition.
        if (life.sick) {
            life.sickDays += chunkGameDays;
        } else {
            double sicknessChance = Life::kSicknessChancePerGameDay * chunkGameDays * sicknessChanceMult;
            if (chance(rng_) < sicknessChance) {
                life.sick = true;
                log.push_back(Localization::t("event_prefix") + Localization::t("event_illness"));
            }
        }

        // Disaster roll: independent of the flavor-event roll below, so it
        // can land on its own chunk (or, rarely, the same one).
        if (!goods.empty() && chance(rng_) < kDisasterChance * disasterChanceMult) {
            std::ostringstream doss;
            doss << std::fixed << std::setprecision(2);
            switch (pickDisaster(rng_)) {
                case 0: { // market-wide crash: every good takes a hit at once
                    double mult = 0.55 + chance(rng_) * 0.20; // -25% to -45%
                    for (auto& good : goods) {
                        good.price *= mult;
                        double floor = good.basePrice * 0.15;
                        if (good.price < floor) good.price = floor;
                    }
                    doss << Localization::t("event_prefix") << Localization::t("event_market_crash");
                    break;
                }
                case 1: { // market-wide boom: the rare good news, symmetric with the crash above
                    double mult = 1.20 + chance(rng_) * 0.20; // +20% to +40%
                    for (auto& good : goods) good.price *= mult;
                    doss << Localization::t("event_prefix") << Localization::t("event_market_boom");
                    break;
                }
                case 2: { // warehouse disaster: much harsher than ordinary spoilage
                    Good& dg = goods[pickGood(rng_)];
                    if (dg.stock <= 0.0) break;
                    double lost = dg.stock * (0.35 + chance(rng_) * 0.35); // 35%-70% of current stock
                    dg.stock -= lost;
                    doss << Localization::t("event_prefix") << Localization::t("event_disaster_prefix") << Localization::t(dg.id)
                        << Localization::t("event_disaster_mid") << lost << Localization::t("event_disaster_suffix");
                    break;
                }
                case 3: { // recession: a real cash hit, percentage-based so it stays relevant late-game
                    if (money <= 0.0) break;
                    double loss = money * (0.12 + chance(rng_) * 0.13); // 12%-25% of current cash
                    money -= loss;
                    doss << Localization::t("event_prefix") << Localization::t("event_recession_prefix") << loss << ".";
                    break;
                }
            }
            if (!doss.str().empty()) log.push_back(doss.str());
        }

        if (goods.empty() || chance(rng_) > kEventChance) continue;

        Good& g = goods[pickGood(rng_)];
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2);

        switch (pickEvent(rng_)) {
            case 0: { // price surge
                double mult = 1.3 + chance(rng_) * 0.3;
                g.price *= mult;
                oss << Localization::t("event_prefix") << Localization::t(g.id) << Localization::t("event_surge_suffix") << g.price << ".";
                break;
            }
            case 1: { // price crash
                double mult = 0.5 + chance(rng_) * 0.2;
                g.price *= mult;
                double floor = g.basePrice * 0.15;
                if (g.price < floor) g.price = floor;
                oss << Localization::t("event_prefix") << Localization::t(g.id) << Localization::t("event_crash_suffix") << g.price << ".";
                break;
            }
            case 2: { // windfall
                double bonus = 20.0 + chance(rng_) * 80.0;
                money += bonus;
                oss << Localization::t("event_prefix") << Localization::t("event_windfall_prefix") << bonus << ".";
                break;
            }
            case 3: { // theft
                if (money <= 0.0) continue;
                double loss = std::min(money, 10.0 + chance(rng_) * 40.0);
                money -= loss;
                oss << Localization::t("event_prefix") << Localization::t("event_theft_prefix") << loss << ".";
                break;
            }
            case 4: { // spoilage
                if (g.stock <= 0.0) continue;
                double lost = g.stock * (0.05 + chance(rng_) * 0.10);
                g.stock -= lost;
                oss << Localization::t("event_prefix") << Localization::t("event_spoilage_prefix") << Localization::t(g.id)
                    << Localization::t("event_spoilage_mid") << lost << Localization::t("event_spoilage_suffix");
                break;
            }
        }
        log.push_back(oss.str());
    }
}
