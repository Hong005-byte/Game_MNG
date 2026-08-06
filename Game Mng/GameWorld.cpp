#include "GameWorld.h"
#include "Localization.h"
#include "Format.h"
#include "Version.h"
#include "UpdateChecker.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX // windows.h's min/max macros would otherwise break every std::min/std::max call below
#endif
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
// windows.h's legacy 16-bit near/far macros collide with this file's own use
// of "near" as a perfectly ordinary local variable name (see the nearby-
// building lookups throughout) -- neutralize them right after the include.
#undef near
#undef far
#endif
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace {
    constexpr float kPlayerSize = 26.f;
    constexpr float kInteractRadius = 55.f;
    constexpr float kMoveSpeed = 220.f;
    constexpr float kSprintMultiplier = 1.8f; // Shift held -> this much faster than the default walk speed
    constexpr float kMinigameIndicatorSpeed = 2.5f; // radians/sec -- how fast the timing-bar marker sweeps
    constexpr float kMinigameCooldownSeconds = 25.f; // shared by the fishing and mining timing-bar cooldowns
    constexpr float kLumberCooldownSeconds = 25.f;

    // Per-activity flavor for the shared timing-bar overlay: title/hint/
    // button text and an accent color, keyed by which building triggered it.
    struct MinigameFlavor { const char* titleKey; const char* buttonKey; sf::Color accent; };
    MinigameFlavor minigameFlavorFor(const std::string& businessId) {
        if (businessId == "mine" || businessId == "goldmine") {
            return { "mining_title", "mining_catch_button", sf::Color(200, 170, 110) };
        }
        return { "fishing_title", "fishing_catch_button", sf::Color(90, 200, 230) };
    }
    constexpr float kTreeRadius = 16.f;
    constexpr float kEdgeMargin = 24.f; // how far from the entry edge a player lands after a zone transition
    constexpr float kHistorySampleInterval = 2.f; // seconds between net-worth sparkline samples
    constexpr size_t kMaxHistorySamples = 150;     // 2s * 150 = 5 minutes of visible history
    constexpr float kDayNightCycleSeconds = 180.f; // one full day/night cycle every 3 real minutes
    constexpr int kRainDropCount = 60;

    // Windowed size presets for the Settings overlay (see Settings::
    // resolutionIndex) -- all share the logical 1280x820 canvas's ~1.561
    // aspect ratio, so the letterbox view (see GameWorld::applyVideoMode)
    // never has to add bars around a windowed mode, only around Fullscreen.
    const sf::Vector2u kResolutionPresets[] = { {1280u, 820u}, {1600u, 1025u}, {1920u, 1230u} };
    constexpr int kResolutionPresetCount = 3;

    sf::Color darken(sf::Color c, float factor) {
        return sf::Color(
            static_cast<std::uint8_t>(c.r * factor),
            static_cast<std::uint8_t>(c.g * factor),
            static_cast<std::uint8_t>(c.b * factor));
    }

    float randRange(float lo, float hi) {
        return lo + static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX) * (hi - lo);
    }

    // sf::String's implicit conversion from std::string assumes the local ANSI
    // code page, which mangles our UTF-8-encoded Chinese text. Every string
    // handed to SFML (window title, sf::Text) must go through this instead.
    sf::String toSfString(const std::string& utf8) {
        return sf::String::fromUtf8(utf8.begin(), utf8.end());
    }

    // Only still consulted by drawWorkshopShape's fallback path and
    // drawCottageShape now -- every id that used to rely on this (bakery,
    // smelter, blacksmith, smokehouse) gets its chimney unconditionally from
    // its themed shape's own `alwaysSmokes` argument instead (see
    // drawOvenShape/drawForgeShape/drawSmokehouseShape). Dropped "gemshop"
    // for the same reason it moved to drawMasonGemShape in the first place --
    // gem-cutting doesn't smoke, and drawWorkshopBody's
    // `alwaysSmokes || smokesFrom(b.id)` fallback check would otherwise have
    // still lit its chimney despite drawMasonGemShape passing false.
    bool smokesFrom(const std::string& id) {
        return id == "bakery" || id == "smelter" || id == "blacksmith" || id == "smokehouse";
    }

    const char* seasonKey(Season s) {
        switch (s) {
        case Season::Spring: return "season_spring";
        case Season::Summer: return "season_summer";
        case Season::Autumn: return "season_autumn";
        default:             return "season_winter";
        }
    }

    // Short parenthetical naming the current season's life/economy nudge
    // (see Game.h's k*Multiplier constants) -- shown right next to the
    // season name in the HUD so the effect isn't invisible unless the
    // player goes and rereads How to Play.
    const char* seasonEffectKey(Season s) {
        switch (s) {
        case Season::Spring: return "season_effect_spring";
        case Season::Summer: return "season_effect_summer";
        case Season::Autumn: return "season_effect_autumn";
        default:             return "season_effect_winter";
        }
    }

    // Display name for a rebindable key -- covers letters/digits/arrows plus
    // whatever else a player might press while the Settings overlay is
    // waiting for a new binding (see awaitingRebind_); anything obscure
    // falls back to a numbered placeholder rather than showing nothing.
    std::string keyName(sf::Keyboard::Key key) {
        using Key = sf::Keyboard::Key;
        if (key >= Key::A && key <= Key::Z) {
            return std::string(1, static_cast<char>('A' + (static_cast<int>(key) - static_cast<int>(Key::A))));
        }
        if (key >= Key::Num0 && key <= Key::Num9) {
            return std::string(1, static_cast<char>('0' + (static_cast<int>(key) - static_cast<int>(Key::Num0))));
        }
        switch (key) {
        case Key::Up:       return "Up";
        case Key::Down:     return "Down";
        case Key::Left:     return "Left";
        case Key::Right:    return "Right";
        case Key::Space:    return "Space";
        case Key::Enter:    return "Enter";
        case Key::Tab:      return "Tab";
        case Key::LShift:   case Key::RShift:   return "Shift";
        case Key::LControl: case Key::RControl: return "Ctrl";
        case Key::LAlt:     case Key::RAlt:     return "Alt";
        default:            return "Key#" + std::to_string(static_cast<int>(key));
        }
    }

    // Small geometric "accent glyph" kinds drawn by drawAccentGlyph, reused
    // across the Field/Workshop/Dock/ServiceHall archetypes so buildings
    // sharing a base shape (there are too many production/service buildings
    // to hand-author a fully bespoke shape for every one) still read as
    // visually distinct from their neighbors. Plain ints (not an enum class)
    // so GameWorld.h's method declarations don't need to know about this —
    // it's purely a GameWorld.cpp rendering detail.
    constexpr int kAccentCircle = 0;
    constexpr int kAccentDiamond = 1;
    constexpr int kAccentTriangle = 2;
    constexpr int kAccentCross = 3;
    constexpr int kAccentDoubleDot = 4;
    constexpr int kAccentBar = 5;

    bool isDockId(const std::string& id) {
        return id == "fishing" || id == "shipyard" || id == "cannery" || id == "port" || id == "deepsea" ||
               id == "island_ferry" || id == "fishermanplatter";
    }
    bool isFieldId(const std::string& id) {
        return id == "dairyfarm" || id == "beehive" || id == "trapper" || id == "teafield" ||
               id == "flaxfield" || id == "seasalt" || id == "pearlfarm";
    }
    bool isServiceHallId(const std::string& id) {
        return id == "storefront" || id == "market" || id == "staff" || id == "bank" ||
               id == "warehouse" || id == "townhall";
    }

    // The 33 remaining tier-2/3 processors used to all share one generic
    // "Workshop" box+lean-to-roof shape, differing only by accent-glyph
    // color/kind -- reads as one uniform building repeated 33 times across
    // the whole map. Split into 8 themed sub-archetypes instead, grouped by
    // what the business actually makes (a bakery and a jeweler have nothing
    // in common to look at, even though both used to get the identical
    // shed). Each still recolors from accentFor() the same way the
    // Field/Dock/ServiceHall archetypes already do -- only the base shape
    // differs per group now. Whatever's left after all 8 (shouldn't be
    // anything -- kept as a safety net, see drawBuilding) still falls
    // through to the plain Workshop shape.
    bool isOvenId(const std::string& id) {
        return id == "bakery" || id == "cakeshop" || id == "artisanbakery" || id == "pieshop" ||
               id == "preserve" || id == "jamkitchen" || id == "roaststand" || id == "picklinghouse" ||
               id == "honeyrefinery";
    }
    bool isStallId(const std::string& id) {
        return id == "popcornstand" || id == "juicebar" || id == "teahouse" || id == "giftbasket" || id == "sushibar";
    }
    bool isForgeId(const std::string& id) {
        return id == "smelter" || id == "blacksmith" || id == "goldsmith";
    }
    bool isSawmillId(const std::string& id) {
        return id == "sawmill" || id == "carpenter";
    }
    bool isFiberId(const std::string& id) {
        return id == "textile" || id == "tailor" || id == "linenmill" || id == "tannery";
    }
    bool isMasonGemId(const std::string& id) {
        return id == "mason" || id == "gemshop" || id == "jeweler" || id == "pearlatelier";
    }
    bool isBreweryId(const std::string& id) {
        return id == "winery" || id == "meadery" || id == "alchemist" || id == "creamery" || id == "apothecary";
    }
    bool isSmokehouseId(const std::string& id) {
        return id == "smokehouse";
    }

    // Per-id accent glyph kind + color for the archetype-shared buildings.
    // Not exhaustive by construction (falls back to a plain grey circle) --
    // every id actually reachable through isFieldId/isServiceHallId/isDockId
    // or the Workshop catch-all in drawBuilding is listed here.
    struct BuildingAccent { int kind; sf::Color color; };
    BuildingAccent accentFor(const std::string& id) {
        static const std::unordered_map<std::string, BuildingAccent> table = {
            // Field
            { "dairyfarm", { kAccentCircle,   sf::Color(240, 240, 235) } },
            { "beehive",   { kAccentDiamond,  sf::Color(230, 180, 60) } },
            { "trapper",   { kAccentTriangle, sf::Color(120, 90, 70) } },
            { "teafield",  { kAccentDoubleDot,sf::Color(90, 140, 70) } },
            { "flaxfield", { kAccentBar,      sf::Color(150, 170, 210) } },
            { "seasalt",   { kAccentDiamond,  sf::Color(235, 235, 240) } },
            { "pearlfarm", { kAccentCircle,   sf::Color(220, 230, 235) } },
            // Workshop
            { "bakery",       { kAccentCircle,    sf::Color(210, 160, 80) } },
            { "smelter",      { kAccentTriangle,  sf::Color(230, 110, 40) } },
            { "mason",        { kAccentBar,       sf::Color(150, 150, 150) } },
            { "gemshop",      { kAccentDiamond,   sf::Color(110, 210, 220) } },
            { "blacksmith",   { kAccentCross,     sf::Color(80, 80, 90) } },
            { "carpenter",    { kAccentBar,       sf::Color(170, 120, 70) } },
            { "sawmill",      { kAccentBar,       sf::Color(170, 120, 70) } },
            { "textile",      { kAccentDoubleDot, sf::Color(170, 120, 190) } },
            { "smokehouse",   { kAccentTriangle,  sf::Color(140, 140, 140) } },
            { "tailor",       { kAccentDoubleDot, sf::Color(190, 140, 170) } },
            { "preserve",     { kAccentCircle,    sf::Color(190, 70, 70) } },
            { "apothecary",   { kAccentCross,     sf::Color(110, 160, 90) } },
            { "goldsmith",    { kAccentDiamond,   sf::Color(220, 180, 60) } },
            { "winery",       { kAccentCircle,    sf::Color(120, 50, 60) } },
            { "alchemist",    { kAccentCross,     sf::Color(140, 90, 190) } },
            { "jeweler",      { kAccentDiamond,   sf::Color(220, 140, 180) } },
            { "creamery",     { kAccentCircle,    sf::Color(235, 225, 200) } },
            { "meadery",      { kAccentDiamond,   sf::Color(220, 160, 50) } },
            { "tannery",      { kAccentTriangle,  sf::Color(140, 100, 70) } },
            { "teahouse",     { kAccentCircle,    sf::Color(120, 150, 90) } },
            { "linenmill",    { kAccentBar,       sf::Color(220, 215, 195) } },
            { "pearlatelier", { kAccentCircle,    sf::Color(225, 225, 230) } },
            { "sushibar",     { kAccentDiamond,   sf::Color(230, 100, 110) } },
            { "giftbasket",   { kAccentDiamond,   sf::Color(210, 160, 200) } },
            { "jamkitchen",     { kAccentCircle,   sf::Color(200, 90, 110) } },
            { "popcornstand",   { kAccentTriangle, sf::Color(230, 210, 120) } },
            { "juicebar",       { kAccentCircle,   sf::Color(200, 60, 70) } },
            { "pieshop",        { kAccentDiamond,  sf::Color(210, 170, 110) } },
            { "roaststand",     { kAccentTriangle, sf::Color(200, 120, 60) } },
            { "picklinghouse",  { kAccentBar,      sf::Color(140, 170, 90) } },
            { "honeyrefinery",  { kAccentDiamond,  sf::Color(230, 180, 60) } },
            { "cakeshop",       { kAccentCircle,   sf::Color(235, 200, 210) } },
            { "artisanbakery",  { kAccentBar,      sf::Color(200, 150, 90) } },
            // Dock
            { "fishing",  { kAccentCircle,   sf::Color(90, 140, 190) } },
            { "shipyard", { kAccentTriangle, sf::Color(230, 230, 235) } },
            { "cannery",  { kAccentBar,      sf::Color(160, 160, 170) } },
            { "port",     { kAccentCross,    sf::Color(90, 160, 200) } },
            { "deepsea",  { kAccentDoubleDot,sf::Color(40, 90, 150) } },
            { "fishermanplatter", { kAccentBar, sf::Color(80, 150, 160) } },
            { "island_ferry", { kAccentTriangle, sf::Color(210, 210, 215) } },
            // ServiceHall
            { "storefront", { kAccentBar,     sf::Color(200, 80, 80) } },
            { "market",     { kAccentDiamond, sf::Color(220, 190, 90) } },
            { "staff",      { kAccentCross,   sf::Color(90, 120, 180) } },
            { "bank",       { kAccentCircle,  sf::Color(215, 180, 70) } },
            { "warehouse",  { kAccentBar,     sf::Color(150, 110, 70) } },
            { "townhall",   { kAccentCross,   sf::Color(150, 110, 190) } },
        };
        auto it = table.find(id);
        return it != table.end() ? it->second : BuildingAccent{ kAccentCircle, sf::Color(180, 180, 180) };
    }

    // Groups the (growing) achievement list into a handful of categories for
    // the Achievements overlay -- a color-coded dot per row plus a header
    // whenever the category changes, rather than one flat undifferentiated
    // list. Fixed display order (not alphabetical): roughly "how the game
    // teaches you these systems", economy first since that's the earliest one.
    struct AchievementCategory { const char* labelKey; sf::Color color; };
    AchievementCategory achievementCategoryFor(const std::string& id) {
        static const std::unordered_map<std::string, AchievementCategory> table = {
            { "thousandaire",      { "ach_cat_economy", sf::Color(220, 190, 90) } },
            { "ten_thousandaire",  { "ach_cat_economy", sf::Color(220, 190, 90) } },
            { "trader",            { "ach_cat_economy", sf::Color(220, 190, 90) } },
            { "tycoon",            { "ach_cat_economy", sf::Color(220, 190, 90) } },
            { "first_business",    { "ach_cat_business", sf::Color(150, 180, 220) } },
            { "diversified",       { "ach_cat_business", sf::Color(150, 180, 220) } },
            { "well_staffed",      { "ach_cat_business", sf::Color(150, 180, 220) } },
            { "supply_chain",      { "ach_cat_business", sf::Color(150, 180, 220) } },
            { "craftsman",         { "ach_cat_business", sf::Color(150, 180, 220) } },
            { "full_crew",         { "ach_cat_business", sf::Color(150, 180, 220) } },
            { "harbor_pioneer",    { "ach_cat_business", sf::Color(150, 180, 220) } },
            { "highlands_settler", { "ach_cat_business", sf::Color(150, 180, 220) } },
            { "quarter_century",   { "ach_cat_life", sf::Color(200, 150, 200) } },
            { "half_century",      { "ach_cat_life", sf::Color(200, 150, 200) } },
            { "centenarian",       { "ach_cat_life", sf::Color(200, 150, 200) } },
            { "good_timing",       { "ach_cat_season", sf::Color(140, 200, 150) } },
            { "crop_rotator",      { "ach_cat_season", sf::Color(140, 200, 150) } },
            { "season_cycle",      { "ach_cat_season", sf::Color(140, 200, 150) } },
            { "master_farmer",     { "ach_cat_season", sf::Color(140, 200, 150) } },
            { "minigame_pro",      { "ach_cat_minigame", sf::Color(230, 150, 90) } },
            { "groundbreaking",    { "ach_cat_business", sf::Color(150, 180, 220) } },
            { "master_builder",    { "ach_cat_business", sf::Color(150, 180, 220) } },
            { "market_row_regular",{ "ach_cat_business", sf::Color(150, 180, 220) } },
            { "full_stock",        { "ach_cat_economy", sf::Color(220, 190, 90) } },
            { "harbormaster",      { "ach_cat_maritime", sf::Color(90, 160, 200) } },
            { "shipshape",         { "ach_cat_maritime", sf::Color(90, 160, 200) } },
            { "set_sail",          { "ach_cat_maritime", sf::Color(90, 160, 200) } },
            { "island_explorer",   { "ach_cat_maritime", sf::Color(90, 160, 200) } },
        };
        auto it = table.find(id);
        return it != table.end() ? it->second : AchievementCategory{ "ach_cat_business", sf::Color(180, 180, 180) };
    }
    // Fixed display order for the categories above.
    const char* const kAchievementCategoryOrder[] = { "ach_cat_economy", "ach_cat_business", "ach_cat_life", "ach_cat_season", "ach_cat_minigame", "ach_cat_maritime" };

    // Zone indices for the Port <-> Fisher's Isle sail/return trip (see
    // handleInteraction's "island_ferry" case and the Port's Sail button in
    // drawBusinessesOverlay) -- named rather than inlined since both sides
    // of the trip need to agree on them.
    constexpr int kHarborZoneIndex = 4;
    constexpr int kFisherIsleZoneIndex = 7;
    const sf::Vector2f kFisherIsleArrivalPos(120.f, 400.f);
    const sf::Vector2f kHarborReturnPos(570.f, 560.f); // just south of the Port building

    // Tier label colors — the single source of truth for "what color means
    // what tier", now applied to name text rather than the building's shape.
    const sf::Color kTier1(120, 200, 120);
    const sf::Color kTier2(226, 166, 82);
    const sf::Color kTier3(180, 130, 220);
    const sf::Color kService(120, 160, 220);

    // ---- Recipe Book pixel-art icons ----
    // Each entry below is a small hand-drawn pixel grid (role chars, not raw
    // colors -- see PixelRole) recolored per-good from one seed color, so
    // (say) wine/mead/elixir/medicine can all reuse the same bottle shape
    // while still looking distinct. Rows don't need equal length padding;
    // drawGoodIcon indexes by each row's own length. Goods with no entry
    // here fall back to a generic shaded pixel "crate" tinted from their
    // production-chain accent color (still pixelated, just not hand-drawn) --
    // see drawGoodIcon in the class body below.
    using PixelRows = std::vector<std::string>;

    // 'O' outline, 'H' highlight, 'B' base, 'S' shadow, 'A' accent1
    // (frosting/cork/lid/cream), 'D' accent2 (cherry/seed/label/stripe),
    // '.' transparent.
    const PixelRows& shapeBottle() {
        static const PixelRows rows = {
            ".....OO.....",
            ".....AA.....",
            ".....AA.....",
            "....OHHO....",
            "....OBBO....",
            "...OBBBBO...",
            "...OBBBBO...",
            "..OHBBBBBO..",
            "..OBBBBBBO..",
            "..OBSSBBSO..",
            "..OSSSSSSO..",
            "...OOOOOO...",
        };
        return rows;
    }
    const PixelRows& shapeJar() {
        static const PixelRows rows = {
            "..OOOOOOOO..",
            ".OAAAAAAAAO.",
            ".OHHHHHHHHO.",
            ".OHBBBBBBHO.",
            ".OBBDDDDBBO.",
            ".OBBDDDDBBO.",
            ".OBBBBBBBBO.",
            ".OBSSSSSSBO.",
            ".OSSSSSSSSO.",
            "..OOOOOOOO..",
        };
        return rows;
    }
    const PixelRows& shapeGem() {
        static const PixelRows rows = {
            "....OOOO....",
            "...OHHHHO...",
            "..OHHBBHHO..",
            ".OHBBBBBBHO.",
            "OHBBBBBBBBHO",
            "OBBBBBBBBBBO",
            ".OSBBBBBSO..",
            "..OSSBSSO...",
            "...OSSSO....",
            "....OOO.....",
        };
        return rows;
    }
    const PixelRows& shapeBar() {
        static const PixelRows rows = {
            "..OOOOOOOO..",
            ".OHHHHHHHHO.",
            "OHBBBBBBBBHO",
            "OBBBBBBBBBBO",
            "OBSSSSSSSBO.",
            ".OSSSSSSSO..",
            "..OOOOOOOO..",
        };
        return rows;
    }
    const PixelRows& shapeLoaf() {
        static const PixelRows rows = {
            "..OOOOOOOO..",
            ".OHHHHHHHHO.",
            "OHBBBBBBBBHO",
            "OBBSBBSBBBBO",
            "OBBBSBBSBBBO",
            "OBBBBBBBBBBO",
            ".OSSSSSSSO..",
            "..OOOOOOOO..",
        };
        return rows;
    }
    const PixelRows& shapeCake() {
        static const PixelRows rows = {
            ".....D......",
            "....OAO.....",
            "...OHHHO....",
            "..OAAAAAO...",
            ".OHHHHHHHO..",
            ".OBBBBBBBO..",
            "OHHHHHHHHHO.",
            "OBBBBBBBBBO.",
            "OBBSBBSBBBO.",
            "OSSSSSSSSSO.",
            ".OOOOOOOOO..",
        };
        return rows;
    }
    const PixelRows& shapeWedge() {
        static const PixelRows rows = {
            ".......OO...",
            "......OHHO..",
            ".....OHBBO..",
            "....OBBDBO..",
            "...OBBBBBO..",
            "..OBBDBBBO..",
            ".OBBBBBBBO..",
            "OSSSSSSSSO..",
            "OOOOOOOOOO..",
        };
        return rows;
    }
    const PixelRows& shapePlanks() {
        static const PixelRows rows = {
            "OOOOOOOOOOOO",
            "OHHHHHHHHHHO",
            "OBBBBBBBBBBO",
            "OOOOOOOOOOOO",
            "OHHHHHHHHHHO",
            "OBBBBBBBBBBO",
            "OOOOOOOOOOOO",
            "OSSSSSSSSSSO",
            "OBBBBBBBBBBO",
            "OOOOOOOOOOOO",
        };
        return rows;
    }
    const PixelRows& shapeBricks() {
        static const PixelRows rows = {
            "OOOOOOOOOOOO",
            "OBBBOBBBBOBO",
            "OBBBOBBBBOBO",
            "OOOOOOOOOOOO",
            "OBOBBBBOBBBO",
            "OBOBBBBOBBBO",
            "OOOOOOOOOOOO",
            "OBBBOBBBBOBO",
            "OBBBOBBBBOBO",
            "OOOOOOOOOOOO",
        };
        return rows;
    }
    const PixelRows& shapeFish() {
        static const PixelRows rows = {
            "............",
            "......OOO...",
            "....OOHHBO..",
            "..OOHHBBBBO.",
            "OODHBBBBBBBO",
            "OODHBBBBBBBO",
            "..OOSBBBBO..",
            "....OOSBO...",
            "......OOO...",
            "............",
        };
        return rows;
    }
    const PixelRows& shapeShirt() {
        static const PixelRows rows = {
            "..OO..OO....",
            ".OHHOOHHO...",
            "OHHHHHHHHO..",
            "OBHHHHHHBO..",
            ".OBBBBBBO...",
            "..OBBBBO....",
            "..OBBDBO....",
            "..OBBBBO....",
            "..OSSSSO....",
            "..OOOOOO....",
        };
        return rows;
    }
    const PixelRows& shapeChair() {
        static const PixelRows rows = {
            "OOOO........",
            "OHHO........",
            "OHHOOOOOOO..",
            "OBBBBBBBBO..",
            "OOOBBBBOOO..",
            "...OBBO.....",
            "...OBBO.....",
            "...OSSO.....",
            "...OOOO.....",
        };
        return rows;
    }
    const PixelRows& shapeBucket() {
        static const PixelRows rows = {
            "OHDAHDAHDO..",
            ".OHHHHHHO...",
            ".OBBBBBBO...",
            ".OBBDBBBO...",
            ".OBBBDBBO...",
            ".OSSSSSSO...",
            "..OOOOOO....",
        };
        return rows;
    }
    const PixelRows& shapeBoat() {
        static const PixelRows rows = {
            "....OA......",
            "....OAO.....",
            "...OHAHO....",
            "...OHAHO....",
            "..OHHAHHO...",
            "OOBBBBBBBOO.",
            "OSSSSSSSSSO.",
            ".OOOOOOOOO..",
        };
        return rows;
    }

    struct GoodIconDef { const PixelRows* shape; sf::Color seed; };

    // goodId -> {shape, seed color}. Not exhaustive by design -- every
    // processed good not listed here still gets a nicer-than-flat generic
    // icon (see drawGoodIcon), this table just covers the ones common/iconic
    // enough to be worth a hand-drawn shape.
    const std::unordered_map<std::string, GoodIconDef>& goodIconDefs() {
        static const std::unordered_map<std::string, GoodIconDef> table = {
            { "cake",                 { &shapeCake(),   sf::Color(235, 195, 165) } },
            { "fruit_bread",          { &shapeLoaf(),   sf::Color(214, 150, 95) } },
            { "bread",                { &shapeLoaf(),   sf::Color(200, 150, 80) } },
            { "cheese",               { &shapeWedge(),  sf::Color(240, 200, 60) } },
            { "honey_syrup",          { &shapeJar(),    sf::Color(230, 160, 30) } },
            { "honey",                { &shapeJar(),    sf::Color(240, 180, 40) } },
            { "preserves",            { &shapeJar(),    sf::Color(200, 60, 70) } },
            { "strawberry_jam",       { &shapeJar(),    sf::Color(190, 40, 60) } },
            { "gift_basket",          { &shapeJar(),    sf::Color(170, 120, 70) } },
            { "sauerkraut",           { &shapeJar(),    sf::Color(200, 210, 120) } },
            { "tea",                  { &shapeJar(),    sf::Color(120, 170, 90) } },
            { "wine",                 { &shapeBottle(), sf::Color(120, 20, 50) } },
            { "mead",                 { &shapeBottle(), sf::Color(220, 170, 40) } },
            { "elixir",               { &shapeBottle(), sf::Color(140, 60, 200) } },
            { "medicine",             { &shapeBottle(), sf::Color(80, 180, 120) } },
            { "watermelon_juice",     { &shapeBottle(), sf::Color(210, 60, 90) } },
            { "gem",                  { &shapeGem(),    sf::Color(80, 200, 230) } },
            { "jewelry",              { &shapeGem(),    sf::Color(230, 80, 140) } },
            { "pearl_jewelry",        { &shapeGem(),    sf::Color(235, 235, 245) } },
            { "iron_ingot",           { &shapeBar(),    sf::Color(150, 155, 165) } },
            { "gold_bars",            { &shapeBar(),    sf::Color(235, 195, 60) } },
            { "planks",               { &shapePlanks(), sf::Color(190, 140, 80) } },
            { "leather",              { &shapePlanks(), sf::Color(120, 80, 55) } },
            { "bricks",               { &shapeBricks(), sf::Color(175, 90, 70) } },
            { "canned_fish",          { &shapeFish(),   sf::Color(150, 170, 190) } },
            { "smoked_fish",          { &shapeFish(),   sf::Color(165, 120, 80) } },
            { "tuna",                 { &shapeFish(),   sf::Color(90, 120, 170) } },
            { "sushi",                { &shapeFish(),   sf::Color(230, 230, 225) } },
            { "seafood_platter",      { &shapeFish(),   sf::Color(120, 180, 170) } },
            { "clothing",             { &shapeShirt(),  sf::Color(90, 130, 190) } },
            { "cloth",                { &shapeShirt(),  sf::Color(200, 200, 210) } },
            { "linen",                { &shapeShirt(),  sf::Color(225, 220, 195) } },
            { "furniture",            { &shapeChair(),  sf::Color(160, 110, 70) } },
            { "popcorn",              { &shapeBucket(), sf::Color(235, 210, 120) } },
            { "pumpkin_pie",          { &shapeWedge(),  sf::Color(220, 140, 50) } },
            { "roasted_sweet_potato", { &shapeWedge(),  sf::Color(190, 110, 60) } },
            { "ships",                { &shapeBoat(),   sf::Color(150, 110, 70) } },
            { "tools",                { &shapeBar(),    sf::Color(160, 165, 175) } },
        };
        return table;
    }

    std::uint8_t clamp8(int v) { return static_cast<std::uint8_t>(std::clamp(v, 0, 255)); }
    sf::Color shade(sf::Color c, int delta) {
        return sf::Color(clamp8(c.r + delta), clamp8(c.g + delta), clamp8(c.b + delta), c.a);
    }
}

GameWorld::GameWorld(Game& game) : game_(game) {}

void GameWorld::initAudio() {
    constexpr unsigned int kSampleRate = 44100;
    auto makeTone = [](sf::SoundBuffer& buffer, const std::vector<float>& freqsHz, float durationEach) {
        std::vector<std::int16_t> samples;
        for (float freq : freqsHz) {
            std::size_t n = static_cast<std::size_t>(durationEach * static_cast<float>(kSampleRate));
            for (std::size_t i = 0; i < n; ++i) {
                float t = static_cast<float>(i) / static_cast<float>(kSampleRate);
                float envelope = 1.f - static_cast<float>(i) / static_cast<float>(n); // linear fade-out, avoids a click at the end
                float sample = std::sin(2.f * 3.14159265f * freq * t) * envelope * 0.3f;
                samples.push_back(static_cast<std::int16_t>(sample * 32767.f));
            }
        }
        if (!buffer.loadFromSamples(samples.data(), samples.size(), 1, kSampleRate, { sf::SoundChannel::Mono })) {
            std::cout << "[GameWorld] Failed to synthesize a sound effect; that sound will just stay silent.\n";
        }
    };

    makeTone(footstepBuffer_, { 140.f }, 0.05f);
    makeTone(interactBuffer_, { 700.f, 1050.f }, 0.07f);
    makeTone(achievementBuffer_, { 520.f, 660.f, 780.f, 1040.f }, 0.10f);
    makeTone(upgradeBuffer_, { 900.f, 1300.f }, 0.055f);

    footstepSound_.emplace(footstepBuffer_);
    interactSound_.emplace(interactBuffer_);
    achievementSound_.emplace(achievementBuffer_);
    upgradeSound_.emplace(upgradeBuffer_);

    // ---- Ambient rain/snow noise: plain white noise (no tone/pitch, unlike
    // every other synthesized sound here) looped underneath drawWeather's
    // rain-or-snow visual -- see the raining_ check in updateDayNightAndWeather. ----
    {
        constexpr float kNoiseDurationSeconds = 2.0f;
        constexpr float kNoiseAmplitude = 0.10f; // was 0.18 -- plain white noise at that level read as harsh static rather than rain
        std::vector<std::int16_t> noiseSamples;
        std::size_t n = static_cast<std::size_t>(kNoiseDurationSeconds * static_cast<float>(kSampleRate));
        noiseSamples.reserve(n);
        // A one-pole low-pass (simple running average with the previous
        // sample) knocks down the harsh high-frequency hiss that pure white
        // noise has, leaving something closer to a soft, dull rain patter.
        float prev = 0.f;
        for (std::size_t i = 0; i < n; ++i) {
            float raw = (static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX) * 2.f - 1.f) * kNoiseAmplitude;
            float sample = 0.6f * raw + 0.4f * prev;
            prev = sample;
            noiseSamples.push_back(static_cast<std::int16_t>(sample * 32767.f));
        }
        if (!ambientRainBuffer_.loadFromSamples(noiseSamples.data(), noiseSamples.size(), 1, kSampleRate, { sf::SoundChannel::Mono })) {
            std::cout << "[GameWorld] Failed to synthesize ambient rain noise; it will just stay silent.\n";
        }
        ambientRainSound_.emplace(ambientRainBuffer_);
        ambientRainSound_->setLooping(true);
    }

    applySfxVolume(); // sets each Sound's actual volume from its baseline * settings_.sfxVolumePercent

    // ---- Background music: one short looping "music box" phrase per season,
    // same makeTone synthesis as the SFX above (no asset files) -- each
    // note's own linear fade-out gives a plucked/bell character rather than
    // a sustained tone, so the notes stay distinct instead of blurring
    // together. Mood differs by register/tempo/note choice, not just tint:
    // Spring bright and major, Summer brighter and faster, Autumn warmer and
    // slower, Winter sparse and cold. ----
    makeTone(musicBuffers_[static_cast<int>(Season::Spring)],
        { 261.63f, 329.63f, 392.00f, 523.25f, 293.66f, 392.00f, 329.63f, 261.63f }, 0.35f);
    makeTone(musicBuffers_[static_cast<int>(Season::Summer)],
        { 329.63f, 392.00f, 493.88f, 659.25f, 587.33f, 493.88f, 392.00f, 329.63f }, 0.25f);
    makeTone(musicBuffers_[static_cast<int>(Season::Autumn)],
        { 220.00f, 261.63f, 329.63f, 440.00f, 392.00f, 329.63f, 261.63f, 220.00f }, 0.45f);
    makeTone(musicBuffers_[static_cast<int>(Season::Winter)],
        { 220.00f, 329.63f, 220.00f, 293.66f }, 0.9f);

    musicSounds_[0].emplace(musicBuffers_[static_cast<int>(game_.currentSeason())]);
    musicSounds_[1].emplace(musicBuffers_[static_cast<int>(game_.currentSeason())]); // same buffer, stays silent/stopped until the first crossfade needs it
    musicActiveIndex_ = 0;
    musicSounds_[0]->setLooping(true);
    applyMusicVolume();
    musicSounds_[0]->play();
}

void GameWorld::playMusicForSeason(Season season) {
    if (!musicSounds_[0] || !musicSounds_[1]) return;
    int nextIndex = 1 - musicActiveIndex_;
    musicSounds_[nextIndex]->setBuffer(musicBuffers_[static_cast<int>(season)]);
    musicSounds_[nextIndex]->setLooping(true);
    musicSounds_[nextIndex]->setVolume(0.f); // ramped up by updateMusicCrossfade
    musicSounds_[nextIndex]->play();
    musicCrossfadeFromIndex_ = musicActiveIndex_;
    musicActiveIndex_ = nextIndex;
    musicCrossfadeActive_ = true;
    musicCrossfadeTimer_ = 0.f;
}

void GameWorld::updateMusicCrossfade(float dt) {
    if (!musicCrossfadeActive_) return;
    musicCrossfadeTimer_ += dt;
    float t = std::clamp(musicCrossfadeTimer_ / kMusicCrossfadeDuration, 0.f, 1.f);
    float base = 45.f * std::clamp(settings_.musicVolumePercent, 0.f, 100.f) / 100.f;
    if (musicSounds_[musicActiveIndex_]) musicSounds_[musicActiveIndex_]->setVolume(base * t);
    if (musicSounds_[musicCrossfadeFromIndex_]) musicSounds_[musicCrossfadeFromIndex_]->setVolume(base * (1.f - t));
    if (t >= 1.f) {
        if (musicCrossfadeFromIndex_ != musicActiveIndex_ && musicSounds_[musicCrossfadeFromIndex_]) {
            musicSounds_[musicCrossfadeFromIndex_]->stop();
        }
        musicCrossfadeActive_ = false;
    }
}

void GameWorld::applyMusicVolume() {
    // Mid-crossfade, updateMusicCrossfade already recomputes both tracks'
    // volume from settings_.musicVolumePercent every frame -- touching just
    // the active one here would fight that ramp, so leave it alone until
    // the crossfade finishes.
    if (musicCrossfadeActive_) return;
    if (musicSounds_[musicActiveIndex_]) {
        musicSounds_[musicActiveIndex_]->setVolume(45.f * std::clamp(settings_.musicVolumePercent, 0.f, 100.f) / 100.f);
    }
}

void GameWorld::applySfxVolume() {
    // Each sound's own baseline (its relative loudness against the others,
    // tuned by ear) scaled by the player's master SFX volume setting.
    float scale = std::clamp(settings_.sfxVolumePercent, 0.f, 100.f) / 100.f;
    if (footstepSound_) footstepSound_->setVolume(30.f * scale);
    if (interactSound_) interactSound_->setVolume(55.f * scale);
    if (achievementSound_) achievementSound_->setVolume(60.f * scale);
    if (upgradeSound_) upgradeSound_->setVolume(50.f * scale);
    if (ambientRainSound_) ambientRainSound_->setVolume(26.f * scale); // was 40 -- softer, less of a hiss under the visual rain/snow
}

void GameWorld::applyVideoMode(sf::RenderWindow& window) {
    sf::VideoMode mode = sf::VideoMode(kResolutionPresets[0]);
    std::uint32_t style = sf::Style::Titlebar | sf::Style::Close;
    sf::State state = sf::State::Windowed;
    if (settings_.fullscreen) {
        mode = sf::VideoMode::getDesktopMode();
        style = sf::Style::None;
        state = sf::State::Fullscreen;
    } else {
        int idx = std::clamp(settings_.resolutionIndex, 0, kResolutionPresetCount - 1);
        mode = sf::VideoMode(kResolutionPresets[idx]);
    }
    window.create(mode, toSfString(Localization::t("window_title") + " v" + Version::kGameVersion), style, state);
    window.setFramerateLimit(60);
    actualWindowSize_ = window.getSize();

    // Letterbox: fit the fixed logical windowSize_ inside actualWindowSize_
    // preserving its aspect ratio (bars, not stretch/distortion), so every
    // existing pixel-coordinate draw/click call in this file stays correct
    // regardless of the real window/fullscreen size chosen here.
    float logicalAspect = static_cast<float>(windowSize_.x) / static_cast<float>(windowSize_.y);
    float actualAspect = static_cast<float>(actualWindowSize_.x) / static_cast<float>(actualWindowSize_.y);
    sf::FloatRect viewport(sf::Vector2f(0.f, 0.f), sf::Vector2f(1.f, 1.f));
    if (actualAspect > logicalAspect) {
        float widthFrac = logicalAspect / actualAspect; // window is relatively wider -> bars left/right
        viewport = sf::FloatRect(sf::Vector2f((1.f - widthFrac) / 2.f, 0.f), sf::Vector2f(widthFrac, 1.f));
    } else if (actualAspect < logicalAspect) {
        float heightFrac = actualAspect / logicalAspect; // window is relatively taller -> bars top/bottom
        viewport = sf::FloatRect(sf::Vector2f(0.f, (1.f - heightFrac) / 2.f), sf::Vector2f(1.f, heightFrac));
    }
    gameView_ = sf::View(sf::FloatRect(sf::Vector2f(0.f, 0.f), sf::Vector2f(static_cast<float>(windowSize_.x), static_cast<float>(windowSize_.y))));
    gameView_.setViewport(viewport);
    window.setView(gameView_);
}

void GameWorld::buildZones() {
    zones_.clear();
    zones_.resize(8); // 0 = Town Square, 1 = Farmlands, 2 = Mining District, 3 = Valley District,
                       // 4 = Harbor District, 5 = Highlands District, 6 = Market Row, 7 = Fisher's Isle

    const sf::Vector2f bSize(110.f, 80.f);
    auto addBuilding = [](Zone& z, const std::string& id, sf::Vector2f pos, sf::Color labelColor, sf::Vector2f size) {
        WorldBuilding b;
        b.id = id;
        b.labelKey = id;
        b.position = pos;
        b.size = size;
        b.labelColor = labelColor;
        z.buildings.push_back(b);
    };
    auto addTree = [](Zone& z, sf::Vector2f pos) {
        z.decorations.push_back(Decoration{ Decoration::Kind::Tree, pos, {} });
    };
    auto addBush = [](Zone& z, sf::Vector2f pos) {
        z.decorations.push_back(Decoration{ Decoration::Kind::Bush, pos, {} });
    };
    auto addPatch = [](Zone& z, sf::Vector2f pos, sf::Vector2f size) {
        z.decorations.push_back(Decoration{ Decoration::Kind::GrassPatch, pos, size });
    };
    auto addPath = [](Zone& z, sf::Vector2f pos, sf::Vector2f size) {
        z.decorations.push_back(Decoration{ Decoration::Kind::Path, pos, size });
    };
    auto addWater = [](Zone& z, sf::Vector2f pos, sf::Vector2f size) {
        z.decorations.push_back(Decoration{ Decoration::Kind::Water, pos, size });
    };
    // A sandy beach strip -- meant to sit directly between a Water band and
    // the grass behind it (see Zone 7/Fisher's Isle and the Harbor's south
    // shore below), so the shoreline reads as an actual beach instead of
    // grass stopping dead at the water's edge.
    auto addSand = [](Zone& z, sf::Vector2f pos, sf::Vector2f size) {
        z.decorations.push_back(Decoration{ Decoration::Kind::Sand, pos, size });
    };
    auto addForageable = [](Zone& z, sf::Vector2f pos) {
        z.forageables.push_back(Forageable{ pos, true, 0.f });
    };
    auto addNpc = [](Zone& z, const std::string& nameKey, sf::Vector2f home, sf::Color color,
        std::vector<std::string> en, std::vector<std::string> zh) {
        Npc npc;
        npc.nameKey = nameKey;
        npc.home = home;
        npc.pos = home;
        npc.target = home;
        npc.color = color;
        npc.linesEn = std::move(en);
        npc.linesZh = std::move(zh);
        z.npcs.push_back(npc);
    };

    // ---------------- Zone 0: Town Square ----------------
    {
        Zone& z = zones_[0];
        z.nameKey = "zone_town_square";
        z.east = 1;  // -> Farmlands
        z.north = 2; // -> Mining District
        z.west = 3;  // -> Valley District
        z.south = 4; // -> Harbor District

        addBuilding(z, "market",     { 585.f, 130.f }, kService, bSize);
        addBuilding(z, "townhall",   { 585.f, 610.f }, kService, bSize);
        addBuilding(z, "staff",      { 220.f, 300.f }, kService, bSize);
        addBuilding(z, "doctor",     { 950.f, 300.f }, kService, bSize);
        addBuilding(z, "sleep",      { 330.f, 470.f }, kService, bSize);
        addBuilding(z, "eat",        { 500.f, 470.f }, kService, bSize);
        addBuilding(z, "storefront", { 750.f, 470.f }, kService, bSize);
        addBuilding(z, "bank",       { 220.f, 610.f }, kService, bSize);
        addBuilding(z, "warehouse",  { 950.f, 610.f }, kService, bSize);

        addPath(z, { 600.f, 40.f }, { 40.f, 740.f }); // north-south spine (toward Mining District)
        addPath(z, { 0.f, 390.f }, { 1280.f, 40.f }); // east-west spine (toward Farmlands / Valley District)

        for (float x = 20.f; x < 1260.f; x += 70.f) {
            if (x > 540.f && x < 780.f) continue; // north gap
            addTree(z, { x, 15.f });
            addTree(z, { x, 800.f });
        }
        for (float y = 90.f; y < 800.f; y += 70.f) {
            bool gapRange = (y > 330.f && y < 490.f);
            if (!gapRange) addTree(z, { 15.f, y });   // west wall, gap -> Valley District
            if (!gapRange) addTree(z, { 1260.f, y }); // east wall, gap -> Farmlands
        }
        for (int i = 0; i < 10; ++i) addBush(z, { randRange(120.f, 1160.f), randRange(120.f, 760.f) });
        for (int i = 0; i < 14; ++i) addPatch(z, { randRange(0.f, 1260.f), randRange(0.f, 800.f) }, { 46.f, 30.f });

        addNpc(z, "npc_merchant", { 430.f, 360.f }, sf::Color(210, 80, 80),
            { "Welcome to town! Start with a Wheat Farm out in the Farmlands.",
              "Ore turns into Iron Ingots at the Smelter, you know.",
              "Don't forget to eat and sleep, or your work will suffer.",
              "I hear prices swing wildly some days. Buy low, sell high!" },
            { "欢迎来到小镇!去农田那边先建个麦田农场吧。",
              "矿石在冶炼厂能炼成铁锭,你知道吗。",
              "别忘了吃饭睡觉,不然干活效率会掉的。",
              "听说市场价格有时候波动很大,低买高卖啊!" });
        addNpc(z, "npc_mayor", { 830.f, 380.f }, sf::Color(220, 180, 60),
            { "As mayor, I hereby declare this town open for business!",
              "Check the Town Hall for the Production Tree and your Achievements.",
              "Every generation leaves something behind for the next.",
              "A hundred years is a long life. Take care of yourself." },
            { "身为镇长,我在此宣布本镇正式开业!",
              "去市政厅可以查看产业树和成就哦。",
              "每一代人都会给下一代留下点什么。",
              "活到一百岁可不容易,好好照顾自己。" });
        addNpc(z, "npc_trader", { 620.f, 550.f }, sf::Color(90, 170, 200),
            { "Prices go up when you buy a lot at once, and down when you sell a lot. Spread it out!",
              "I travel between towns, but this one has the best deals lately." },
            { "一次买太多价格会涨,一次卖太多价格会跌,分批来比较划算。",
              "我在各个镇子之间跑生意,最近这镇子的行情最好。" });
        addNpc(z, "npc_guard", { 960.f, 550.f }, sf::Color(120, 120, 130),
            { "All quiet in town today. Good weather for business.",
              "Mind the trees on the way out -- can't walk through those." },
            { "今天镇上很太平,是做生意的好天气。",
              "出去的时候小心树,那个是走不过去的。" });
    }

    // ---------------- Zone 1: Farmlands ----------------
    {
        Zone& z = zones_[1];
        z.nameKey = "zone_farmlands";
        z.west = 0;  // -> Town Square
        z.south = 6; // -> Market Row

        // 4-column x 2-row grid, each column pairing a raw producer directly
        // above its own first-stage processor (lumber/sawmill, farm/bakery,
        // quarry/mason, sheep/textile) -- reads as four short production
        // lines instead of a flat list. The six farm-crop specialty stalls
        // that used to crowd a 5th/6th column here (jam/popcorn/juice/pie/
        // roast/pickling) now live in their own zone (see Zone 6: Market
        // Row below) -- this zone was up to 14 buildings, more than double
        // every other zone's 6-10.
        addBuilding(z, "lumber",  { 130.f, 180.f }, kTier1, bSize);
        addBuilding(z, "sawmill", { 370.f, 180.f }, kTier2, bSize);
        addBuilding(z, "farm",    { 610.f, 180.f }, kTier1, bSize);
        addBuilding(z, "bakery",  { 850.f, 180.f }, kTier2, bSize);
        addBuilding(z, "quarry",  { 130.f, 480.f }, kTier1, bSize);
        addBuilding(z, "mason",   { 370.f, 480.f }, kTier2, bSize);
        addBuilding(z, "sheep",   { 610.f, 480.f }, kTier1, bSize);
        addBuilding(z, "textile", { 850.f, 480.f }, kTier2, bSize);

        addPath(z, { 0.f, 390.f }, { 1280.f, 40.f });
        addPath(z, { 590.f, 40.f }, { 40.f, 740.f });

        for (float x = 20.f; x < 1260.f; x += 70.f) {
            addTree(z, { x, 15.f });
            if (x > 540.f && x < 780.f) continue; // south gap (-> Market Row)
            addTree(z, { x, 800.f });
        }
        for (float y = 90.f; y < 800.f; y += 70.f) {
            if (y > 330.f && y < 490.f) continue; // west gap (back to Town Square)
            addTree(z, { 15.f, y });
            addTree(z, { 1260.f, y });
        }
        for (int i = 0; i < 12; ++i) addBush(z, { randRange(120.f, 1160.f), randRange(120.f, 760.f) });
        for (int i = 0; i < 16; ++i) addPatch(z, { randRange(0.f, 1260.f), randRange(0.f, 800.f) }, { 50.f, 34.f });

        addNpc(z, "npc_farmer", { 570.f, 330.f }, sf::Color(120, 180, 90),
            { "These fields grow the finest wheat in the land.",
              "The Bakery turns my wheat into bread -- smells wonderful.",
              "Lumber and stone come from further out here too.",
              "The jam, pie, and popcorn stalls moved down to Market Row now -- more room for actual fields up here." },
            { "这片地里种出的小麦是全镇最好的。",
              "面包坊会把我的小麦做成面包,可香了。",
              "木材和石头在这附近也能找到。",
              "果酱、派和爆米花那些摊子都搬到集市区去了,这边总算能腾出地方种地了。" });
        addNpc(z, "npc_child", { 950.f, 330.f }, sf::Color(230, 190, 210),
            { "Have you seen how tall the wheat gets? Taller than me!",
              "Mom says the Bakery bread is best right after it's made." },
            { "你看小麦长得比我还高呢!",
              "妈妈说面包坊刚出炉的面包最好吃。" });
    }

    // ---------------- Zone 2: Mining District ----------------
    {
        Zone& z = zones_[2];
        z.nameKey = "zone_mining";
        z.south = 0; // -> Town Square
        z.north = 5; // -> Highlands District

        // 3x2 grid -- smokehouse used to sit here as a 7th building despite
        // having nothing to do with mining (a leftover placement mistake);
        // it's moved to Fisher's Isle now (see Zone 7 below), where the rest
        // of the fish-processing chain actually lives.
        addBuilding(z, "mine",       { 280.f, 180.f }, kTier1, bSize);
        addBuilding(z, "smelter",    { 600.f, 180.f }, kTier2, bSize);
        addBuilding(z, "gemshop",    { 920.f, 180.f }, kTier2, bSize);
        addBuilding(z, "blacksmith", { 280.f, 480.f }, kTier3, bSize);
        addBuilding(z, "carpenter",  { 600.f, 480.f }, kTier3, bSize);
        addBuilding(z, "tailor",     { 920.f, 480.f }, kTier3, bSize);

        addPath(z, { 590.f, 0.f }, { 40.f, 780.f });

        for (float x = 20.f; x < 1260.f; x += 70.f) {
            if (x > 540.f && x < 780.f) continue; // north gap (-> Highlands District) and south gap (-> Town Square)
            addTree(z, { x, 15.f });
            addTree(z, { x, 800.f });
        }
        for (float y = 90.f; y < 800.f; y += 70.f) { addTree(z, { 15.f, y }); addTree(z, { 1260.f, y }); }
        for (int i = 0; i < 10; ++i) addBush(z, { randRange(120.f, 1160.f), randRange(120.f, 760.f) });
        for (int i = 0; i < 14; ++i) addPatch(z, { randRange(0.f, 1260.f), randRange(0.f, 800.f) }, { 44.f, 28.f });

        addNpc(z, "npc_miner", { 600.f, 340.f }, sf::Color(150, 150, 160),
            { "Watch your step, these tunnels run deep.",
              "The Smelter and Gem Workshop both want my ore.",
              "A Blacksmith can turn iron into fine tools, if you build one." },
            { "小心脚下,这些坑道很深的。",
              "冶炼厂和宝石工坊都缺我挖的矿石。",
              "铁匠铺能把铁锭打成趁手的工具,你可以去建一个。" });
        addNpc(z, "npc_prospector", { 780.f, 330.f }, sf::Color(200, 170, 90),
            { "Fifty years I've dug these hills. Found a gem or two along the way.",
              "A Carpenter can turn planks into fine furniture -- worth a good price." },
            { "我在这片山里挖了五十年,也算挖到过几颗宝石。",
              "木匠坊能把木板做成家具,能卖不少钱。" });
    }

    // ---------------- Zone 3: Valley District ----------------
    {
        Zone& z = zones_[3];
        z.nameKey = "zone_valley";
        z.east = 0; // -> Town Square

        addBuilding(z, "orchard",    { 130.f, 180.f }, kTier1, bSize);
        addBuilding(z, "preserve",   { 370.f, 180.f }, kTier2, bSize);
        addBuilding(z, "herbgarden", { 610.f, 180.f }, kTier1, bSize);
        addBuilding(z, "apothecary", { 850.f, 180.f }, kTier2, bSize);
        addBuilding(z, "alchemist",  { 1090.f, 180.f }, kTier3, bSize);
        addBuilding(z, "goldmine",   { 130.f, 480.f }, kTier1, bSize);
        addBuilding(z, "goldsmith",  { 370.f, 480.f }, kTier2, bSize);
        addBuilding(z, "jeweler",    { 610.f, 480.f }, kTier3, bSize);
        addBuilding(z, "vineyard",   { 850.f, 480.f }, kTier1, bSize);
        addBuilding(z, "winery",     { 1090.f, 480.f }, kTier2, bSize);

        addPath(z, { 0.f, 390.f }, { 1280.f, 40.f });

        for (float x = 20.f; x < 1260.f; x += 70.f) { addTree(z, { x, 15.f }); addTree(z, { x, 800.f }); }
        for (float y = 90.f; y < 800.f; y += 70.f) {
            addTree(z, { 15.f, y }); // west wall, no gap -- this is the far edge of town
            if (y > 330.f && y < 490.f) continue; // east gap (back to Town Square)
            addTree(z, { 1260.f, y });
        }
        for (int i = 0; i < 12; ++i) addBush(z, { randRange(120.f, 1160.f), randRange(120.f, 760.f) });
        for (int i = 0; i < 16; ++i) addPatch(z, { randRange(0.f, 1260.f), randRange(0.f, 800.f) }, { 48.f, 32.f });

        addNpc(z, "npc_orchardist", { 400.f, 330.f }, sf::Color(200, 120, 90),
            { "Sweetest fruit in the valley, straight off the tree.",
              "The Preserve turns my fruit into jars that keep for months." },
            { "整个山谷里最甜的水果,刚从树上摘的。",
              "果酱坊会把我的水果做成能存好几个月的果酱。" });
        addNpc(z, "npc_herbalist", { 850.f, 330.f }, sf::Color(140, 190, 110),
            { "These herbs can cure most anything, if you know how to prepare them.",
              "An Alchemist can turn medicine into something far more potent." },
            { "这些草药如果炮制得当,几乎什么病都能治。",
              "炼金坊能把药剂炼成威力大得多的灵药。" });
        addNpc(z, "npc_prospector2", { 250.f, 550.f }, sf::Color(212, 175, 55),
            { "Gold runs deep under this valley -- slow going, but worth every ounce.",
              "A Jeweler can set gold bars into pieces worth a small fortune." },
            { "这山谷底下藏着金子,挖得慢,但每一两都值。",
              "珠宝坊能把金条打造成价值不菲的珠宝。" });
        addNpc(z, "npc_vintner", { 950.f, 550.f }, sf::Color(120, 60, 90),
            { "A good vineyard takes patience -- but the Winery makes it all worthwhile.",
              "Wine only gets more valuable as the town grows." },
            { "种葡萄急不得,不过酒庄酿出来的酒绝对值得等待。",
              "镇子越发展,葡萄酒就越值钱。" });
    }

    // ---------------- Zone 4: Harbor District ----------------
    {
        Zone& z = zones_[4];
        z.nameKey = "zone_harbor";
        z.north = 0; // -> Town Square

        addBuilding(z, "seasalt",      { 250.f, 180.f }, kTier1, bSize);
        addBuilding(z, "fishing",      { 570.f, 180.f }, kTier1, bSize);
        addBuilding(z, "pearlfarm",    { 890.f, 180.f }, kTier1, bSize);
        addBuilding(z, "shipyard",     { 250.f, 480.f }, kTier2, bSize);
        // Port sits in the lower row (the "downstream/further out" half of
        // this zone's layout, matching every other district) -- once built
        // it's what unlocks commissioning a ship and sailing to Fisher's
        // Isle (see Zone 7 below and Game::tryCommissionShip).
        addBuilding(z, "port",         { 570.f, 480.f }, kTier3, bSize);
        addBuilding(z, "pearlatelier", { 890.f, 480.f }, kTier2, bSize);

        addPath(z, { 590.f, 0.f }, { 40.f, 260.f }); // spine leading down from the Town Square gap

        for (float x = 20.f; x < 1260.f; x += 70.f) {
            if (x > 540.f && x < 780.f) continue; // north gap (-> Town Square)
            addTree(z, { x, 15.f });
            addTree(z, { x, 800.f });
        }
        for (float y = 90.f; y < 800.f; y += 70.f) { addTree(z, { 15.f, y }); addTree(z, { 1260.f, y }); }
        for (int i = 0; i < 10; ++i) addBush(z, { randRange(120.f, 1160.f), randRange(120.f, 760.f) });
        for (int i = 0; i < 14; ++i) addPatch(z, { randRange(0.f, 1260.f), randRange(0.f, 800.f) }, { 46.f, 30.f });
        // A band of animated water along the south edge, ties the dock
        // buildings to the shoreline (see Decoration::Kind::Water in drawZone).
        addWater(z, { 0.f, 700.f }, { 1280.f, 120.f });
        addSand(z, { 0.f, 676.f }, { 1280.f, 26.f }); // a beach strip right where the shoreline meets the water

        addNpc(z, "npc_shipwright", { 340.f, 380.f }, sf::Color(90, 110, 140),
            { "Give me enough planks and I'll build you a ship worth sailing.",
              "The Fishing Dock keeps me in steady work -- always something to haul.",
              "Once the Port's built, commission a ship there and you can sail out to Fisher's Isle." },
            { "只要木板给够,我就能造出一艘经得起出海的船。",
              "渔港一直有活给我干,总有东西要装卸。",
              "港口建好以后,在那边造艘船,就能出海去渔人岛了。" });
        addNpc(z, "npc_pearldiver", { 780.f, 620.f }, sf::Color(80, 160, 170),
            { "Deep water, cold water -- but a good pearl makes it worth the dive.",
              "The Pearl Atelier pays well for a clean, unblemished pearl." },
            { "水又深又冷,但捞到一颗好珍珠就都值了。",
              "珍珠工坊对干净无瑕的珍珠出价很高。" });
    }

    // ---------------- Zone 5: Highlands District ----------------
    {
        Zone& z = zones_[5];
        z.nameKey = "zone_highlands";
        z.south = 2; // -> Mining District

        addBuilding(z, "dairyfarm", { 130.f, 180.f }, kTier1, bSize);
        addBuilding(z, "creamery",  { 370.f, 180.f }, kTier2, bSize);
        addBuilding(z, "beehive",   { 610.f, 180.f }, kTier1, bSize);
        addBuilding(z, "meadery",   { 850.f, 180.f }, kTier2, bSize);
        addBuilding(z, "trapper",   { 1090.f, 180.f }, kTier1, bSize);
        addBuilding(z, "tannery",   { 130.f, 480.f }, kTier2, bSize);
        addBuilding(z, "teafield",  { 370.f, 480.f }, kTier1, bSize);
        addBuilding(z, "teahouse",  { 610.f, 480.f }, kTier2, bSize);
        addBuilding(z, "flaxfield", { 850.f, 480.f }, kTier1, bSize);
        addBuilding(z, "linenmill", { 1090.f, 480.f }, kTier2, bSize);
        // Country Gift Basket: a multi-input recipe (see BusinessType::
        // extraInputs) sourced entirely from within this district (cheese +
        // honey + tea), off in its own spot clear of the south-facing path
        // spine below.
        addBuilding(z, "giftbasket", { 1090.f, 650.f }, kTier3, bSize);

        addPath(z, { 590.f, 540.f }, { 40.f, 260.f }); // spine leading up to the Mining District gap

        for (float x = 20.f; x < 1260.f; x += 70.f) {
            addTree(z, { x, 15.f }); // north wall, no gap -- far edge of the map
            if (x > 540.f && x < 780.f) continue; // south gap (-> Mining District)
            addTree(z, { x, 800.f });
        }
        for (float y = 90.f; y < 800.f; y += 70.f) {
            addTree(z, { 15.f, y }); // west wall, no gap -- far edge of the map
            addTree(z, { 1260.f, y }); // east wall, no gap -- far edge of the map
        }
        for (int i = 0; i < 12; ++i) addBush(z, { randRange(120.f, 1160.f), randRange(120.f, 760.f) });
        for (int i = 0; i < 16; ++i) addPatch(z, { randRange(0.f, 1260.f), randRange(0.f, 800.f) }, { 48.f, 32.f });

        addNpc(z, "npc_dairymaid", { 250.f, 330.f }, sf::Color(230, 210, 180),
            { "Fresh milk every morning -- the Creamery turns it into cheese by noon.",
              "These highland pastures make for the creamiest milk in the region." },
            { "每天早上都有新鲜牛奶——乳品厂中午前就能做成奶酪。",
              "这片高原牧场产的牛奶是这一带最浓郁的。" });
        addNpc(z, "npc_beekeeper", { 730.f, 330.f }, sf::Color(230, 180, 60),
            { "Mind the bees, but don't mind them too much -- the honey's worth it.",
              "The Meadery turns my honey into something with a real kick." },
            { "小心蜜蜂,但也别太紧张——蜂蜜是值得的。",
              "蜂蜜酒坊会把我的蜂蜜酿成后劲十足的酒。" });
        addNpc(z, "npc_trapper", { 970.f, 630.f }, sf::Color(120, 90, 70),
            { "A good pelt takes patience to trap and skill to skin.",
              "The Tannery pays fair for clean leather, no questions asked." },
            { "捕到一张好兽皮需要耐心,剥皮则需要手艺。",
              "制革厂对干净的皮革出价公道,不多问。" });

        // Wild berries scattered in the open ground between the two rows of
        // buildings -- walk up and press E (see findNearbyForageable) for a
        // small bonus of a random Highlands good; each respawns on its own
        // timer a while after being picked.
        addForageable(z, { 280.f, 400.f });
        addForageable(z, { 520.f, 350.f });
        addForageable(z, { 760.f, 420.f });
        addForageable(z, { 1000.f, 380.f });
        addForageable(z, { 400.f, 600.f });
        addForageable(z, { 830.f, 610.f });
    }

    // ---------------- Zone 6: Market Row ----------------
    {
        // The six farm-crop specialty stalls that used to crowd Farmlands
        // (see Zone 1 above) -- jam/popcorn/juice/pie/roast/pickling all
        // consume one of the Farm's seasonal crops (strawberry/corn/
        // watermelon/pumpkin/sweetpotato/cabbage, see Business.cpp's
        // "Farm crop processors" section), so they read better as their own
        // bustling market square than mixed in with the open fields. A 3rd
        // row (honeyrefinery/cakeshop/artisanbakery) joined later -- see
        // Business.cpp's multi-input recipes -- making this a 3x3 grid.
        Zone& z = zones_[6];
        z.nameKey = "zone_market";
        z.north = 1; // -> Farmlands

        addBuilding(z, "jamkitchen",     { 250.f, 160.f }, kTier2, bSize);
        addBuilding(z, "popcornstand",   { 610.f, 160.f }, kTier2, bSize);
        addBuilding(z, "juicebar",       { 970.f, 160.f }, kTier2, bSize);
        addBuilding(z, "pieshop",        { 250.f, 390.f }, kTier2, bSize);
        addBuilding(z, "roaststand",     { 610.f, 390.f }, kTier2, bSize);
        addBuilding(z, "picklinghouse",  { 970.f, 390.f }, kTier2, bSize);
        addBuilding(z, "honeyrefinery",  { 250.f, 620.f }, kTier2, bSize);
        addBuilding(z, "cakeshop",       { 610.f, 620.f }, kTier3, bSize);
        addBuilding(z, "artisanbakery",  { 970.f, 620.f }, kTier3, bSize);

        addPath(z, { 590.f, 0.f }, { 40.f, 260.f }); // spine leading down from the Farmlands gap

        for (float x = 20.f; x < 1260.f; x += 70.f) {
            if (x > 540.f && x < 780.f) continue; // north gap (-> Farmlands)
            addTree(z, { x, 15.f });
            addTree(z, { x, 800.f });
        }
        for (float y = 90.f; y < 800.f; y += 70.f) { addTree(z, { 15.f, y }); addTree(z, { 1260.f, y }); }
        for (int i = 0; i < 14; ++i) addBush(z, { randRange(120.f, 1160.f), randRange(120.f, 760.f) });
        for (int i = 0; i < 18; ++i) addPatch(z, { randRange(0.f, 1260.f), randRange(0.f, 800.f) }, { 46.f, 30.f }); // busier ground clutter than the open zones -- reads as a market square, not a field

        addNpc(z, "npc_market_vendor", { 610.f, 280.f }, sf::Color(220, 140, 60),
            { "Fresh jam, hot popcorn, cold juice -- whatever the Farm's growing this season, we're selling it.",
              "Business swings with the crop calendar. Strawberries in spring, pumpkins in autumn.",
              "The Honey Refinery, Cake Shop, and Artisan Bakery down the row mix in stuff from other parts of town too." },
            { "新鲜果酱、热爆米花、冰果汁——农场这季种什么,我们就卖什么。",
              "生意跟着作物季节走,春天卖草莓,秋天就卖南瓜了。",
              "那边的蜜糖坊、蛋糕坊和手工烘焙坊,用的料是从镇上好几个地方凑来的。" });
    }

    // ---------------- Zone 7: Fisher's Isle ----------------
    {
        // Reached only by sailing from the Port in Harbor District (see
        // Game::tryCommissionShip and the Port's Sail button in
        // drawBusinessesOverlay) -- deliberately has no north/south/east/west
        // links at all, unlike every other zone. Walking to any edge here
        // just stops at the edge (see the travel code's "else" branches);
        // the only way back is interacting with the ferry below.
        Zone& z = zones_[7];
        z.nameKey = "zone_fisher_isle";

        addBuilding(z, "cannery",    { 250.f, 220.f }, kTier2, bSize);
        addBuilding(z, "smokehouse", { 610.f, 220.f }, kTier2, bSize);
        addBuilding(z, "deepsea",    { 250.f, 520.f }, kTier1, bSize);
        addBuilding(z, "sushibar",   { 610.f, 520.f }, kTier2, bSize);
        // Fisherman's Platter: a multi-input recipe (see BusinessType::
        // extraInputs) combining Cannery/Smokehouse's own output with
        // Harbor's salt.
        addBuilding(z, "fishermanplatter", { 970.f, 520.f }, kTier3, bSize);
        // The ferry home -- not a BusinessType, just an interactive world
        // object (see handleInteraction's "island_ferry" special case,
        // alongside market/staff/etc.).
        addBuilding(z, "island_ferry", { 970.f, 220.f }, kService, bSize);

        // Surrounded by sea on every side instead of the usual tree-wall
        // border -- there's no adjacent zone to wall off from here, and
        // water reads immediately as "this is an island" the way trees wouldn't.
        addWater(z, { 0.f, 0.f }, { 1280.f, 60.f });
        addWater(z, { 0.f, 760.f }, { 1280.f, 60.f });
        addWater(z, { 0.f, 0.f }, { 60.f, 820.f });
        addWater(z, { 1220.f, 0.f }, { 60.f, 820.f });
        // A ring of sand between the grass and the surrounding sea on all 4
        // sides -- an actual beach, instead of grass stopping dead at the
        // water's edge, matching the "this is an island" read the water
        // border was already going for.
        addSand(z, { 0.f, 60.f }, { 1280.f, 26.f });
        addSand(z, { 0.f, 734.f }, { 1280.f, 26.f });
        addSand(z, { 60.f, 0.f }, { 26.f, 820.f });
        addSand(z, { 1194.f, 0.f }, { 26.f, 820.f });
        for (int i = 0; i < 10; ++i) addBush(z, { randRange(120.f, 1160.f), randRange(120.f, 700.f) });
        for (int i = 0; i < 14; ++i) addPatch(z, { randRange(80.f, 1200.f), randRange(80.f, 740.f) }, { 46.f, 30.f });

        addNpc(z, "npc_islander", { 610.f, 650.f }, sf::Color(70, 150, 160),
            { "Tuna run deep out here -- takes real gear, but it's worth the trip.",
              "The Sushi Bar pays top price for a fresh catch.",
              "Ring for the ferry whenever you're ready to head back to the mainland." },
            { "这一带的金枪鱼要往深处下网才捞得到,不过很值。",
              "寿司吧对新鲜的鱼货出价最高。",
              "想回大陆的话,去渡船那边就行。" });
    }
}

bool GameWorld::collidesWithBuilding(sf::Vector2f pos, float size) const {
    sf::FloatRect testRect(pos, sf::Vector2f(size, size));
    for (const auto& b : zones_[currentZone_].buildings) {
        if (sf::FloatRect(b.position, b.size).findIntersection(testRect)) return true;
    }
    return false;
}

bool GameWorld::collidesWithWater(sf::Vector2f pos, float size) const {
    // Water was purely decorative until now -- walkable, which meant nothing
    // stopped the player (or an NPC) from wandering straight into the
    // Harbor's shoreline band or clean off Fisher's Isle into the sea
    // surrounding it, which never made sense. Same rectangle-overlap test as
    // collidesWithBuilding, just against Decoration::Kind::Water instead --
    // water now behaves like a wall the same way a building or tree does.
    sf::FloatRect testRect(pos, sf::Vector2f(size, size));
    for (const auto& d : zones_[currentZone_].decorations) {
        if (d.kind != Decoration::Kind::Water) continue;
        if (sf::FloatRect(d.position, d.size).findIntersection(testRect)) return true;
    }
    return false;
}

bool GameWorld::collidesWithTree(sf::Vector2f pos, float size) const {
    sf::Vector2f center = pos + sf::Vector2f(size / 2.f, size / 2.f);
    for (const auto& d : zones_[currentZone_].decorations) {
        if (d.kind != Decoration::Kind::Tree) continue;
        sf::Vector2f diff = center - d.position;
        float distSq = diff.x * diff.x + diff.y * diff.y;
        float minDist = kTreeRadius + size / 2.f - 6.f;
        if (distSq < minDist * minDist) return true;
    }
    return false;
}

sf::FloatRect GameWorld::achievementsButtonBounds() const {
    return sf::FloatRect({ static_cast<float>(windowSize_.x) - 168.f, 10.f }, { 158.f, 34.f });
}

sf::FloatRect GameWorld::howToPlayButtonBounds() const {
    // Sits just left of the Achievements button, same size, same row.
    return sf::FloatRect({ static_cast<float>(windowSize_.x) - 168.f - 158.f - 10.f, 10.f }, { 158.f, 34.f });
}

sf::FloatRect GameWorld::recipeBookButtonBounds() const {
    // Sits just left of How to Play, same size, same row.
    return sf::FloatRect({ static_cast<float>(windowSize_.x) - 168.f - 158.f - 10.f - 158.f - 10.f, 10.f }, { 158.f, 34.f });
}

void GameWorld::handleInteraction(const WorldBuilding& building) {
    if (interactSound_) interactSound_->play();

    // Map the building id straight to its overlay. The 12 production-tree
    // business ids all share the Businesses overlay type, but each opens it
    // focused on just that one building (focusedBusinessId_) -- walking up to
    // the Lumber Camp should only let you manage the Lumber Camp, not scroll
    // over and upgrade the Wheat Farm from the same screen.
    if (building.id == "market") { openOverlay(OverlayKind::Market); return; }
    if (building.id == "staff") { openOverlay(OverlayKind::Staff); return; }
    if (building.id == "sleep") { openOverlay(OverlayKind::Sleep); return; }
    if (building.id == "eat") { openOverlay(OverlayKind::Eat); return; }
    if (building.id == "doctor") { openOverlay(OverlayKind::Doctor); return; }
    if (building.id == "townhall") { openOverlay(OverlayKind::Tree); return; }
    if (building.id == "bank") { openOverlay(OverlayKind::Bank); return; }
    if (building.id == "warehouse") { openOverlay(OverlayKind::Warehouse); return; }
    // The ferry back to the mainland (see Zone 7 in buildZones()) -- an
    // instant round-trip, no overlay, symmetric with the Port's "Sail"
    // button that got the player out here in the first place.
    if (building.id == "island_ferry") {
        currentZone_ = kHarborZoneIndex;
        playerPos_ = kHarborReturnPos;
        setFeedback(Localization::t("returned_to_harbor"), true);
        return;
    }

    // A locked production building still opening the full Businesses screen
    // (just to show that one row greyed out) reads as "I can still interact
    // with a locked building" -- a quick world-view hint pointing at the Town
    // Hall's production tree is more useful than the whole management screen.
    if (game_.isBusinessLocked(building.id)) {
        setFeedback(Localization::t("world_locked_hint"), false);
        return;
    }
    focusedBusinessId_ = building.id;
    openOverlay(OverlayKind::Businesses);
}

namespace {
    // Key-fragment (not translated itself) for building the npc_*_season_*
    // localization keys below -- distinct from seasonKey(), which returns a
    // translatable "season_spring"-style key for display text.
    const char* seasonKeyFragment(Season s) {
        switch (s) {
        case Season::Spring: return "spring";
        case Season::Summer: return "summer";
        case Season::Autumn: return "autumn";
        default:             return "winter";
        }
    }
    // NPCs (besides Farmer Nell, who gets her own crop-aware line -- see
    // farmerSeasonLine) that comment on the current season every other visit.
    const std::vector<std::string> kSeasonalCommentNpcs = { "npc_orchardist", "npc_beekeeper", "npc_trapper", "npc_mayor" };

    // Localization key for a rebindable action's display name -- shared by
    // drawSettingsOverlay's key rows and the rebind-collision toast below.
    const char* rebindActionLabelKey(RebindAction action) {
        switch (action) {
        case RebindAction::MoveUp:       return "key_move_up";
        case RebindAction::MoveDown:     return "key_move_down";
        case RebindAction::MoveLeft:     return "key_move_left";
        case RebindAction::MoveRight:    return "key_move_right";
        case RebindAction::Interact:     return "key_interact";
        case RebindAction::QuickUpgrade: return "key_quick_upgrade";
        case RebindAction::Minimap:      return "key_minimap";
        case RebindAction::Minigame:     return "key_minigame";
        default:                         return "";
        }
    }
}

std::string GameWorld::farmerSeasonLine() const {
    Season season = game_.currentSeason();
    std::string cropId = "wheat";
    for (const auto& info : game_.businessInfos()) {
        if (info.id == "farm") { cropId = info.cropId; break; }
    }
    const CropType* crop = nullptr;
    for (const auto& c : game_.cropOptions()) {
        if (c.id == cropId) { crop = &c; break; }
    }
    std::string line = Localization::t("farmer_season_line_prefix") + Localization::t(seasonKey(season)) +
        Localization::t("farmer_season_line_mid") + Localization::t(cropId);
    if (crop && crop->favoriteSeason == season) {
        line += Localization::t("farmer_season_line_in_season");
    } else if (crop) {
        line += Localization::t("farmer_season_line_off_prefix") + Localization::t(seasonKey(crop->favoriteSeason)) +
            Localization::t("farmer_season_line_off_suffix");
    } else {
        line += ".";
    }
    return line;
}

void GameWorld::handleNpcTalk(Npc& npc) {
    if (interactSound_) interactSound_->play();
    dialogueSpeaker_ = Localization::t(npc.nameKey);

    if (npc.hasQuest) {
        std::ostringstream oss;
        oss << Localization::t("quest_intro_prefix") << formatNumber(npc.questQty) << " " << Localization::t(npc.questGoodId)
            << Localization::t("quest_reward_prefix") << formatNumber(npc.questReward) << Localization::t("quest_reward_suffix");
        dialogueText_ = oss.str();
        dialogueNpc_ = &npc;
        std::cout << "\n" << dialogueSpeaker_ << ": " << dialogueText_ << "\n";
    } else if (npc.hasDeal) {
        std::ostringstream oss;
        oss << Localization::t("deal_intro_prefix") << formatNumber(npc.dealQty) << " " << Localization::t(npc.dealGoodId)
            << Localization::t("deal_price_prefix") << formatNumber(npc.dealTotalPrice) << Localization::t("deal_price_suffix");
        dialogueText_ = oss.str();
        dialogueNpc_ = &npc;
        std::cout << "\n" << dialogueSpeaker_ << ": " << dialogueText_ << "\n";
    } else {
        dialogueNpc_ = nullptr;
        std::string line;
        // Farmer Nell comments on the season/crop every other visit instead
        // of pulling from her fixed lines that time -- lineIndex still
        // advances every visit either way, so the static lines keep cycling
        // normally on the visits that do use them.
        if (npc.nameKey == "npc_farmer" && npc.lineIndex % 2 == 1) {
            line = farmerSeasonLine();
        } else if (npc.lineIndex % 2 == 1 &&
            std::find(kSeasonalCommentNpcs.begin(), kSeasonalCommentNpcs.end(), npc.nameKey) != kSeasonalCommentNpcs.end()) {
            line = Localization::t(npc.nameKey + "_season_" + seasonKeyFragment(game_.currentSeason()));
        } else {
            auto& lines = Localization::current == Language::English ? npc.linesEn : npc.linesZh;
            if (lines.empty()) return;
            line = lines[static_cast<size_t>(npc.lineIndex) % lines.size()];
        }
        // Also printed to the console for a persistent log, but that's not
        // where a player actually looking at the game window would see it --
        // the Dialogue overlay is the real, in-window way to read this.
        std::cout << "\n" << dialogueSpeaker_ << ": " << line << "\n";
        npc.lineIndex++;
        dialogueText_ = line;
    }

    openOverlay(OverlayKind::Dialogue);
}

void GameWorld::openOverlay(OverlayKind kind) {
    currentOverlay_ = kind;
    overlayFeedback_.clear();
    overlayFeedbackTimer_ = 0.f;
    // Contracts is only reachable via a button inside the Market overlay, and
    // deliberately signs a contract for whichever good was selected there --
    // resetting the selection here would always fall back to good #0.
    if (kind != OverlayKind::Contracts) selectedGoodIndex_ = 0;
    overlayScrollOffset_ = 0.f;
    if (kind == OverlayKind::RecipeBook) recipeBookSelectedGoodId_.clear(); // always start at the grid, not wherever it was left last time
    if (kind == OverlayKind::WelcomeBack) welcomeBackExpanded_ = false; // always starts collapsed
}

void GameWorld::closeOverlay() {
    currentOverlay_ = OverlayKind::None;
    overlayFeedback_.clear();
}

void GameWorld::setFeedback(const std::string& text, bool success) {
    overlayFeedback_ = text;
    overlayFeedbackColor_ = success ? sf::Color(140, 220, 140) : sf::Color(230, 120, 110);
    overlayFeedbackTimer_ = 2.5f;
}

void GameWorld::handleTickOutcome(const TickOutcome& outcome) {
    // Queue any flavor/disaster events rolled this tick as toasts (see
    // drawEventToast) -- previously these only ever printed to a console
    // window the player isn't looking at once the SFML window is open,
    // which is exactly why "my wood keeps disappearing and I don't see why"
    // reports happen: a spoilage/warehouse-disaster event silently eats
    // stock with zero on-screen indication. Queued even on the death path
    // below (whatever happened this tick still happened).
    for (const auto& line : outcome.eventLog) eventToastQueue_.push_back(line);

    if (!outcome.died) return;
    deathNoticeMessage_ = outcome.deathMessage;
    deathNoticeGeneration_ = outcome.generation;
    currentOverlay_ = OverlayKind::DeathNotice;
    if (achievementSound_) achievementSound_->play(); // reuse the fanfare tone as a simple "something happened" cue
}

const WorldBuilding* GameWorld::findNearbyBuilding(float radius) const {
    sf::Vector2f center = playerPos_ + sf::Vector2f(kPlayerSize / 2.f, kPlayerSize / 2.f);
    const WorldBuilding* closest = nullptr;
    float bestDistSq = radius * radius;
    for (const auto& b : zones_[currentZone_].buildings) {
        // Distance to the closest point ON the building's rectangle, not to
        // its center -- buildings are 110x80, so measuring from the center
        // made the effective reach wildly inconsistent depending on approach
        // angle (dead-on to an edge worked, near a corner often didn't, even
        // though collision already puts the player right at the wall either way).
        float closestX = std::clamp(center.x, b.position.x, b.position.x + b.size.x);
        float closestY = std::clamp(center.y, b.position.y, b.position.y + b.size.y);
        sf::Vector2f d = center - sf::Vector2f(closestX, closestY);
        float distSq = d.x * d.x + d.y * d.y;
        if (distSq < bestDistSq) { bestDistSq = distSq; closest = &b; }
    }
    return closest;
}

Npc* GameWorld::findNearbyNpc(float radius) {
    sf::Vector2f center = playerPos_ + sf::Vector2f(kPlayerSize / 2.f, kPlayerSize / 2.f);
    Npc* closest = nullptr;
    float bestDistSq = radius * radius;
    for (auto& npc : zones_[currentZone_].npcs) {
        sf::Vector2f d = center - npc.pos;
        float distSq = d.x * d.x + d.y * d.y;
        if (distSq < bestDistSq) { bestDistSq = distSq; closest = &npc; }
    }
    return closest;
}

Forageable* GameWorld::findNearbyForageable(float radius) {
    sf::Vector2f center = playerPos_ + sf::Vector2f(kPlayerSize / 2.f, kPlayerSize / 2.f);
    Forageable* closest = nullptr;
    float bestDistSq = radius * radius;
    for (auto& f : zones_[currentZone_].forageables) {
        if (!f.active) continue;
        sf::Vector2f d = center - f.home;
        float distSq = d.x * d.x + d.y * d.y;
        if (distSq < bestDistSq) { bestDistSq = distSq; closest = &f; }
    }
    return closest;
}

void GameWorld::updateForaging(float dt) {
    for (auto& zone : zones_) {
        for (auto& f : zone.forageables) {
            if (f.active) continue;
            f.respawnTimer -= dt;
            if (f.respawnTimer <= 0.f) f.active = true;
        }
    }
}

void GameWorld::drawForageable(sf::RenderWindow& window, const Forageable& f) {
    if (!f.active) return;
    // A small cluster of berries -- deliberately simple/bright so it reads
    // as "walk over and press E" at a glance against the Highlands' pasture
    // greens, distinct from the Bush decoration's flat single circle.
    const sf::Vector2f dots[] = { { -5.f, 0.f }, { 5.f, 0.f }, { 0.f, -7.f } };
    for (const auto& d : dots) {
        sf::CircleShape berry(5.f);
        berry.setPosition(f.home + d - sf::Vector2f(5.f, 5.f));
        berry.setFillColor(sf::Color(210, 60, 90));
        berry.setOutlineThickness(1.f);
        berry.setOutlineColor(sf::Color(25, 20, 15));
        window.draw(berry);
    }
}

void GameWorld::updateNpcs(float dt) {
    for (auto& npc : zones_[currentZone_].npcs) {
        npc.wanderTimer -= dt;
        if (npc.wanderTimer <= 0.f) {
            npc.target = npc.home + sf::Vector2f(randRange(-50.f, 50.f), randRange(-50.f, 50.f));
            npc.wanderTimer = randRange(2.5f, 5.f);
        }
        sf::Vector2f toTarget = npc.target - npc.pos;
        float dist = std::sqrt(toTarget.x * toTarget.x + toTarget.y * toTarget.y);
        npc.isWalking = dist > 2.f; // drives drawNpc's walk bob (see drawPixelPerson)
        if (dist > 2.f) {
            sf::Vector2f step = toTarget / dist * 40.f * dt;
            // Same per-axis collision the player uses (see run()'s movement
            // block) so NPCs no longer walk through buildings/trees -- if a
            // step is blocked it's just skipped for this frame rather than
            // pathed around; the wander-retarget timer above picks a new
            // nearby target every 2.5-5s regardless, so an NPC never stays
            // stuck for long. Doesn't touch the player's own collision or
            // the E/click interaction radius (findNearbyBuilding), which are
            // both unrelated to this.
            sf::Vector2f tryX = npc.pos + sf::Vector2f(step.x, 0.f);
            if (!collidesWithBuilding(tryX, kPlayerSize) && !collidesWithTree(tryX, kPlayerSize) && !collidesWithWater(tryX, kPlayerSize)) npc.pos.x = tryX.x;
            sf::Vector2f tryY = npc.pos + sf::Vector2f(0.f, step.y);
            if (!collidesWithBuilding(tryY, kPlayerSize) && !collidesWithTree(tryY, kPlayerSize) && !collidesWithWater(tryY, kPlayerSize)) npc.pos.y = tryY.y;
        }

        // The Traveling Trader rolls bundle deals instead of fetch quests;
        // everyone else is the other way around (see the Npc::hasDeal doc
        // comment in GameWorld.h).
        if (npc.nameKey == "npc_trader") {
            if (!npc.hasDeal) {
                npc.dealRollTimer -= dt;
                if (npc.dealRollTimer <= 0.f) {
                    npc.dealRollTimer = randRange(25.f, 45.f);
                    if (randRange(0.f, 1.f) < 0.25f) generateDealFor(npc);
                }
            }
        } else if (!npc.hasQuest) {
            // Slow, low-probability roll for a new fetch quest whenever this
            // NPC doesn't already have one active.
            npc.questRollTimer -= dt;
            if (npc.questRollTimer <= 0.f) {
                npc.questRollTimer = randRange(20.f, 40.f);
                if (randRange(0.f, 1.f) < 0.15f) generateQuestFor(npc);
            }
        }
    }
}

void GameWorld::generateQuestFor(Npc& npc) {
    auto goods = game_.goodInfos();
    if (goods.empty()) return;
    size_t idx = static_cast<size_t>(randRange(0.f, static_cast<float>(goods.size())));
    if (idx >= goods.size()) idx = goods.size() - 1;
    const auto& g = goods[idx];

    npc.hasQuest = true;
    npc.questGoodId = g.id;
    npc.questQty = std::floor(randRange(5.f, 20.f));
    npc.questReward = g.price * npc.questQty * 1.3; // a markup over just selling it normally, to make accepting worthwhile
}

void GameWorld::generateDealFor(Npc& npc) {
    auto goods = game_.goodInfos();
    if (goods.empty()) return;
    size_t idx = static_cast<size_t>(randRange(0.f, static_cast<float>(goods.size())));
    if (idx >= goods.size()) idx = goods.size() - 1;
    const auto& g = goods[idx];

    npc.hasDeal = true;
    npc.dealGoodId = g.id;
    npc.dealQty = std::floor(randRange(10.f, 30.f));
    // A genuine discount (60% of the normal buy cost) rather than a markup --
    // this is the trader offloading stock cheap, the mirror image of a fetch
    // quest's markup for bringing stock TO an NPC.
    npc.dealTotalPrice = g.price * npc.dealQty * 0.6;
}

void GameWorld::drawCottageShape(sf::RenderWindow& window, const WorldBuilding& b) {
    float roofH = b.size.y * 0.38f;
    sf::Color roofColor = darken(b.labelColor, 0.55f);
    sf::Color bodyColor = darken(b.labelColor, 0.85f);

    drawPixelPanel(window, sf::Vector2f(b.position.x, b.position.y + roofH),
        sf::Vector2f(b.size.x, b.size.y - roofH), bodyColor, sf::Color(25, 20, 15), b.position);

    drawPixelRoof(window, sf::FloatRect(sf::Vector2f(b.position.x - 8.f, b.position.y - 10.f),
        sf::Vector2f(b.size.x + 16.f, roofH + 10.f)), roofColor);

    sf::Vector2f doorSize(b.size.x * 0.22f, (b.size.y - roofH) * 0.55f);
    sf::RectangleShape door(doorSize);
    door.setPosition(sf::Vector2f(b.position.x + b.size.x / 2.f - doorSize.x / 2.f, b.position.y + b.size.y - doorSize.y));
    door.setFillColor(sf::Color(60, 42, 24));
    window.draw(door);

    sf::Vector2f winSize(b.size.x * 0.16f, b.size.x * 0.16f);
    sf::RectangleShape win(winSize);
    win.setPosition(sf::Vector2f(b.position.x + b.size.x * 0.7f, b.position.y + b.size.y - roofH - winSize.y - 8.f + roofH));
    win.setFillColor(sf::Color(205, 228, 250));
    win.setOutlineThickness(1.5f);
    win.setOutlineColor(sf::Color(25, 20, 15));
    window.draw(win);

    if (smokesFrom(b.id)) {
        sf::RectangleShape chimney(sf::Vector2f(10.f, 20.f));
        chimney.setPosition(sf::Vector2f(b.position.x + b.size.x * 0.72f, b.position.y - 24.f));
        chimney.setFillColor(sf::Color(96, 80, 70));
        chimney.setOutlineThickness(1.5f);
        chimney.setOutlineColor(sf::Color(25, 20, 15));
        window.draw(chimney);
    }
}

void GameWorld::drawFarmShape(sf::RenderWindow& window, const WorldBuilding& b) {
    drawPixelPanel(window, b.position, b.size, sf::Color(107, 84, 48), sf::Color(25, 20, 15), b.position, 8.f);

    constexpr int rows = 5;
    float gap = b.size.x / static_cast<float>(rows);
    for (int i = 0; i < rows; ++i) {
        sf::Vector2f rowPos(b.position.x + gap * static_cast<float>(i) + gap * 0.2f, b.position.y + 5.f);
        drawPixelPanel(window, rowPos, sf::Vector2f(gap * 0.55f, b.size.y - 10.f),
            sf::Color(198, 182, 68), sf::Color(140, 120, 40), rowPos, 6.f);
    }
}

void GameWorld::drawMineShape(sf::RenderWindow& window, const WorldBuilding& b) {
    drawPixelMound(window, sf::FloatRect(b.position, b.size), sf::Color(114, 106, 100));

    float archW = b.size.x * 0.34f, archH = b.size.y * 0.6f;
    sf::Vector2f archPos(b.position.x + b.size.x / 2.f - archW / 2.f, b.position.y + b.size.y - archH);

    sf::Vector2f frameSize(7.f, archH + 4.f);
    drawPixelPanel(window, sf::Vector2f(archPos.x - 7.f, archPos.y - 4.f), frameSize, sf::Color(94, 62, 32), sf::Color(50, 32, 16), b.position, 5.f);
    drawPixelPanel(window, sf::Vector2f(archPos.x + archW, archPos.y - 4.f), frameSize, sf::Color(94, 62, 32), sf::Color(50, 32, 16), b.position + sf::Vector2f(1.f, 0.f), 5.f);

    drawPixelPanel(window, archPos, sf::Vector2f(archW, archH), sf::Color(18, 18, 20), sf::Color(8, 8, 10), b.position, 6.f);
}

void GameWorld::drawLumberShape(sf::RenderWindow& window, const WorldBuilding& b) {
    drawPixelPanel(window, b.position, b.size, sf::Color(96, 124, 60), sf::Color(25, 20, 15), b.position, 8.f);

    for (int i = 0; i < 3; ++i) {
        float y = b.position.y + b.size.y * 0.24f + static_cast<float>(i) * 15.f;
        sf::Vector2f logPos(b.position.x + b.size.x * 0.2f, y);
        drawPixelPanel(window, logPos, sf::Vector2f(b.size.x * 0.62f, 11.f), sf::Color(124, 84, 44), sf::Color(70, 45, 20), logPos, 5.f);

        sf::CircleShape ring(6.f);
        ring.setPosition(sf::Vector2f(b.position.x + b.size.x * 0.2f - 5.f, y - 1.f));
        ring.setFillColor(sf::Color(172, 134, 88));
        ring.setOutlineThickness(1.f);
        ring.setOutlineColor(sf::Color(70, 45, 20));
        window.draw(ring);
    }
}

void GameWorld::drawQuarryShape(sf::RenderWindow& window, const WorldBuilding& b) {
    drawPixelPanel(window, b.position, b.size, sf::Color(98, 98, 102), sf::Color(25, 20, 15), b.position, 8.f);

    const sf::Vector2f offsets[] = {
        { b.size.x * 0.18f, b.size.y * 0.28f },
        { b.size.x * 0.55f, b.size.y * 0.48f },
        { b.size.x * 0.32f, b.size.y * 0.62f },
        { b.size.x * 0.70f, b.size.y * 0.22f },
    };
    for (const auto& off : offsets) {
        sf::Vector2f chunkPos = b.position + off;
        drawPixelPanel(window, chunkPos, sf::Vector2f(16.f, 12.f), sf::Color(152, 152, 156), sf::Color(70, 70, 74), chunkPos, 5.f);
    }
}

void GameWorld::drawPastureShape(sf::RenderWindow& window, const WorldBuilding& b) {
    drawPixelPanel(window, b.position, b.size, sf::Color(102, 140, 70), sf::Color(25, 20, 15), b.position, 8.f);

    for (float x = b.position.x + 4.f; x < b.position.x + b.size.x - 2.f; x += 16.f) {
        sf::RectangleShape post(sf::Vector2f(4.f, 16.f));
        post.setPosition(sf::Vector2f(x, b.position.y + b.size.y - 16.f));
        post.setFillColor(sf::Color(150, 118, 76));
        window.draw(post);
    }
    sf::RectangleShape rail(sf::Vector2f(b.size.x - 8.f, 4.f));
    rail.setPosition(sf::Vector2f(b.position.x + 4.f, b.position.y + b.size.y - 22.f));
    rail.setFillColor(sf::Color(150, 118, 76));
    window.draw(rail);

    // A few grazing "sheep" -- a wool-white puff with a small dark head.
    const sf::Vector2f puffs[] = { { 0.30f, 0.35f }, { 0.55f, 0.50f }, { 0.72f, 0.30f } };
    for (const auto& pf : puffs) {
        sf::Vector2f p = b.position + sf::Vector2f(b.size.x * pf.x, b.size.y * pf.y);
        drawPixelBlob(window, p, 9.f, sf::Color(240, 240, 235));
        sf::CircleShape head(4.f);
        head.setPosition(sf::Vector2f(p.x - 13.f, p.y - 5.f));
        head.setFillColor(sf::Color(60, 50, 45));
        window.draw(head);
    }
}

void GameWorld::drawOrchardShape(sf::RenderWindow& window, const WorldBuilding& b) {
    drawPixelPanel(window, b.position, b.size, sf::Color(90, 130, 66), sf::Color(25, 20, 15), b.position, 8.f);

    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 3; ++col) {
            sf::Vector2f p = b.position + sf::Vector2f(
                b.size.x * (0.2f + 0.3f * static_cast<float>(col)),
                b.size.y * (0.35f + 0.4f * static_cast<float>(row)));
            sf::RectangleShape trunk(sf::Vector2f(4.f, 10.f));
            trunk.setPosition(sf::Vector2f(p.x - 2.f, p.y));
            trunk.setFillColor(sf::Color(96, 64, 32));
            window.draw(trunk);
            // Fruit-red canopy -- distinct from the world's plain green trees.
            drawPixelBlob(window, sf::Vector2f(p.x, p.y - 5.f), 9.f, sf::Color(200, 90, 70));
        }
    }
}

void GameWorld::drawHerbGardenShape(sf::RenderWindow& window, const WorldBuilding& b) {
    drawPixelPanel(window, b.position, b.size, sf::Color(74, 58, 40), sf::Color(25, 20, 15), b.position, 8.f);

    const sf::Vector2f offsets[] = {
        { b.size.x * 0.15f, b.size.y * 0.25f }, { b.size.x * 0.35f, b.size.y * 0.55f },
        { b.size.x * 0.55f, b.size.y * 0.30f }, { b.size.x * 0.75f, b.size.y * 0.60f },
        { b.size.x * 0.25f, b.size.y * 0.75f }, { b.size.x * 0.65f, b.size.y * 0.20f },
        { b.size.x * 0.85f, b.size.y * 0.40f }, { b.size.x * 0.45f, b.size.y * 0.80f },
    };
    for (const auto& off : offsets) {
        drawPixelBlob(window, b.position + off + sf::Vector2f(5.f, 5.f), 5.f, sf::Color(110, 160, 80));
    }
}

void GameWorld::drawVineyardShape(sf::RenderWindow& window, const WorldBuilding& b) {
    drawPixelPanel(window, b.position, b.size, sf::Color(107, 84, 48), sf::Color(25, 20, 15), b.position, 8.f);

    constexpr int rows = 4;
    float gap = b.size.x / static_cast<float>(rows);
    for (int i = 0; i < rows; ++i) {
        float x = b.position.x + gap * static_cast<float>(i) + gap * 0.5f;
        sf::RectangleShape post(sf::Vector2f(3.f, b.size.y - 10.f));
        post.setPosition(sf::Vector2f(x, b.position.y + 5.f));
        post.setFillColor(sf::Color(120, 90, 55));
        window.draw(post);

        for (int j = 0; j < 3; ++j) {
            sf::Vector2f center(x, b.position.y + 15.5f + static_cast<float>(j) * 16.f);
            drawPixelBlob(window, center, 3.5f, sf::Color(110, 60, 130));
        }
    }
}

void GameWorld::drawGoldMineShape(sf::RenderWindow& window, const WorldBuilding& b) {
    // Gold-tinted rock -- distinct from the Ore Mine's grey mound.
    drawPixelMound(window, sf::FloatRect(b.position, b.size), sf::Color(150, 130, 80));

    float archW = b.size.x * 0.34f, archH = b.size.y * 0.6f;
    sf::Vector2f archPos(b.position.x + b.size.x / 2.f - archW / 2.f, b.position.y + b.size.y - archH);

    sf::Vector2f frameSize(7.f, archH + 4.f);
    drawPixelPanel(window, sf::Vector2f(archPos.x - 7.f, archPos.y - 4.f), frameSize, sf::Color(94, 62, 32), sf::Color(50, 32, 16), b.position, 5.f);
    drawPixelPanel(window, sf::Vector2f(archPos.x + archW, archPos.y - 4.f), frameSize, sf::Color(94, 62, 32), sf::Color(50, 32, 16), b.position + sf::Vector2f(1.f, 0.f), 5.f);

    drawPixelPanel(window, archPos, sf::Vector2f(archW, archH), sf::Color(18, 18, 20), sf::Color(8, 8, 10), b.position, 6.f);

    const sf::Vector2f sparkles[] = { { b.size.x * 0.28f, b.size.y * 0.55f }, { b.size.x * 0.68f, b.size.y * 0.45f } };
    for (const auto& s : sparkles) {
        sf::Vector2f p = b.position + s;
        sf::ConvexShape spark(4);
        spark.setPoint(0, sf::Vector2f(p.x, p.y - 6.f));
        spark.setPoint(1, sf::Vector2f(p.x + 4.f, p.y));
        spark.setPoint(2, sf::Vector2f(p.x, p.y + 6.f));
        spark.setPoint(3, sf::Vector2f(p.x - 4.f, p.y));
        spark.setFillColor(sf::Color(255, 215, 90));
        window.draw(spark);
    }
}

// Small geometric glyph scattered a few times across a building's face --
// shared by the Field/Workshop/Dock/ServiceHall archetypes so buildings that
// share a base shape still read as visually distinct from one another. See
// the kAccent* constants and accentFor() above for how each id picks one.
void GameWorld::drawAccentGlyph(sf::RenderWindow& window, const WorldBuilding& b, int accentKind, sf::Color color) {
    const sf::Vector2f spots[] = {
        { b.size.x * 0.28f, b.size.y * 0.42f },
        { b.size.x * 0.55f, b.size.y * 0.62f },
        { b.size.x * 0.75f, b.size.y * 0.38f },
    };
    for (const auto& s : spots) {
        sf::Vector2f p = b.position + s;
        switch (accentKind) {
        case kAccentCircle: {
            sf::CircleShape c(6.f);
            c.setPosition(sf::Vector2f(p.x - 6.f, p.y - 6.f));
            c.setFillColor(color);
            c.setOutlineThickness(1.f);
            c.setOutlineColor(sf::Color(25, 20, 15));
            window.draw(c);
            break;
        }
        case kAccentDiamond: {
            sf::ConvexShape d(4);
            d.setPoint(0, sf::Vector2f(p.x, p.y - 7.f));
            d.setPoint(1, sf::Vector2f(p.x + 7.f, p.y));
            d.setPoint(2, sf::Vector2f(p.x, p.y + 7.f));
            d.setPoint(3, sf::Vector2f(p.x - 7.f, p.y));
            d.setFillColor(color);
            d.setOutlineThickness(1.f);
            d.setOutlineColor(sf::Color(25, 20, 15));
            window.draw(d);
            break;
        }
        case kAccentTriangle: {
            sf::ConvexShape t(3);
            t.setPoint(0, sf::Vector2f(p.x, p.y - 7.f));
            t.setPoint(1, sf::Vector2f(p.x + 7.f, p.y + 6.f));
            t.setPoint(2, sf::Vector2f(p.x - 7.f, p.y + 6.f));
            t.setFillColor(color);
            t.setOutlineThickness(1.f);
            t.setOutlineColor(sf::Color(25, 20, 15));
            window.draw(t);
            break;
        }
        case kAccentCross: {
            drawPixelPanel(window, sf::Vector2f(p.x - 6.f, p.y - 2.f), sf::Vector2f(12.f, 4.f), color, shade(color, -60), p, 4.f);
            drawPixelPanel(window, sf::Vector2f(p.x - 2.f, p.y - 6.f), sf::Vector2f(4.f, 12.f), color, shade(color, -60), p, 4.f);
            break;
        }
        case kAccentDoubleDot: {
            sf::CircleShape c1(3.5f);
            c1.setPosition(sf::Vector2f(p.x - 6.f, p.y - 3.f));
            c1.setFillColor(color);
            window.draw(c1);
            sf::CircleShape c2(3.5f);
            c2.setPosition(sf::Vector2f(p.x + 2.f, p.y - 3.f));
            c2.setFillColor(color);
            window.draw(c2);
            break;
        }
        default: { // kAccentBar
            drawPixelPanel(window, sf::Vector2f(p.x - 7.f, p.y - 2.5f), sf::Vector2f(14.f, 5.f), color, shade(color, -60), p, 5.f);
            break;
        }
        }
    }
}

void GameWorld::drawFieldShape(sf::RenderWindow& window, const WorldBuilding& b, int accentKind, sf::Color accentColor) {
    drawPixelPanel(window, b.position, b.size, sf::Color(92, 112, 58), sf::Color(25, 20, 15), b.position, 8.f);

    for (float x = b.position.x + 6.f; x < b.position.x + b.size.x - 4.f; x += 18.f) {
        sf::RectangleShape post(sf::Vector2f(4.f, 14.f));
        post.setPosition(sf::Vector2f(x, b.position.y + b.size.y - 14.f));
        post.setFillColor(sf::Color(140, 110, 70));
        window.draw(post);
    }

    // The 7 Field buildings used to only tell apart by their generic accent
    // glyph (a plain colored dot/diamond/triangle). Same idea as
    // drawServiceHallShape's icon signs -- one hand pixel icon per business,
    // matched to what it actually produces, mounted above the fence.
    static const std::vector<std::string> milkRows = {
        ".OOOOOO.", "OHHHHHHO", "OHBBBBHO", "OBBBBBBO",
        "OBBBBBBO", "OBBBBBBO", ".OSSSSO.", "..OOOO..",
    };
    static const std::vector<std::string> hiveRows = {
        "...OO...", "..OHHO..", ".OHBBHO.", "OHBBBBHO",
        "OBBDBDBO", "OBBBBBBO", ".OSSSSO.", "..OOOO..",
    };
    static const std::vector<std::string> peltRows = {
        "..OOOO..", ".OHHHHO.", "OHBBBBHO", "OBBDBDBO",
        "OBBBBBBO", "OHBBBBHO", ".OSSSSO.", "..OOOO..",
    };
    static const std::vector<std::string> leafRows = {
        "...OO...", "..OHHO..", ".OHBDHO.", "OHBBDBHO",
        "OBBBDBBO", ".OBBDBO.", "..OSSO..", "...OO...",
    };
    static const std::vector<std::string> flaxRows = {
        "O.O..O.O", "OHOHHOHO", "OHOBBOHO", "OHOBBOHO",
        "DDDDDDDD", "OHOBBOHO", "OHOBBOHO", ".OOOOOO.",
    };
    static const std::vector<std::string> saltRows = {
        "........", "...OO...", "..OHHO..", ".OHBBHO.",
        "OHBBBBHO", "OBBBBBBO", ".OSSSSO.", "..OOOO..",
    };
    static const std::vector<std::string> pearlRows = {
        "........", "..OOOO..", ".OHBBHO.", "OHBBBBHO",
        "OBBDDBBO", "OSBBBBSO", ".OSSSSO.", "..OOOO..",
    };
    const std::vector<std::string>* iconRows = &milkRows;
    sf::Color seed(240, 240, 235); // default: dairyfarm's milk-white
    sf::Color accent2 = shade(seed, -25);
    if (b.id == "beehive")        { iconRows = &hiveRows;  seed = sf::Color(230, 180, 60);  accent2 = sf::Color(60, 40, 15); }
    else if (b.id == "trapper")   { iconRows = &peltRows;  seed = sf::Color(150, 110, 75);  accent2 = shade(seed, -50); }
    else if (b.id == "teafield")  { iconRows = &leafRows;  seed = sf::Color(90, 150, 70);   accent2 = shade(seed, -40); }
    else if (b.id == "flaxfield") { iconRows = &flaxRows;  seed = sf::Color(180, 165, 90);  accent2 = sf::Color(110, 80, 40); }
    else if (b.id == "seasalt")   { iconRows = &saltRows;  seed = sf::Color(235, 235, 240); accent2 = shade(seed, -25); }
    else if (b.id == "pearlfarm") { iconRows = &pearlRows; seed = sf::Color(150, 170, 180); accent2 = sf::Color(250, 248, 240); }

    std::unordered_map<char, sf::Color> palette = {
        { 'O', sf::Color(25, 20, 15) },
        { 'H', shade(seed, 40) },
        { 'B', seed },
        { 'S', shade(seed, -40) },
        { 'D', accent2 },
    };
    sf::Vector2f iconSize(24.f, 24.f);
    sf::Vector2f iconPos(b.position.x + b.size.x / 2.f - iconSize.x / 2.f, b.position.y - iconSize.y - 6.f);
    drawPixelSprite(window, *iconRows, sf::FloatRect(iconPos, iconSize), palette);

    drawAccentGlyph(window, b, accentKind, accentColor);
}

// A shared lean-to roof + optional chimney, factored out of drawWorkshopShape
// so all 8 themed shapes below (and the plain Workshop fallback) build their
// body out of the same boxy silhouette and only differ in what's added in
// front of it -- keeps 9 shape functions from each re-deriving roof geometry.
void GameWorld::drawWorkshopBody(sf::RenderWindow& window, const WorldBuilding& b, bool alwaysSmokes) {
    float roofH = b.size.y * 0.22f;
    sf::Color roofColor = darken(b.labelColor, 0.5f);
    sf::Color bodyColor = darken(b.labelColor, 0.8f);

    drawPixelPanel(window, sf::Vector2f(b.position.x, b.position.y + roofH),
        sf::Vector2f(b.size.x, b.size.y - roofH), bodyColor, sf::Color(25, 20, 15), b.position);
    drawPixelPanel(window, sf::Vector2f(b.position.x - 6.f, b.position.y),
        sf::Vector2f(b.size.x + 12.f, roofH + 6.f), roofColor, sf::Color(25, 20, 15), b.position, 6.f);

    if (alwaysSmokes || smokesFrom(b.id)) {
        sf::RectangleShape chimney(sf::Vector2f(10.f, 20.f));
        chimney.setPosition(sf::Vector2f(b.position.x + b.size.x * 0.72f, b.position.y - 20.f));
        chimney.setFillColor(sf::Color(96, 80, 70));
        chimney.setOutlineThickness(1.5f);
        chimney.setOutlineColor(sf::Color(25, 20, 15));
        window.draw(chimney);
    }
}

// Bakery/kitchen family (bread, cakes, pies, jams, honey syrup, ...) -- an
// arched, glowing oven mouth out front is the one visual every one of these
// businesses has in common, whatever the specific good.
void GameWorld::drawOvenShape(sf::RenderWindow& window, const WorldBuilding& b, int accentKind, sf::Color accentColor) {
    drawWorkshopBody(window, b, true);

    float mouthW = b.size.x * 0.36f, mouthH = b.size.y * 0.4f;
    sf::Vector2f mouthPos(b.position.x + b.size.x / 2.f - mouthW / 2.f, b.position.y + b.size.y - mouthH);
    drawPixelPanel(window, mouthPos, sf::Vector2f(mouthW, mouthH), sf::Color(40, 18, 10), sf::Color(20, 10, 6), b.position, 5.f);
    sf::CircleShape glow(mouthW * 0.26f);
    glow.setPosition(sf::Vector2f(mouthPos.x + mouthW / 2.f - mouthW * 0.26f, mouthPos.y + mouthH * 0.5f - mouthW * 0.26f));
    glow.setFillColor(sf::Color(255, 150, 60, 210));
    window.draw(glow);

    drawAccentGlyph(window, b, accentKind, accentColor);
}

// Open-air market stall (popcorn, juice, tea, gift baskets, sushi) -- a
// canvas awning over a counter instead of an enclosed shed, so it reads as
// "buy here" rather than "goods are made here", matching how light these
// businesses actually are compared to a full workshop.
void GameWorld::drawStallShape(sf::RenderWindow& window, const WorldBuilding& b, int accentKind, sf::Color accentColor) {
    float counterH = b.size.y * 0.4f;
    drawPixelPanel(window, sf::Vector2f(b.position.x, b.position.y + b.size.y - counterH),
        sf::Vector2f(b.size.x, counterH), sf::Color(150, 118, 76), sf::Color(90, 65, 35), b.position, 6.f);

    // Striped canvas awning -- a wide shallow triangle in the accent color,
    // alternating with a darker shade for the "market stall" stripe look.
    sf::Color awningA = accentColor, awningB = shade(accentColor, -45);
    constexpr int kStripes = 5;
    float stripeW = (b.size.x + 16.f) / static_cast<float>(kStripes);
    float apexY = b.position.y - 26.f, baseY = b.position.y + b.size.y - counterH;
    for (int i = 0; i < kStripes; ++i) {
        float x0 = b.position.x - 8.f + stripeW * static_cast<float>(i);
        sf::ConvexShape stripe(3);
        stripe.setPoint(0, sf::Vector2f(x0, baseY));
        stripe.setPoint(1, sf::Vector2f(b.position.x + b.size.x / 2.f, apexY));
        stripe.setPoint(2, sf::Vector2f(x0 + stripeW, baseY));
        stripe.setFillColor(i % 2 == 0 ? awningA : awningB);
        stripe.setOutlineThickness(1.f);
        stripe.setOutlineColor(sf::Color(25, 20, 15));
        window.draw(stripe);
    }

    drawAccentGlyph(window, b, accentKind, accentColor);
}

// Metalworking (smelter, blacksmith, goldsmith) -- a stone furnace with a
// glowing window and an anvil out front, always smoking regardless of the
// old ad-hoc smokesFrom list (every forge runs hot).
void GameWorld::drawForgeShape(sf::RenderWindow& window, const WorldBuilding& b, int accentKind, sf::Color accentColor) {
    drawWorkshopBody(window, b, true);

    sf::Vector2f windowSize(b.size.x * 0.22f, b.size.x * 0.22f);
    sf::Vector2f windowPos(b.position.x + b.size.x * 0.6f, b.position.y + b.size.y - windowSize.y - 10.f);
    sf::RectangleShape furnaceWin(windowSize);
    furnaceWin.setPosition(windowPos);
    furnaceWin.setFillColor(sf::Color(255, 140, 40));
    furnaceWin.setOutlineThickness(2.f);
    furnaceWin.setOutlineColor(sf::Color(25, 20, 15));
    window.draw(furnaceWin);

    // Anvil -- a dark trapezoid on a short leg, sitting in front of the shed.
    sf::ConvexShape anvil(4);
    float ax = b.position.x + b.size.x * 0.22f, ay = b.position.y + b.size.y - 4.f;
    anvil.setPoint(0, sf::Vector2f(ax - 10.f, ay));
    anvil.setPoint(1, sf::Vector2f(ax + 14.f, ay));
    anvil.setPoint(2, sf::Vector2f(ax + 9.f, ay - 8.f));
    anvil.setPoint(3, sf::Vector2f(ax - 5.f, ay - 8.f));
    anvil.setFillColor(sf::Color(60, 60, 66));
    anvil.setOutlineThickness(1.5f);
    anvil.setOutlineColor(sf::Color(25, 20, 15));
    window.draw(anvil);

    drawAccentGlyph(window, b, accentKind, accentColor);
}

// Woodworking (sawmill, carpenter) -- a big circular saw blade mounted on
// the wall plus a stack of planks, distinct from the Sawmill/Carpenter's own
// output (planks/furniture) being shown as goods elsewhere in the UI.
void GameWorld::drawSawmillShape(sf::RenderWindow& window, const WorldBuilding& b, int accentKind, sf::Color accentColor) {
    drawWorkshopBody(window, b, false);

    sf::Vector2f center(b.position.x + b.size.x * 0.68f, b.position.y + b.size.y * 0.52f);
    float r = b.size.x * 0.2f;
    sf::CircleShape blade(r);
    blade.setPosition(sf::Vector2f(center.x - r, center.y - r));
    blade.setFillColor(sf::Color(180, 180, 186));
    blade.setOutlineThickness(2.f);
    blade.setOutlineColor(sf::Color(25, 20, 15));
    window.draw(blade);
    sf::CircleShape hub(r * 0.25f);
    hub.setPosition(sf::Vector2f(center.x - r * 0.25f, center.y - r * 0.25f));
    hub.setFillColor(sf::Color(90, 90, 96));
    window.draw(hub);
    // Saw teeth -- short radial spokes around the rim.
    constexpr int kTeeth = 8;
    for (int i = 0; i < kTeeth; ++i) {
        float ang = static_cast<float>(i) / static_cast<float>(kTeeth) * 6.2832f;
        sf::Vertex tooth[] = {
            sf::Vertex{ sf::Vector2f(center.x + std::cos(ang) * r * 0.6f, center.y + std::sin(ang) * r * 0.6f), sf::Color(60, 60, 64) },
            sf::Vertex{ sf::Vector2f(center.x + std::cos(ang) * r * 1.15f, center.y + std::sin(ang) * r * 1.15f), sf::Color(60, 60, 64) },
        };
        window.draw(tooth, 2, sf::PrimitiveType::Lines);
    }

    for (int i = 0; i < 3; ++i) {
        sf::Vector2f plankPos(b.position.x + b.size.x * 0.1f, b.position.y + b.size.y - 10.f - static_cast<float>(i) * 6.f);
        drawPixelPanel(window, plankPos, sf::Vector2f(b.size.x * 0.32f, 5.f), sf::Color(190, 140, 80), sf::Color(120, 85, 45), plankPos, 5.f);
    }

    drawAccentGlyph(window, b, accentKind, accentColor);
}

// Fiber/hide processing (textile, tailor, linen, tannery) -- a loom frame
// with vertical warp threads and a hanging bolt of material in the accent
// color, standing in for cloth or hides depending on which business it is.
void GameWorld::drawFiberShape(sf::RenderWindow& window, const WorldBuilding& b, int accentKind, sf::Color accentColor) {
    drawWorkshopBody(window, b, false);

    float frameW = b.size.x * 0.4f, frameH = b.size.y * 0.55f;
    sf::Vector2f framePos(b.position.x + b.size.x * 0.08f, b.position.y + b.size.y - frameH - 6.f);
    sf::RectangleShape postL(sf::Vector2f(3.f, frameH));
    postL.setPosition(framePos);
    postL.setFillColor(sf::Color(110, 80, 50));
    window.draw(postL);
    sf::RectangleShape postR(postL);
    postR.setPosition(sf::Vector2f(framePos.x + frameW, framePos.y));
    window.draw(postR);
    for (int i = 0; i < 4; ++i) {
        float x = framePos.x + frameW * (0.2f + 0.22f * static_cast<float>(i));
        sf::RectangleShape thread(sf::Vector2f(2.f, frameH));
        thread.setPosition(sf::Vector2f(x, framePos.y));
        thread.setFillColor(sf::Color(220, 215, 200));
        window.draw(thread);
    }
    drawPixelPanel(window, sf::Vector2f(framePos.x, framePos.y - 10.f), sf::Vector2f(frameW, 10.f),
        accentColor, shade(accentColor, -50), b.position, 5.f);

    drawAccentGlyph(window, b, accentKind, accentColor);
}

// Stonecutting/gem workshop (mason, gemshop, jeweler, pearl atelier) -- a
// workbench out front with a large faceted gem in the accent color, the
// closest thing to a "shopfront display" any Workshop-family building gets.
void GameWorld::drawMasonGemShape(sf::RenderWindow& window, const WorldBuilding& b, int accentKind, sf::Color accentColor) {
    drawWorkshopBody(window, b, false);

    sf::Vector2f benchPos(b.position.x + b.size.x * 0.18f, b.position.y + b.size.y - 14.f);
    drawPixelPanel(window, benchPos, sf::Vector2f(b.size.x * 0.5f, 8.f), sf::Color(120, 90, 60), sf::Color(70, 50, 30), b.position, 5.f);

    sf::Vector2f gemCenter(benchPos.x + b.size.x * 0.25f, benchPos.y - 10.f);
    sf::ConvexShape gem(6);
    const float gr = 12.f;
    for (int i = 0; i < 6; ++i) {
        float ang = static_cast<float>(i) / 6.f * 6.2832f - 1.5708f;
        gem.setPoint(static_cast<std::size_t>(i), sf::Vector2f(gemCenter.x + std::cos(ang) * gr, gemCenter.y + std::sin(ang) * gr * 0.85f));
    }
    gem.setFillColor(accentColor);
    gem.setOutlineThickness(1.5f);
    gem.setOutlineColor(sf::Color(25, 20, 15));
    window.draw(gem);
    sf::CircleShape sparkle(2.5f);
    sparkle.setPosition(sf::Vector2f(gemCenter.x - 4.f, gemCenter.y - 6.f));
    sparkle.setFillColor(sf::Color(255, 255, 255, 200));
    window.draw(sparkle);

    drawAccentGlyph(window, b, accentKind, accentColor);
}

// Brewing/alchemy (winery, meadery, alchemist, creamery, apothecary) --
// stacked barrels plus a bubbling still, the one visual all five share
// despite making very different final goods.
void GameWorld::drawBreweryShape(sf::RenderWindow& window, const WorldBuilding& b, int accentKind, sf::Color accentColor) {
    drawWorkshopBody(window, b, false);

    for (int i = 0; i < 2; ++i) {
        sf::Vector2f barrelPos(b.position.x + b.size.x * 0.12f + static_cast<float>(i) * 20.f, b.position.y + b.size.y - 22.f);
        drawPixelPanel(window, barrelPos, sf::Vector2f(16.f, 20.f), sf::Color(150, 108, 60), sf::Color(90, 60, 30), barrelPos, 4.f);
    }

    sf::Vector2f vatPos(b.position.x + b.size.x * 0.58f, b.position.y + b.size.y - 26.f);
    drawPixelPanel(window, vatPos, sf::Vector2f(24.f, 22.f), sf::Color(120, 122, 128), sf::Color(60, 60, 66), b.position, 5.f);
    for (int i = 0; i < 3; ++i) {
        float bx = vatPos.x + 6.f + static_cast<float>(i) * 6.f;
        sf::CircleShape bubble(2.2f);
        bubble.setPosition(sf::Vector2f(bx, vatPos.y - 4.f - static_cast<float>(i) * 3.f));
        bubble.setFillColor(accentColor);
        window.draw(bubble);
    }

    drawAccentGlyph(window, b, accentKind, accentColor);
}

// Smokehouse -- a few hanging smoked fish under the eave, plus a chimney
// that's always lit (this is a one-business "theme" but distinct enough
// from every other shed to be worth its own shape rather than reusing Oven).
void GameWorld::drawSmokehouseShape(sf::RenderWindow& window, const WorldBuilding& b, int accentKind, sf::Color accentColor) {
    drawWorkshopBody(window, b, true);

    sf::Color fishColor(150, 130, 90);
    for (int i = 0; i < 3; ++i) {
        float x = b.position.x + b.size.x * (0.25f + 0.25f * static_cast<float>(i));
        float hookY = b.position.y + b.size.y * 0.22f;
        sf::Vertex hook[] = { sf::Vertex{ sf::Vector2f(x, hookY - 6.f), sf::Color(60, 60, 64) },
                               sf::Vertex{ sf::Vector2f(x, hookY), sf::Color(60, 60, 64) } };
        window.draw(hook, 2, sf::PrimitiveType::Lines);
        sf::ConvexShape fish(4);
        fish.setPoint(0, sf::Vector2f(x - 7.f, hookY + 4.f));
        fish.setPoint(1, sf::Vector2f(x, hookY + 1.f));
        fish.setPoint(2, sf::Vector2f(x + 7.f, hookY + 4.f));
        fish.setPoint(3, sf::Vector2f(x, hookY + 12.f));
        fish.setFillColor(fishColor);
        fish.setOutlineThickness(1.f);
        fish.setOutlineColor(sf::Color(25, 20, 15));
        window.draw(fish);
    }

    drawAccentGlyph(window, b, accentKind, accentColor);
}

void GameWorld::drawWorkshopShape(sf::RenderWindow& window, const WorldBuilding& b, int accentKind, sf::Color accentColor) {
    // Fallback shape only -- every id that used to land here now has one of
    // the 8 themed shapes above instead (see isOvenId etc. and drawBuilding's
    // dispatch chain). Kept as a safety net for any future business that
    // doesn't fit one of those 8 groups.
    drawWorkshopBody(window, b, smokesFrom(b.id));
    drawAccentGlyph(window, b, accentKind, accentColor);
}

void GameWorld::drawDockShape(sf::RenderWindow& window, const WorldBuilding& b, int accentKind, sf::Color accentColor) {
    drawPixelPanel(window, b.position, sf::Vector2f(b.size.x, b.size.y * 0.72f), sf::Color(126, 92, 56), sf::Color(25, 20, 15), b.position, 6.f);

    for (float x = b.position.x + 8.f; x < b.position.x + b.size.x - 4.f; x += 14.f) {
        sf::RectangleShape plank(sf::Vector2f(2.f, b.size.y * 0.72f - 6.f));
        plank.setPosition(sf::Vector2f(x, b.position.y + 3.f));
        plank.setFillColor(sf::Color(100, 72, 42));
        window.draw(plank);
    }

    drawPixelPanel(window, sf::Vector2f(b.position.x, b.position.y + b.size.y * 0.72f),
        sf::Vector2f(b.size.x, b.size.y * 0.28f), sf::Color(60, 110, 150), sf::Color(25, 20, 15), b.position, 6.f);

    // The 7 Dock buildings shared the same deck+water look, distinguished
    // only by their accent glyph -- last of the archetype groups to get its
    // own icon sign per business (see drawServiceHallShape/drawFieldShape).
    static const std::vector<std::string> fishRows = {
        "........", "...OOO..", "..OHBO..", ".OHBBBO.",
        "OHBBBBBO", ".OSBBSO.", "..OSSO..", "...OO...",
    };
    static const std::vector<std::string> shipRows = {
        "...O....", "...O....", "..OHO...", "..OHO...",
        ".OHHHO..", "OBBBBBBO", "OSSSSSSO", ".OOOOOO.",
    };
    static const std::vector<std::string> canRows = {
        ".OOOOOO.", "OHHHHHHO", "OHBBBBHO", "OBBBBBBO",
        "OBBBBBBO", "OBBBBBBO", "OSSSSSSO", ".OOOOOO.",
    };
    static const std::vector<std::string> anchorRows = {
        "...OO...", "..OHHO..", "...BB...", "...BB...",
        "...BB...", "O..BB..O", "OH.BB.HO", ".OOBBOO.",
    };
    const std::vector<std::string>* iconRows = &fishRows;
    sf::Color seed(90, 140, 190); // default: fishing's own blue-grey fish
    if (b.id == "shipyard")           { iconRows = &shipRows;   seed = sf::Color(225, 225, 230); }
    else if (b.id == "cannery")       { iconRows = &canRows;    seed = sf::Color(160, 165, 175); }
    else if (b.id == "port")          { iconRows = &anchorRows; seed = sf::Color(90, 150, 195); }
    else if (b.id == "deepsea")       { iconRows = &fishRows;   seed = sf::Color(50, 95, 150); }
    else if (b.id == "fishermanplatter") { iconRows = &fishRows; seed = sf::Color(90, 170, 165); }
    else if (b.id == "island_ferry")  { iconRows = &shipRows;   seed = sf::Color(200, 205, 210); }

    std::unordered_map<char, sf::Color> palette = {
        { 'O', sf::Color(25, 20, 15) },
        { 'H', shade(seed, 45) },
        { 'B', seed },
        { 'S', shade(seed, -40) },
    };
    sf::Vector2f iconSize(24.f, 24.f);
    sf::Vector2f iconPos(b.position.x + b.size.x / 2.f - iconSize.x / 2.f, b.position.y - iconSize.y - 6.f);
    drawPixelSprite(window, *iconRows, sf::FloatRect(iconPos, iconSize), palette);

    drawAccentGlyph(window, b, accentKind, accentColor);
}

void GameWorld::drawServiceHallShape(sf::RenderWindow& window, const WorldBuilding& b, int accentKind, sf::Color accentColor) {
    sf::Color bodyColor = darken(b.labelColor, 0.82f);
    drawPixelPanel(window, b.position, b.size, bodyColor, sf::Color(25, 20, 15), b.position);

    // A row of pillars along the front, civic-hall style.
    for (int i = 0; i < 4; ++i) {
        float x = b.position.x + b.size.x * (0.15f + 0.23f * static_cast<float>(i));
        sf::RectangleShape pillar(sf::Vector2f(8.f, b.size.y - 10.f));
        pillar.setPosition(sf::Vector2f(x, b.position.y + 6.f));
        pillar.setFillColor(b.labelColor);
        window.draw(pillar);
    }

    if (b.id == "townhall") {
        // Keeps the original flagpole -- the one ServiceHall building an
        // actual civic flag still makes sense for.
        sf::RectangleShape pole(sf::Vector2f(3.f, 22.f));
        pole.setPosition(sf::Vector2f(b.position.x + b.size.x * 0.5f, b.position.y - 20.f));
        pole.setFillColor(sf::Color(80, 70, 60));
        window.draw(pole);
        sf::ConvexShape flag(3);
        sf::Vector2f fp(b.position.x + b.size.x * 0.5f + 3.f, b.position.y - 20.f);
        flag.setPoint(0, fp);
        flag.setPoint(1, sf::Vector2f(fp.x + 14.f, fp.y + 4.f));
        flag.setPoint(2, sf::Vector2f(fp.x, fp.y + 8.f));
        flag.setFillColor(accentColor);
        window.draw(flag);
        return;
    }

    // The other 5 ServiceHall buildings (storefront/market/staff/bank/
    // warehouse) used to all get that same flag too, just recolored -- one
    // more spot that read as uniform despite being the buildings right in
    // the middle of Town Square. A small icon sign mounted above the
    // doorway instead, one shape per business, fixes that.
    static const std::vector<std::string> coinRows = {
        "..OOOO..", ".OHHHHO.", "OHBBABHO", "OBBAABBO",
        "OBBAABBO", "OHBBABHO", ".OSSSSO.", "..OOOO..",
    };
    static const std::vector<std::string> basketRows = {
        ".O....O.", "OOHHHHOO", "OHBBBBHO", "OBBBBBBO",
        "OBBDBDBO", ".OSSSSO.", "..OOOO..",
    };
    static const std::vector<std::string> badgeRows = {
        "....O....", "...OHO...", "..OHHHO..", ".OHHHHHO.",
        "OOHHHHHOO", ".OHHHHHO.", "..OSSSO..", "...OOO...",
    };
    static const std::vector<std::string> vaultRows = {
        "..OOOO..", ".OHHHHO.", "OHBBDBHO", "OBBBDBBO",
        "OBDDDDBO", "OHBBDBHO", ".OSSSSO.", "..OOOO..",
    };
    static const std::vector<std::string> crateRows = {
        "OOOOOOOO", "OHBDBBHO", "OBBDBBBO", "DDDDDDDD",
        "OBBDBBBO", "OBBDBBBO", "OSBDBSSO", "OOOOOOOO",
    };
    const std::vector<std::string>* iconRows = &coinRows;
    sf::Color seed(220, 190, 90); // gold -- the default (storefront: coin)
    if (b.id == "market")        { iconRows = &basketRows; seed = sf::Color(190, 140, 80); }
    else if (b.id == "staff")    { iconRows = &badgeRows;  seed = accentColor; }
    else if (b.id == "bank")     { iconRows = &vaultRows;  seed = sf::Color(200, 200, 210); }
    else if (b.id == "warehouse"){ iconRows = &crateRows;  seed = sf::Color(170, 125, 75); }

    std::unordered_map<char, sf::Color> palette = {
        { 'O', sf::Color(25, 20, 15) },
        { 'H', shade(seed, 40) },
        { 'B', seed },
        { 'S', shade(seed, -40) },
        { 'A', sf::Color(255, 245, 210) },
        { 'D', shade(seed, -25) },
    };
    sf::Vector2f iconSize(26.f, 22.f);
    sf::Vector2f iconPos(b.position.x + b.size.x / 2.f - iconSize.x / 2.f, b.position.y - iconSize.y - 6.f);
    drawPixelSprite(window, *iconRows, sf::FloatRect(iconPos, iconSize), palette);
}

void GameWorld::drawLockOverlay(sf::RenderWindow& window, const WorldBuilding& b) {
    sf::RectangleShape dim(b.size);
    dim.setPosition(b.position);
    dim.setFillColor(sf::Color(10, 10, 15, 150));
    window.draw(dim);

    sf::Vector2f center = b.position + b.size / 2.f;
    sf::RectangleShape lockBody(sf::Vector2f(20.f, 16.f));
    lockBody.setPosition(sf::Vector2f(center.x - 10.f, center.y - 2.f));
    lockBody.setFillColor(sf::Color(225, 205, 110));
    lockBody.setOutlineThickness(1.5f);
    lockBody.setOutlineColor(sf::Color(40, 30, 10));
    window.draw(lockBody);

    sf::CircleShape shackle(8.f);
    shackle.setFillColor(sf::Color::Transparent);
    shackle.setOutlineThickness(3.f);
    shackle.setOutlineColor(sf::Color(225, 205, 110));
    shackle.setPosition(sf::Vector2f(center.x - 8.f, center.y - 18.f));
    window.draw(shackle);
}

void GameWorld::drawEmptyPlotShape(sf::RenderWindow& window, const WorldBuilding& b, const ConstructionInfo& ci) {
    // Bare, muted ground -- deliberately duller/flatter than any built shape
    // (or the construction site below) so an unstarted plot reads as
    // "nothing here yet" at a glance rather than looking broken.
    sf::RectangleShape ground(b.size);
    ground.setPosition(b.position);
    ground.setFillColor(sf::Color(70, 78, 62));
    ground.setOutlineThickness(1.5f);
    ground.setOutlineColor(sf::Color(45, 50, 40));
    window.draw(ground);

    // A small signboard in the corner of the plot naming what's meant to go
    // here (plus the material shortlist, so a walk-by tells you what to
    // stockpile without opening the full panel) -- separate from (and in
    // addition to) the floating name label drawBuilding draws above every
    // building regardless of state. Tall enough for up to 3 material lines
    // (the most any recipe has -- see BusinessManager::buildMaterialsFor).
    float boardH = 22.f + static_cast<float>(ci.materials.size()) * 13.f;
    sf::Vector2f postPos(b.position.x + 10.f, b.position.y + b.size.y - (boardH + 10.f));
    sf::RectangleShape post(sf::Vector2f(5.f, boardH + 6.f));
    post.setPosition(postPos);
    post.setFillColor(sf::Color(94, 62, 32));
    window.draw(post);

    sf::RectangleShape board(sf::Vector2f(88.f, boardH));
    board.setPosition(sf::Vector2f(postPos.x - 6.f, postPos.y));
    board.setFillColor(sf::Color(196, 168, 118));
    board.setOutlineThickness(1.5f);
    board.setOutlineColor(sf::Color(94, 62, 32));
    window.draw(board);

    if (fontLoaded_) {
        std::string text = Localization::t("construction_plot_sign_prefix") + Localization::t(b.labelKey);
        sf::Text label(font_, toSfString(text), 10);
        label.setFillColor(sf::Color(40, 30, 15));
        label.setPosition(sf::Vector2f(board.getPosition().x + 4.f, board.getPosition().y + 4.f));
        window.draw(label);

        float lineY = board.getPosition().y + 20.f;
        for (const auto& m : ci.materials) {
            std::ostringstream line;
            line << Localization::t(m.goodId) << " " << formatNumber(m.have) << "/" << formatNumber(m.required);
            sf::Text matLine(font_, toSfString(line.str()), 9);
            matLine.setFillColor(m.have >= m.required ? sf::Color(30, 90, 30) : sf::Color(110, 30, 30));
            matLine.setPosition(sf::Vector2f(board.getPosition().x + 4.f, lineY));
            window.draw(matLine);
            lineY += 13.f;
        }
    }
}

void GameWorld::drawConstructionSiteShape(sf::RenderWindow& window, const WorldBuilding& b, const ConstructionInfo& ci) {
    // Excavated, earthy ground -- distinct from both the empty plot's dull
    // grey-green above and any finished building's own colors.
    drawPixelPanel(window, b.position, b.size, sf::Color(120, 96, 62), sf::Color(70, 55, 30), b.position, 7.f);

    // A crossed scaffold -- flat lines, no gradients, matching every other
    // building shape's look.
    sf::Color beam(150, 110, 60);
    sf::Vertex scaffold[] = {
        sf::Vertex{ sf::Vector2f(b.position.x + 6.f, b.position.y + b.size.y - 6.f), beam },
        sf::Vertex{ sf::Vector2f(b.position.x + b.size.x - 6.f, b.position.y + 6.f), beam },
        sf::Vertex{ sf::Vector2f(b.position.x + 6.f, b.position.y + 6.f), beam },
        sf::Vertex{ sf::Vector2f(b.position.x + b.size.x - 6.f, b.position.y + b.size.y - 6.f), beam },
    };
    window.draw(scaffold, 4, sf::PrimitiveType::Lines);

    // A small pile of material blocks in the corner.
    const sf::Color materialColors[3] = { sf::Color(196, 168, 118), sf::Color(150, 150, 156), sf::Color(120, 80, 50) };
    for (int i = 0; i < 3; ++i) {
        sf::RectangleShape chunk(sf::Vector2f(12.f, 10.f));
        chunk.setPosition(sf::Vector2f(b.position.x + 8.f + static_cast<float>(i) * 14.f, b.position.y + b.size.y - 18.f));
        chunk.setFillColor(materialColors[i]);
        window.draw(chunk);
    }

    // Progress bar along the bottom edge of the site itself (not above the
    // building) so it never collides with the floating name label
    // drawBuilding already draws above every building's footprint.
    float barW = b.size.x - 12.f, barH = 6.f;
    sf::Vector2f barPos(b.position.x + 6.f, b.position.y + b.size.y - 8.f);
    sf::RectangleShape barBg(sf::Vector2f(barW, barH));
    barBg.setPosition(barPos);
    barBg.setFillColor(sf::Color(30, 30, 34));
    window.draw(barBg);

    double totalDays = std::max(1, ci.totalDays);
    float progress = static_cast<float>(std::clamp(1.0 - (ci.daysRemaining / totalDays), 0.0, 1.0));
    sf::RectangleShape barFill(sf::Vector2f(barW * progress, barH));
    barFill.setPosition(barPos);
    barFill.setFillColor(sf::Color(232, 212, 120));
    window.draw(barFill);

    if (fontLoaded_) {
        int daysLeft = static_cast<int>(std::ceil(ci.daysRemaining));
        std::string text = Localization::t("construction_site_days_left_prefix") + std::to_string(daysLeft) + Localization::t("construction_site_days_left_suffix");
        sf::Text label(font_, toSfString(text), 12);
        sf::FloatRect bounds = label.getLocalBounds();
        label.setPosition(sf::Vector2f(b.position.x + b.size.x / 2.f - bounds.size.x / 2.f - bounds.position.x, b.position.y + b.size.y / 2.f - 8.f));
        label.setFillColor(sf::Color(255, 240, 210));
        label.setOutlineColor(sf::Color::Black);
        label.setOutlineThickness(2.f);
        window.draw(label);
    }
}

void GameWorld::drawBuilding(sf::RenderWindow& window, const WorldBuilding& b) {
    // Prerequisite lock (see BusinessManager::isLocked) takes priority over
    // everything below and is unchanged from before this feature: a
    // tier-2/3 building whose tier-1 source isn't built yet still just gets
    // its full normal shape dimmed with a padlock, not a plot/site.
    bool locked = game_.isBusinessLocked(b.id);

    // First-build construction (see Business::constructionDaysRemaining):
    // requiresConstruction is only ever true here for an unlocked business
    // still at level 0 that isn't one of the 4 free starters (see
    // Game::businessConstructionInfo's early-outs) -- draw the empty-plot
    // or construction-site shape instead of the real building, then skip
    // straight to the shared name label below.
    ConstructionInfo ci = locked ? ConstructionInfo{} : game_.businessConstructionInfo(b.id);
    if (ci.requiresConstruction) {
        if (ci.inProgress) drawConstructionSiteShape(window, b, ci);
        else drawEmptyPlotShape(window, b, ci);
    } else {
        if (b.id == "farm") drawFarmShape(window, b);
        else if (b.id == "mine") drawMineShape(window, b);
        else if (b.id == "lumber") drawLumberShape(window, b);
        else if (b.id == "quarry") drawQuarryShape(window, b);
        else if (b.id == "sheep") drawPastureShape(window, b);
        else if (b.id == "orchard") drawOrchardShape(window, b);
        else if (b.id == "herbgarden") drawHerbGardenShape(window, b);
        else if (b.id == "vineyard") drawVineyardShape(window, b);
        else if (b.id == "goldmine") drawGoldMineShape(window, b);
        else if (isDockId(b.id)) { BuildingAccent a = accentFor(b.id); drawDockShape(window, b, a.kind, a.color); }
        else if (isFieldId(b.id)) { BuildingAccent a = accentFor(b.id); drawFieldShape(window, b, a.kind, a.color); }
        else if (isServiceHallId(b.id)) { BuildingAccent a = accentFor(b.id); drawServiceHallShape(window, b, a.kind, a.color); }
        else if (b.id == "sleep" || b.id == "eat" || b.id == "doctor") drawCottageShape(window, b);
        // The 33 remaining processors used to all land on the one generic
        // Workshop shape below -- now split into 8 themed sub-archetypes by
        // what they actually make (see isOvenId etc. above).
        else if (isOvenId(b.id)) { BuildingAccent a = accentFor(b.id); drawOvenShape(window, b, a.kind, a.color); }
        else if (isStallId(b.id)) { BuildingAccent a = accentFor(b.id); drawStallShape(window, b, a.kind, a.color); }
        else if (isForgeId(b.id)) { BuildingAccent a = accentFor(b.id); drawForgeShape(window, b, a.kind, a.color); }
        else if (isSawmillId(b.id)) { BuildingAccent a = accentFor(b.id); drawSawmillShape(window, b, a.kind, a.color); }
        else if (isFiberId(b.id)) { BuildingAccent a = accentFor(b.id); drawFiberShape(window, b, a.kind, a.color); }
        else if (isMasonGemId(b.id)) { BuildingAccent a = accentFor(b.id); drawMasonGemShape(window, b, a.kind, a.color); }
        else if (isBreweryId(b.id)) { BuildingAccent a = accentFor(b.id); drawBreweryShape(window, b, a.kind, a.color); }
        else if (isSmokehouseId(b.id)) { BuildingAccent a = accentFor(b.id); drawSmokehouseShape(window, b, a.kind, a.color); }
        else { BuildingAccent a = accentFor(b.id); drawWorkshopShape(window, b, a.kind, a.color); } // safety net -- shouldn't be reachable

        if (locked) drawLockOverlay(window, b);
    }

    if (fontLoaded_) {
        // The Farm alone shows which crop is currently planted -- otherwise
        // the sign stays "Farm" (or a stale "Wheat Farm"-style name) even
        // after switching to strawberries/corn/watermelon/etc, which reads
        // as a labeling mistake when the field itself is clearly a
        // different crop. Every other building's label is just its own
        // fixed name.
        std::string label = Localization::t(b.labelKey);
        if (b.id == "farm") {
            label += " (" + Localization::t(game_.farmCropId()) + ")";
        }
        // Field/Dock/ServiceHall (besides townhall, which keeps its flag
        // instead) mount a ~24px icon sign above the roofline (see
        // drawFieldShape/drawDockShape/drawServiceHallShape) -- the label's
        // usual -26 offset sits right on top of that sign, so push it
        // further up whenever one of those was actually drawn (not for an
        // unbuilt plot/construction site, which has no sign to clear).
        bool hasIconSign = !ci.requiresConstruction &&
            (isFieldId(b.id) || isDockId(b.id) || (isServiceHallId(b.id) && b.id != "townhall"));
        float labelOffsetY = hasIconSign ? 48.f : 26.f;
        sf::Text text(font_, toSfString(label), 13);
        sf::FloatRect bounds = text.getLocalBounds();
        text.setPosition(sf::Vector2f(b.position.x + b.size.x / 2.f - bounds.size.x / 2.f, b.position.y - labelOffsetY));
        text.setFillColor(b.labelColor);
        text.setOutlineColor(sf::Color::Black);
        text.setOutlineThickness(2.f);
        text.setStyle(sf::Text::Bold);
        window.draw(text);
    }
}

void GameWorld::drawTree(sf::RenderWindow& window, sf::Vector2f pos) {
    // Canopy color follows the season -- fresh light green in Spring, the
    // original deep green as Summer's full-bloom baseline, warm orange/red
    // in Autumn, pale/bare in Winter -- now shaded into highlight/base/shadow
    // pixel bands (see the H/B/S roles below) instead of one flat fill.
    sf::Color canopyFill, canopyOutline;
    switch (game_.currentSeason()) {
    case Season::Spring: canopyFill = sf::Color(96, 178, 98);  canopyOutline = sf::Color(50, 130, 52);  break;
    case Season::Autumn: canopyFill = sf::Color(198, 122, 42); canopyOutline = sf::Color(140, 80, 20);  break;
    case Season::Winter: canopyFill = sf::Color(222, 226, 230); canopyOutline = sf::Color(150, 150, 160); break;
    default:             canopyFill = sf::Color(34, 104, 44);  canopyOutline = sf::Color(16, 60, 24);   break; // Summer
    }
    // 'L' scatters a few mid-tone leaf-cluster pixels through the canopy
    // (between H and B) so it reads as clumpy foliage rather than one flat
    // shaded blob -- fixed positions, not per-frame random, so it doesn't
    // flicker as the player walks past.
    static const std::vector<std::string> rows = {
        "....OOOO....",
        "...OHHLHO...",
        "..OHHBBLHO..",
        ".OLHBBBBBHO.",
        "OHBBBLBBBHLO",
        "OBBBBBBLBBBO",
        "OBBSBBBSBBBO",
        "OSSBBBBBSSO.",
        ".OSSSSSSSO..",
        "..OOOOOOO...",
        "....OTTO....",
        "....OTtO....",
        "....OTTO....",
        "....OTtO....",
        "....OOOO....",
    };
    std::unordered_map<char, sf::Color> palette = {
        { 'O', canopyOutline },
        { 'H', shade(canopyFill, 35) },
        { 'B', canopyFill },
        { 'L', shade(canopyFill, 14) },
        { 'S', shade(canopyFill, -35) },
        { 'T', sf::Color(96, 64, 32) },
        { 't', sf::Color(78, 50, 24) }, // bark shading -- alternates with 'T' down the trunk instead of one flat brown column
    };
    // kTreeRadius*2 wide, tall enough for canopy+trunk -- trunk bottom
    // (the old trunk rect's own bottom edge) still lands at pos.y + 14,
    // same footprint collidesWithTree/the interact radius already assume.
    sf::Vector2f spriteSize(kTreeRadius * 2.25f, kTreeRadius * 2.9f);
    sf::Vector2f topLeft(pos.x - spriteSize.x / 2.f, pos.y + 14.f - spriteSize.y);
    // A shadow under the trunk grounds the sprite -- without it a flat pixel
    // cutout on top of the grass reads as pasted-on/floating rather than an
    // actual tree standing in the world.
    drawGroundShadow(window, sf::Vector2f(pos.x, pos.y + 14.f), kTreeRadius * 0.9f);
    drawPixelSprite(window, rows, sf::FloatRect(topLeft, spriteSize), palette);
}

void GameWorld::drawBush(sf::RenderWindow& window, sf::Vector2f pos) {
    sf::Color fill;
    switch (game_.currentSeason()) {
    case Season::Spring: fill = sf::Color(122, 192, 112); break;
    case Season::Autumn: fill = sf::Color(172, 112, 50);  break;
    case Season::Winter: fill = sf::Color(202, 206, 212); break;
    default:             fill = sf::Color(54, 132, 62);   break; // Summer
    }
    static const std::vector<std::string> rows = {
        "..OOOO..",
        ".OHHHHO.",
        "OHBBBBHO",
        "OBBBBBBO",
        "OBBSBBBO",
        ".OSSSSO.",
        "..OOOO..",
    };
    std::unordered_map<char, sf::Color> palette = {
        { 'O', shade(fill, -60) },
        { 'H', shade(fill, 35) },
        { 'B', fill },
        { 'S', shade(fill, -35) },
    };
    drawGroundShadow(window, sf::Vector2f(pos.x + 9.f, pos.y + 16.f), 7.f);
    drawPixelSprite(window, rows, sf::FloatRect(pos, sf::Vector2f(18.f, 18.f)), palette);
}

void GameWorld::drawNpc(sf::RenderWindow& window, const Npc& npc) {
    // npc.pos is this NPC's collision-box center (see updateNpcs/collidesWith*),
    // same spot the old flat circle used to be centered on -- drawPixelPerson
    // wants a feet-center point, i.e. kPlayerSize/2 further down.
    // Walk phase is 0 (standing pose) unless actually walking this frame
    // (see updateNpcs); `npc.home` seeds a per-NPC offset off the shared
    // waterWaveTimer_ clock so a whole zone's NPCs don't all bob in lockstep.
    float walkPhase = npc.isWalking ? waterWaveTimer_ * 6.f + npc.home.x * 0.3f : 0.f;
    drawPixelPerson(window, sf::Vector2f(npc.pos.x, npc.pos.y + kPlayerSize / 2.f), npc.color, false, walkPhase);

    if (fontLoaded_) {
        sf::Text text(font_, toSfString(Localization::t(npc.nameKey)), 12);
        sf::FloatRect bounds = text.getLocalBounds();
        text.setPosition(sf::Vector2f(npc.pos.x - bounds.size.x / 2.f, npc.pos.y - kPlayerSize));
        text.setFillColor(sf::Color::White);
        text.setOutlineColor(sf::Color::Black);
        text.setOutlineThickness(2.f);
        window.draw(text);
    }
}

void GameWorld::drawZone(sf::RenderWindow& window) {
    const Zone& z = zones_[currentZone_];

    for (const auto& d : z.decorations) {
        if (d.kind == Decoration::Kind::GrassPatch) {
            sf::RectangleShape patch(d.size);
            patch.setPosition(d.position);
            patch.setFillColor(sf::Color(40, 122, 50, 120));
            window.draw(patch);
        } else if (d.kind == Decoration::Kind::Path) {
            drawPixelPanel(window, d.position, d.size, sf::Color(176, 152, 104), sf::Color(140, 118, 78), d.position, 9.f);
        } else if (d.kind == Decoration::Kind::Sand) {
            drawPixelPanel(window, d.position, d.size, sf::Color(222, 198, 150), sf::Color(190, 165, 115), d.position, 9.f);
        } else if (d.kind == Decoration::Kind::Water) {
            drawPixelPanel(window, d.position, d.size, sf::Color(48, 92, 130), sf::Color(28, 60, 90), d.position, 9.f);

            // A few slow, phase-offset sine "ripples" -- purely cosmetic,
            // driven by waterWaveTimer_ (see run()'s per-frame update).
            constexpr int kWaveLines = 4;
            for (int i = 0; i < kWaveLines; ++i) {
                float rowY = d.position.y + d.size.y * (0.25f + 0.2f * static_cast<float>(i));
                float phase = waterWaveTimer_ * 1.2f + static_cast<float>(i) * 1.7f;
                std::vector<sf::Vertex> verts;
                for (float x = d.position.x; x <= d.position.x + d.size.x; x += 24.f) {
                    float wobble = std::sin(phase + x * 0.02f) * 3.f;
                    verts.push_back(sf::Vertex{ sf::Vector2f(x, rowY + wobble), sf::Color(120, 170, 200, 160) });
                }
                if (verts.size() >= 2) window.draw(verts.data(), verts.size(), sf::PrimitiveType::LineStrip);
            }
        }
    }
    for (const auto& d : z.decorations) if (d.kind == Decoration::Kind::Bush) drawBush(window, d.position);
    for (const auto& f : z.forageables) drawForageable(window, f);
    for (const auto& b : z.buildings) drawBuilding(window, b);
    for (const auto& npc : z.npcs) drawNpc(window, npc);
    for (const auto& d : z.decorations) if (d.kind == Decoration::Kind::Tree) drawTree(window, d.position);
}

void GameWorld::drawLegend(sf::RenderWindow& window) {
    if (!fontLoaded_) return;
    struct Row { sf::Color color; std::string key; };
    const Row rows[] = {
        { kTier1, "legend_tier1" },
        { kTier2, "legend_tier2" },
        { kTier3, "legend_tier3" },
        { kService, "legend_service" },
        { sf::Color(210, 80, 80), "legend_npc" },
    };

    float panelW = 230.f;
    float panelH = 22.f + 5 * 22.f;
    sf::RectangleShape bg(sf::Vector2f(panelW, panelH));
    bg.setPosition(sf::Vector2f(10.f, 10.f));
    bg.setFillColor(sf::Color(0, 0, 0, 150));
    window.draw(bg);

    sf::Text title(font_, toSfString(Localization::t("legend_title")), 14);
    title.setPosition(sf::Vector2f(18.f, 14.f));
    title.setFillColor(sf::Color::White);
    window.draw(title);

    float y = 38.f;
    for (const auto& row : rows) {
        sf::RectangleShape swatch(sf::Vector2f(14.f, 14.f));
        swatch.setPosition(sf::Vector2f(18.f, y));
        swatch.setFillColor(row.color);
        swatch.setOutlineThickness(1.f);
        swatch.setOutlineColor(sf::Color(20, 20, 20));
        window.draw(swatch);

        sf::Text label(font_, toSfString(Localization::t(row.key)), 13);
        label.setPosition(sf::Vector2f(38.f, y - 2.f));
        label.setFillColor(sf::Color::White);
        window.draw(label);
        y += 22.f;
    }
}

void GameWorld::drawMinimap(sf::RenderWindow& window) {
    // Fixed layout matching the zones' actual relationship: Mining District
    // is north of Town Square, Farmlands is east of it, Valley District is
    // west of it, Harbor District is south of it, and Highlands District is
    // further north beyond Mining. Positioned below the Legend panel (which
    // occupies roughly (10,10)-(240,142)) so the two never overlap -- only
    // drawn at all while showMinimap_ is true (toggled with M).
    constexpr float panelX = 10.f, panelY = 152.f;
    constexpr float panelW = 290.f, panelH = 292.f;
    sf::RectangleShape bg(sf::Vector2f(panelW, panelH));
    bg.setPosition(sf::Vector2f(panelX, panelY));
    bg.setFillColor(sf::Color(0, 0, 0, 150));
    window.draw(bg);

    const sf::Vector2f boxSize(60.f, 44.f);
    const sf::Vector2f highlandsPos(panelX + 148.f, panelY + 14.f);
    const sf::Vector2f miningPos(panelX + 148.f, panelY + 76.f);
    const sf::Vector2f townPos(panelX + 148.f, panelY + 138.f);
    const sf::Vector2f farmPos(panelX + 220.f, panelY + 138.f);
    const sf::Vector2f valleyPos(panelX + 76.f, panelY + 138.f);
    const sf::Vector2f harborPos(panelX + 148.f, panelY + 200.f);
    const sf::Vector2f marketPos(panelX + 220.f, panelY + 200.f); // south of Farmlands, same row as Harbor

    sf::Vertex vline[] = {
        sf::Vertex{ sf::Vector2f(townPos.x + boxSize.x / 2.f, townPos.y), sf::Color(210, 210, 210) },
        sf::Vertex{ sf::Vector2f(miningPos.x + boxSize.x / 2.f, miningPos.y + boxSize.y), sf::Color(210, 210, 210) },
    };
    window.draw(vline, 2, sf::PrimitiveType::Lines);

    sf::Vertex vlineNorth[] = {
        sf::Vertex{ sf::Vector2f(miningPos.x + boxSize.x / 2.f, miningPos.y), sf::Color(210, 210, 210) },
        sf::Vertex{ sf::Vector2f(highlandsPos.x + boxSize.x / 2.f, highlandsPos.y + boxSize.y), sf::Color(210, 210, 210) },
    };
    window.draw(vlineNorth, 2, sf::PrimitiveType::Lines);

    sf::Vertex vlineSouth[] = {
        sf::Vertex{ sf::Vector2f(townPos.x + boxSize.x / 2.f, townPos.y + boxSize.y), sf::Color(210, 210, 210) },
        sf::Vertex{ sf::Vector2f(harborPos.x + boxSize.x / 2.f, harborPos.y), sf::Color(210, 210, 210) },
    };
    window.draw(vlineSouth, 2, sf::PrimitiveType::Lines);

    sf::Vertex hlineEast[] = {
        sf::Vertex{ sf::Vector2f(townPos.x + boxSize.x, townPos.y + boxSize.y / 2.f), sf::Color(210, 210, 210) },
        sf::Vertex{ sf::Vector2f(farmPos.x, farmPos.y + boxSize.y / 2.f), sf::Color(210, 210, 210) },
    };
    window.draw(hlineEast, 2, sf::PrimitiveType::Lines);

    sf::Vertex hlineWest[] = {
        sf::Vertex{ sf::Vector2f(valleyPos.x + boxSize.x, valleyPos.y + boxSize.y / 2.f), sf::Color(210, 210, 210) },
        sf::Vertex{ sf::Vector2f(townPos.x, townPos.y + boxSize.y / 2.f), sf::Color(210, 210, 210) },
    };
    window.draw(hlineWest, 2, sf::PrimitiveType::Lines);

    sf::Vertex vlineMarket[] = {
        sf::Vertex{ sf::Vector2f(farmPos.x + boxSize.x / 2.f, farmPos.y + boxSize.y), sf::Color(210, 210, 210) },
        sf::Vertex{ sf::Vector2f(marketPos.x + boxSize.x / 2.f, marketPos.y), sf::Color(210, 210, 210) },
    };
    window.draw(vlineMarket, 2, sf::PrimitiveType::Lines);

    // Non-current boxes are outlined in a subtle per-zone theme color instead
    // of a flat grey, so the map reads at a glance even before walking there;
    // the current zone still always overrides to the same gold highlight.
    auto drawZoneBox = [&](sf::Vector2f pos, int zoneIndex, const char* labelKey, sf::Color themeColor) {
        bool isCurrent = zoneIndex == currentZone_;
        sf::RectangleShape box(boxSize);
        box.setPosition(pos);
        box.setFillColor(isCurrent ? sf::Color(90, 150, 90) : sf::Color(65, 65, 75));
        box.setOutlineThickness(isCurrent ? 3.f : 1.5f);
        box.setOutlineColor(isCurrent ? sf::Color(232, 212, 120) : themeColor);
        window.draw(box);
        if (fontLoaded_) {
            sf::Text label(font_, toSfString(Localization::t(labelKey)), 10);
            sf::FloatRect b = label.getLocalBounds();
            label.setPosition(sf::Vector2f(pos.x + boxSize.x / 2.f - b.size.x / 2.f, pos.y + boxSize.y / 2.f - 8.f));
            label.setFillColor(sf::Color::White);
            window.draw(label);
        }

        // A small orange dot in the corner if anything in this zone is
        // currently under construction (see Business::constructionDaysRemaining)
        // -- lets a player planning a route see which districts have work
        // in progress without walking all the way over.
        if (zoneIndex >= 0 && zoneIndex < static_cast<int>(zones_.size())) {
            bool anyBuilding = false;
            for (const auto& zb : zones_[static_cast<size_t>(zoneIndex)].buildings) {
                if (game_.businessConstructionInfo(zb.id).inProgress) { anyBuilding = true; break; }
            }
            if (anyBuilding) {
                sf::CircleShape dot(4.f);
                dot.setPosition(sf::Vector2f(pos.x + boxSize.x - 10.f, pos.y + 2.f));
                dot.setFillColor(sf::Color(235, 150, 60));
                window.draw(dot);
            }
        }
    };

    drawZoneBox(highlandsPos, 5, "zone_highlands", sf::Color(110, 190, 120));   // pastoral green
    drawZoneBox(miningPos, 2, "zone_mining", sf::Color(160, 140, 120));         // rock/ore brown
    drawZoneBox(townPos, 0, "zone_town_square", sf::Color(200, 190, 160));     // neutral civic tan
    drawZoneBox(farmPos, 1, "zone_farmlands", sf::Color(200, 190, 100));       // wheat gold
    drawZoneBox(valleyPos, 3, "zone_valley", sf::Color(180, 140, 200));        // orchard/vineyard purple
    drawZoneBox(harborPos, 4, "zone_harbor", sf::Color(100, 160, 210));        // marine blue
    drawZoneBox(marketPos, 6, "zone_market", sf::Color(230, 150, 90));         // food-stall orange
}

void GameWorld::drawHud(sf::RenderWindow& window) {
    sf::Vector2u winSize = window.getSize();
    sf::RectangleShape bg(sf::Vector2f(static_cast<float>(winSize.x), 54.f));
    bg.setPosition(sf::Vector2f(0.f, static_cast<float>(winSize.y) - 54.f));
    bg.setFillColor(sf::Color(0, 0, 0, 150));
    window.draw(bg);

    if (!fontLoaded_) return;

    std::ostringstream oss;
    oss << Localization::t("hud_generation") << " " << game_.generation()
        << "   " << Localization::t("hud_cash") << ": $" << std::fixed << std::setprecision(2) << game_.money()
        << "   " << Localization::t("hud_age") << ": " << std::fixed << std::setprecision(1) << game_.ageYears() << " " << Localization::t("hud_years")
        // A whole-number day count alongside the (barely-moves-per-day) age
        // in years -- makes a handful of sleeps/fast-forwards immediately
        // visible instead of only showing up after enough days accumulate
        // to move the 1-decimal age figure.
        << "   " << Localization::t("hud_day_prefix") << game_.totalDaysElapsed() << Localization::t("hud_day_suffix")
        << "   " << Localization::t("hud_season_prefix") << Localization::t(seasonKey(game_.currentSeason()))
        << " " << Localization::t(seasonEffectKey(game_.currentSeason()))
        << Localization::t("hud_days_until_season_prefix") << game_.daysUntilNextSeason() << Localization::t("hud_days_until_season_suffix")
        << "   |   " << Localization::t(zones_[currentZone_].nameKey);
    if (double upkeep = game_.upkeepPerSecond(); upkeep > 0.0) {
        oss << "   " << Localization::t("status_upkeep_prefix") << std::fixed << std::setprecision(3) << upkeep << "/s";
    }
    oss << "\n" << applyKeyPlaceholders(Localization::t("hud_help"));

    sf::Text hud(font_, toSfString(oss.str()), 14);
    hud.setPosition(sf::Vector2f(10.f, static_cast<float>(winSize.y) - 46.f));
    hud.setFillColor(sf::Color::White);
    window.draw(hud);

    // World-view toast (e.g. the locked-building hint) for feedback set
    // while no overlay is open. Overlays render overlayFeedback_ themselves
    // at the bottom of their own panel, so only show it here when there's no
    // overlay to have already done that -- otherwise it'd render twice.
    if (currentOverlay_ == OverlayKind::None && !overlayFeedback_.empty()) {
        sf::Text toast(font_, toSfString(overlayFeedback_), 16);
        toast.setStyle(sf::Text::Bold);
        sf::FloatRect bounds = toast.getLocalBounds();
        toast.setPosition(sf::Vector2f(static_cast<float>(winSize.x) / 2.f - bounds.size.x / 2.f - bounds.position.x,
            static_cast<float>(winSize.y) - 96.f));
        toast.setFillColor(overlayFeedbackColor_);
        toast.setOutlineColor(sf::Color::Black);
        toast.setOutlineThickness(2.f);
        window.draw(toast);
    }
}

void GameWorld::drawNetWorthPanel(sf::RenderWindow& window) {
    constexpr float panelW = 220.f, panelH = 120.f;
    float panelX = static_cast<float>(windowSize_.x) - panelW - 10.f;
    float panelY = static_cast<float>(windowSize_.y) - 54.f - panelH - 10.f; // just above the HUD bar

    sf::RectangleShape bg(sf::Vector2f(panelW, panelH));
    bg.setPosition(sf::Vector2f(panelX, panelY));
    bg.setFillColor(sf::Color(0, 0, 0, 150));
    window.draw(bg);

    if (!fontLoaded_) return;

    sf::Text title(font_, toSfString(Localization::t("networth_panel_title")), 13);
    title.setPosition(sf::Vector2f(panelX + 10.f, panelY + 8.f));
    title.setFillColor(sf::Color::White);
    window.draw(title);

    if (moneyHistory_.size() >= 2) {
        float minV = *std::min_element(moneyHistory_.begin(), moneyHistory_.end());
        float maxV = *std::max_element(moneyHistory_.begin(), moneyHistory_.end());
        if (maxV - minV < 1.f) maxV = minV + 1.f; // flat history -- avoid a divide-by-zero, draw a flat line instead

        float graphX = panelX + 10.f, graphY = panelY + 32.f;
        float graphW = panelW - 20.f, graphH = panelH - 60.f;

        std::vector<sf::Vertex> verts;
        verts.reserve(moneyHistory_.size());
        for (size_t i = 0; i < moneyHistory_.size(); ++i) {
            float t = static_cast<float>(i) / static_cast<float>(moneyHistory_.size() - 1);
            float norm = (moneyHistory_[i] - minV) / (maxV - minV);
            verts.push_back(sf::Vertex{
                sf::Vector2f(graphX + t * graphW, graphY + graphH - norm * graphH),
                sf::Color(130, 220, 150) });
        }
        window.draw(verts.data(), verts.size(), sf::PrimitiveType::LineStrip);
    }

    sf::Text current(font_, toSfString("$" + formatNumber(game_.money())), 14);
    current.setStyle(sf::Text::Bold);
    current.setPosition(sf::Vector2f(panelX + 10.f, panelY + panelH - 24.f));
    current.setFillColor(sf::Color(150, 230, 150));
    window.draw(current);
}

void GameWorld::drawLifeStatusPanel(sf::RenderWindow& window) {
    constexpr float panelW = 190.f, panelH = 104.f;
    float panelX = static_cast<float>(windowSize_.x) - panelW - 10.f;
    float panelY = 54.f; // just below the Achievements/How to Play/Recipe Book button row (see *ButtonBounds, all at y:10-44)

    sf::RectangleShape bg(sf::Vector2f(panelW, panelH));
    bg.setPosition(sf::Vector2f(panelX, panelY));
    bg.setFillColor(sf::Color(0, 0, 0, 150));
    window.draw(bg);

    if (!fontLoaded_) return;

    // Same red/orange/green bands as Life::productionMultiplier's own
    // thresholds (<=0 halves output, otherwise fine) -- 30% is just an
    // early warning, not itself a mechanical threshold.
    auto barColor = [](double pct) {
        if (pct <= 0.0) return sf::Color(220, 90, 90);
        if (pct < 30.0) return sf::Color(230, 160, 70);
        return sf::Color(130, 210, 130);
    };
    auto drawStat = [&](float y, const std::string& labelKey, double pct) {
        uiText(window, { panelX + 10.f, y }, Localization::t(labelKey), 12, sf::Color(200, 200, 200));
        sf::RectangleShape barBg(sf::Vector2f(panelW - 20.f, 10.f));
        barBg.setPosition(sf::Vector2f(panelX + 10.f, y + 16.f));
        barBg.setFillColor(sf::Color(50, 50, 56));
        window.draw(barBg);
        float frac = static_cast<float>(std::clamp(pct, 0.0, 100.0) / 100.0);
        if (frac > 0.f) {
            sf::RectangleShape barFill(sf::Vector2f((panelW - 20.f) * frac, 10.f));
            barFill.setPosition(sf::Vector2f(panelX + 10.f, y + 16.f));
            barFill.setFillColor(barColor(pct));
            window.draw(barFill);
        }
    };

    drawStat(panelY + 8.f, "status_energy_label", game_.energy());
    drawStat(panelY + 40.f, "status_hunger_label", game_.hunger());

    // Sickness is binary, not a 0-100 bar -- just a colored yes/no line.
    std::string sickText = game_.isSick() ? Localization::t("status_sick_yes") : Localization::t("status_sick_no");
    uiText(window, { panelX + 10.f, panelY + 76.f }, Localization::t("status_sick_label") + sickText, 13,
        game_.isSick() ? sf::Color(230, 120, 120) : sf::Color(150, 220, 150), true);
}

void GameWorld::updateDayNightAndWeather(float dt) {
    dayNightTimer_ += dt;
    if (dayNightTimer_ >= kDayNightCycleSeconds) dayNightTimer_ -= kDayNightCycleSeconds;

    if (raining_) rainTime_ += dt;
    seasonalAmbientTimer_ += dt; // drives drawSeasonalAmbient -- its own accumulator, independent of rainTime_

    weatherCheckTimer_ -= dt;
    if (weatherCheckTimer_ <= 0.f) {
        if (raining_) {
            raining_ = false;
            weatherCheckTimer_ = randRange(60.f, 140.f); // clear spell before the next roll
            if (ambientRainSound_) ambientRainSound_->stop();
        } else if (randRange(0.f, 1.f) < 0.35f) {
            raining_ = true;
            weatherCheckTimer_ = randRange(20.f, 45.f); // rain duration
            if (ambientRainSound_) ambientRainSound_->play();
        } else {
            weatherCheckTimer_ = randRange(30.f, 60.f); // try again soon
        }
    }
}

sf::Color GameWorld::dayNightTint() const {
    // Four flat-color keyframes cycled through smoothly: dawn -> day (no
    // tint) -> dusk -> night -> back to dawn. Deliberately just a linear
    // blend between two RGBA keyframes, no gradients/glow, to match the
    // rest of the world's plain-shape look.
    struct Key { float t; std::uint8_t r, g, b, a; };
    static const Key keys[] = {
        { 0.00f, 255, 200, 150, 55 },  // dawn
        { 0.25f, 255, 255, 255, 0 },   // day
        { 0.50f, 255, 150, 90, 60 },   // dusk
        { 0.75f, 25, 35, 80, 130 },    // night
        { 1.00f, 255, 200, 150, 55 },  // wraps back to dawn
    };
    float t = dayNightTimer_ / kDayNightCycleSeconds;
    for (int i = 0; i < 4; ++i) {
        if (t < keys[i].t || t > keys[i + 1].t) continue;
        float span = keys[i + 1].t - keys[i].t;
        float local = span > 0.f ? (t - keys[i].t) / span : 0.f;
        auto lerp = [&](std::uint8_t a, std::uint8_t b) {
            return static_cast<std::uint8_t>(static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * local);
        };
        return sf::Color(lerp(keys[i].r, keys[i + 1].r), lerp(keys[i].g, keys[i + 1].g), lerp(keys[i].b, keys[i + 1].b), lerp(keys[i].a, keys[i + 1].a));
    }
    return sf::Color(255, 255, 255, 0);
}

sf::Color GameWorld::seasonTint() const {
    // Deliberately very low alpha (14-26) -- a hint of color, not a filter
    // over the whole scene; layered on top of the day/night tint below.
    switch (game_.currentSeason()) {
    case Season::Spring: return sf::Color(140, 220, 150, 18);
    case Season::Summer: return sf::Color(255, 230, 120, 14);
    case Season::Autumn: return sf::Color(210, 140, 60, 22);
    default:             return sf::Color(150, 190, 230, 26); // Winter
    }
}

void GameWorld::drawDayNightOverlay(sf::RenderWindow& window) {
    sf::RectangleShape tint(sf::Vector2f(static_cast<float>(windowSize_.x), static_cast<float>(windowSize_.y)));
    tint.setFillColor(dayNightTint());
    window.draw(tint);

    sf::RectangleShape season(sf::Vector2f(static_cast<float>(windowSize_.x), static_cast<float>(windowSize_.y)));
    season.setFillColor(seasonTint());
    window.draw(season);
}

void GameWorld::drawWeather(sf::RenderWindow& window) {
    if (!raining_) return;
    // Winter re-skins the same rain roll as snow instead of adding a
    // separate precipitation system -- still just "raining_ is true"
    // underneath, only how it's drawn changes.
    bool snowing = (game_.currentSeason() == Season::Winter);

    if (snowing) {
        for (int i = 0; i < kRainDropCount; ++i) {
            float laneX = std::fmod(static_cast<float>(i) * 137.f, static_cast<float>(windowSize_.x));
            float fallSpeed = 70.f;
            float y = std::fmod(rainTime_ * fallSpeed + static_cast<float>(i) * 53.f, static_cast<float>(windowSize_.y) + 20.f) - 20.f;
            float drift = std::sin(rainTime_ * 0.6f + static_cast<float>(i)) * 18.f;
            sf::CircleShape flake(2.5f);
            flake.setPosition(sf::Vector2f(laneX + drift, y));
            flake.setFillColor(sf::Color(255, 255, 255, 190));
            window.draw(flake);
        }
        return;
    }

    // Deterministic pseudo-scatter (no stored per-drop state): each drop's
    // lane and phase come from its index, so this needs only one accumulator.
    std::vector<sf::Vertex> verts;
    verts.reserve(kRainDropCount * 2);
    sf::Color dropColor(170, 190, 215, 130);
    for (int i = 0; i < kRainDropCount; ++i) {
        float laneX = std::fmod(static_cast<float>(i) * 137.f, static_cast<float>(windowSize_.x));
        float fallSpeed = 500.f;
        float y = std::fmod(rainTime_ * fallSpeed + static_cast<float>(i) * 53.f, static_cast<float>(windowSize_.y) + 20.f) - 20.f;
        verts.push_back(sf::Vertex{ sf::Vector2f(laneX, y), dropColor });
        verts.push_back(sf::Vertex{ sf::Vector2f(laneX - 5.f, y + 16.f), dropColor });
    }
    window.draw(verts.data(), verts.size(), sf::PrimitiveType::Lines);
}

void GameWorld::drawSeasonalAmbient(sf::RenderWindow& window) {
    // Always-on, deliberately sparse per-season ambience -- distinct from
    // drawWeather's occasional rain/snow "event" above: this runs every
    // frame regardless of raining_, using the same deterministic
    // index-driven fmod trick (own accumulator, seasonalAmbientTimer_, so it
    // never shares a timeline with actual rain/snow).
    float w = static_cast<float>(windowSize_.x);
    float h = static_cast<float>(windowSize_.y);

    switch (game_.currentSeason()) {
    case Season::Winter: {
        constexpr int kCount = 24;
        for (int i = 0; i < kCount; ++i) {
            float laneX = std::fmod(static_cast<float>(i) * 149.f, w);
            float y = std::fmod(seasonalAmbientTimer_ * 34.f + static_cast<float>(i) * 61.f, h + 20.f) - 20.f;
            float drift = std::sin(seasonalAmbientTimer_ * 0.5f + static_cast<float>(i)) * 14.f;
            sf::RectangleShape flake(sf::Vector2f(4.f, 4.f));
            flake.setPosition(sf::Vector2f(laneX + drift, y));
            flake.setFillColor(sf::Color(255, 255, 255, 170));
            window.draw(flake);
        }
        break;
    }
    case Season::Autumn: {
        constexpr int kCount = 24;
        const sf::Color leafColors[3] = { sf::Color(200, 110, 40, 205), sf::Color(190, 60, 40, 205), sf::Color(212, 172, 50, 205) };
        for (int i = 0; i < kCount; ++i) {
            float laneX = std::fmod(static_cast<float>(i) * 163.f, w);
            float y = std::fmod(seasonalAmbientTimer_ * 22.f + static_cast<float>(i) * 71.f, h + 20.f) - 20.f;
            float drift = std::sin(seasonalAmbientTimer_ * 0.8f + static_cast<float>(i) * 1.3f) * 36.f; // wider sway -- tumbling, not just falling
            sf::RectangleShape leaf(sf::Vector2f(6.f, 6.f));
            leaf.setPosition(sf::Vector2f(laneX + drift, y));
            leaf.setFillColor(leafColors[i % 3]);
            leaf.setRotation(sf::degrees(std::fmod(seasonalAmbientTimer_ * 40.f + static_cast<float>(i) * 30.f, 360.f)));
            window.draw(leaf);
        }
        break;
    }
    case Season::Spring: {
        // Cherry-blossom petals -- round, saturated pink, and visibly
        // tumbling (drift amplitude wider than it falls). Deliberately far
        // from Winter's white square snowflakes below: a pale near-white
        // square falling straight down used to read as "snow in spring" at
        // a glance, which is what this shape+color swap fixes.
        constexpr int kCount = 20;
        for (int i = 0; i < kCount; ++i) {
            float laneX = std::fmod(static_cast<float>(i) * 173.f, w);
            float y = std::fmod(seasonalAmbientTimer_ * 16.f + static_cast<float>(i) * 47.f, h + 20.f) - 20.f; // slowest fall of the four -- petals/pollen, not rain
            float drift = std::sin(seasonalAmbientTimer_ * 0.4f + static_cast<float>(i) * 0.7f) * 26.f;
            sf::CircleShape petal(3.5f);
            petal.setPosition(sf::Vector2f(laneX + drift, y));
            petal.setFillColor(sf::Color(255, 140, 190, 210));
            window.draw(petal);
        }
        break;
    }
    case Season::Summer: {
        // Rises instead of falls -- heat haze/dust, the one season that reads
        // as "up" rather than "down" so all four are visually distinct at a glance.
        constexpr int kCount = 18;
        constexpr float kCycle = 860.f; // taller than the window so motes fully clear the top before wrapping
        for (int i = 0; i < kCount; ++i) {
            float laneX = std::fmod(static_cast<float>(i) * 181.f, w);
            float progress = std::fmod(seasonalAmbientTimer_ * 18.f + static_cast<float>(i) * 59.f, kCycle);
            float y = h - progress + 20.f;
            float drift = std::sin(seasonalAmbientTimer_ * 0.6f + static_cast<float>(i) * 0.9f) * 10.f;
            float tNorm = progress / kCycle;
            std::uint8_t alpha = static_cast<std::uint8_t>(160.f * (1.f - std::fabs(2.f * tNorm - 1.f))); // fades in, peaks mid-screen, fades out
            sf::RectangleShape mote(sf::Vector2f(3.f, 3.f));
            mote.setPosition(sf::Vector2f(laneX + drift, y));
            mote.setFillColor(sf::Color(250, 230, 140, alpha));
            window.draw(mote);
        }
        break;
    }
    }
}

void GameWorld::updateSeasonTransition(float dt) {
    if (!seasonTransitionActive_) return;
    seasonTransitionTimer_ += dt;
    if (seasonTransitionTimer_ >= kSeasonTransitionDuration) {
        seasonTransitionActive_ = false;
        seasonTransitionTimer_ = 0.f;
    }
}

void GameWorld::updateAchievementToast(float dt) {
    if (!currentAchievementToastId_.empty()) {
        achievementToastTimer_ -= dt;
        if (achievementToastTimer_ <= 0.f) {
            currentAchievementToastId_.clear();
        }
        return; // don't pop the next one the same frame the current one ends -- one at a time
    }
    if (!achievementToastQueue_.empty()) {
        currentAchievementToastId_ = achievementToastQueue_.front();
        achievementToastQueue_.erase(achievementToastQueue_.begin());
        achievementToastTimer_ = kAchievementToastSeconds;
        if (achievementSound_) achievementSound_->play();
    }
}

void GameWorld::drawAchievementToast(sf::RenderWindow& window) {
    if (currentAchievementToastId_.empty() || !fontLoaded_) return;

    sf::Vector2f size(300.f, 64.f);
    sf::Vector2f pos(16.f, static_cast<float>(windowSize_.y) - size.y - 16.f);
    sf::RectangleShape bg(size);
    bg.setPosition(pos);
    bg.setFillColor(sf::Color(30, 32, 40, 235));
    bg.setOutlineThickness(3.f);
    bg.setOutlineColor(sf::Color(232, 212, 120));
    window.draw(bg);

    // A small gold square standing in for a trophy/badge icon -- keeps with
    // the game's flat-shape, no-imported-art look instead of needing a
    // texture just for this.
    sf::RectangleShape badge(sf::Vector2f(36.f, 36.f));
    badge.setPosition(sf::Vector2f(pos.x + 14.f, pos.y + size.y / 2.f - 18.f));
    badge.setFillColor(sf::Color(232, 212, 120));
    badge.setOutlineThickness(2.f);
    badge.setOutlineColor(sf::Color(120, 100, 40));
    window.draw(badge);

    uiText(window, { pos.x + 62.f, pos.y + 12.f }, Localization::t("achievement_toast_header"), 13, sf::Color(232, 212, 120), true);
    uiText(window, { pos.x + 62.f, pos.y + 34.f }, Localization::t("ach_" + currentAchievementToastId_ + "_name"), 15, sf::Color::White, true);
}

void GameWorld::updateEventToast(float dt) {
    if (!currentEventToast_.empty()) {
        eventToastTimer_ -= dt;
        if (eventToastTimer_ <= 0.f) currentEventToast_.clear();
        return; // one at a time, same as the achievement toast
    }
    if (!eventToastQueue_.empty()) {
        currentEventToast_ = eventToastQueue_.front();
        eventToastQueue_.erase(eventToastQueue_.begin());
        eventToastTimer_ = kEventToastSeconds;
    }
}

void GameWorld::drawEventToast(sf::RenderWindow& window) {
    if (currentEventToast_.empty() || !fontLoaded_) return;

    // Bottom-right, mirroring the achievement toast's bottom-left -- width
    // sized to the actual line (these vary a lot more in length than an
    // achievement name), clamped so a pathologically long line can't run
    // off the left edge of the window.
    sf::Text text(font_, toSfString(currentEventToast_), 14);
    sf::FloatRect bounds = text.getLocalBounds();
    float boxW = std::clamp(bounds.size.x + 32.f, 220.f, 520.f);
    sf::Vector2f size(boxW, 50.f);
    sf::Vector2f pos(static_cast<float>(windowSize_.x) - size.x - 16.f, static_cast<float>(windowSize_.y) - size.y - 16.f - 54.f);

    sf::RectangleShape bg(size);
    bg.setPosition(pos);
    bg.setFillColor(sf::Color(30, 32, 40, 235));
    bg.setOutlineThickness(3.f);
    bg.setOutlineColor(sf::Color(150, 170, 220));
    window.draw(bg);

    text.setPosition(sf::Vector2f(pos.x + 14.f, pos.y + size.y / 2.f - 9.f));
    text.setFillColor(sf::Color(225, 230, 245));
    window.draw(text);
}

void GameWorld::drawSeasonTransitionOverlay(sf::RenderWindow& window) {
    if (!seasonTransitionActive_) return;

    // More saturated than seasonTint()'s barely-there wash -- this is meant
    // to read clearly as "the season just changed", not blend into the scene.
    auto transitionColor = [](Season s) {
        switch (s) {
        case Season::Spring: return sf::Color(90, 195, 115);
        case Season::Summer: return sf::Color(245, 195, 60);
        case Season::Autumn: return sf::Color(205, 105, 40);
        default:             return sf::Color(105, 165, 225); // Winter
        }
    };

    // Three color-block bars sweep down to fully cover the screen, hold with
    // the season's name, then continue sweeping down and off -- staggered
    // per bar (kStagger) so it reads as a cascade rather than one flat panel
    // popping in/out. Linear interpolation throughout, no easing curves, to
    // match the rest of the game's flat-shape/no-gradient look.
    constexpr float kCoverDur = 0.5f, kHoldDur = 0.6f, kUncoverDur = 0.5f;
    constexpr int kBars = 3;
    constexpr float kStagger = 0.1f;
    float w = static_cast<float>(windowSize_.x), h = static_cast<float>(windowSize_.y);
    float barW = w / static_cast<float>(kBars);
    float t = seasonTransitionTimer_;
    sf::Color color = transitionColor(seasonTransitionTo_);

    for (int i = 0; i < kBars; ++i) {
        float y;
        if (t < kCoverDur) {
            float localDur = kCoverDur - static_cast<float>(kBars - 1) * kStagger;
            float localT = std::clamp((t - static_cast<float>(i) * kStagger) / localDur, 0.f, 1.f);
            y = -h + h * localT; // slides down from fully above the screen to y=0 (covering)
        } else if (t < kCoverDur + kHoldDur) {
            y = 0.f;
        } else {
            float localDur = kUncoverDur - static_cast<float>(kBars - 1) * kStagger;
            float localT = std::clamp((t - (kCoverDur + kHoldDur) - static_cast<float>(i) * kStagger) / localDur, 0.f, 1.f);
            y = h * localT; // continues down and off the bottom of the screen
        }
        sf::RectangleShape bar(sf::Vector2f(barW + 1.f, h)); // +1px overlap so adjacent bars never show a hairline seam
        bar.setPosition(sf::Vector2f(static_cast<float>(i) * barW, y));
        bar.setFillColor((i % 2 == 0) ? color : darken(color, 0.8f)); // alternating shade so it doesn't read as one flat rectangle
        window.draw(bar);
    }

    if (t >= kCoverDur && t < kCoverDur + kHoldDur && fontLoaded_) {
        sf::Text label(font_, toSfString(Localization::t(seasonKey(seasonTransitionTo_))), 42);
        sf::FloatRect b = label.getLocalBounds();
        label.setPosition(sf::Vector2f(w / 2.f - b.size.x / 2.f - b.position.x, h / 2.f - b.size.y / 2.f - b.position.y));
        label.setFillColor(sf::Color(255, 255, 255, 235));
        label.setStyle(sf::Text::Bold);
        window.draw(label);
    }
}

void GameWorld::drawAchievementsButton(sf::RenderWindow& window) {
    sf::FloatRect rect = achievementsButtonBounds();
    sf::RectangleShape btn(rect.size);
    btn.setPosition(rect.position);
    btn.setFillColor(sf::Color(72, 58, 112));
    btn.setOutlineThickness(2.f);
    btn.setOutlineColor(sf::Color(232, 212, 120));
    window.draw(btn);

    if (!fontLoaded_) return;
    sf::Text label(font_, toSfString(Localization::t("achievements_button")), 15);
    sf::FloatRect bounds = label.getLocalBounds();
    label.setPosition(sf::Vector2f(rect.position.x + rect.size.x / 2.f - bounds.size.x / 2.f, rect.position.y + rect.size.y / 2.f - 11.f));
    label.setFillColor(sf::Color(232, 212, 120));
    window.draw(label);
}

void GameWorld::drawUpdateBanner(sf::RenderWindow& window) {
    if (!updateAvailable_ || updateBannerDismissed_) return;

    sf::Vector2f pos(10.f, static_cast<float>(windowSize_.y) - 54.f - 44.f), size(420.f, 36.f);
    sf::RectangleShape bg(size);
    bg.setPosition(pos);
    bg.setFillColor(sf::Color(60, 90, 60, 235));
    bg.setOutlineThickness(2.f);
    bg.setOutlineColor(sf::Color(150, 220, 150));
    window.draw(bg);

    uiText(window, { pos.x + 10.f, pos.y + 9.f },
        Localization::t("update_banner_prefix") + updateLatestVersion_, 13, sf::Color(220, 250, 220));

    uiButton(window, { pos.x + size.x - 170.f, pos.y + 3.f }, { 100.f, 30.f }, Localization::t("update_banner_open_button"),
        [this]() {
#ifdef _WIN32
            if (!updateReleaseUrl_.empty()) {
                ShellExecuteW(nullptr, L"open", toSfString(updateReleaseUrl_).toWideString().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
#endif
        });
    uiButton(window, { pos.x + size.x - 62.f, pos.y + 3.f }, { 52.f, 30.f }, Localization::t("update_banner_dismiss_button"),
        [this]() { updateBannerDismissed_ = true; });
}

void GameWorld::drawHowToPlayButton(sf::RenderWindow& window) {
    sf::FloatRect rect = howToPlayButtonBounds();
    sf::RectangleShape btn(rect.size);
    btn.setPosition(rect.position);
    btn.setFillColor(sf::Color(72, 58, 112));
    btn.setOutlineThickness(2.f);
    btn.setOutlineColor(sf::Color(232, 212, 120));
    window.draw(btn);

    if (!fontLoaded_) return;
    sf::Text label(font_, toSfString(Localization::t("howtoplay_button")), 15);
    sf::FloatRect bounds = label.getLocalBounds();
    label.setPosition(sf::Vector2f(rect.position.x + rect.size.x / 2.f - bounds.size.x / 2.f, rect.position.y + rect.size.y / 2.f - 11.f));
    label.setFillColor(sf::Color(232, 212, 120));
    window.draw(label);
}

void GameWorld::drawRecipeBookButton(sf::RenderWindow& window) {
    sf::FloatRect rect = recipeBookButtonBounds();
    sf::RectangleShape btn(rect.size);
    btn.setPosition(rect.position);
    btn.setFillColor(sf::Color(72, 58, 112));
    btn.setOutlineThickness(2.f);
    btn.setOutlineColor(sf::Color(232, 212, 120));
    window.draw(btn);

    if (!fontLoaded_) return;
    sf::Text label(font_, toSfString(Localization::t("recipebook_button")), 15);
    sf::FloatRect bounds = label.getLocalBounds();
    label.setPosition(sf::Vector2f(rect.position.x + rect.size.x / 2.f - bounds.size.x / 2.f, rect.position.y + rect.size.y / 2.f - 11.f));
    label.setFillColor(sf::Color(232, 212, 120));
    window.draw(label);
}

void GameWorld::drawTutorial(sf::RenderWindow& window) {
    window.clear(sf::Color(22, 24, 30));
    if (!fontLoaded_) {
        window.display();
        return;
    }

    sf::Vector2u winSize = window.getSize();

    // Measure the body text before sizing the panel -- its length varies a
    // lot between languages (and whenever the copy grows), so a panel with a
    // fixed height/width used to silently start clipping past its own
    // outline and overlapping the "press any key" prompt below it. Sizing to
    // fit the actual measured bounds keeps that from happening again.
    sf::Text body(font_, toSfString(applyKeyPlaceholders(Localization::t("tutorial_body"))), 17);
    body.setFillColor(sf::Color::White);
    body.setLineSpacing(1.35f);
    sf::FloatRect bodyBounds = body.getLocalBounds();

    constexpr float kMargin = 36.f;
    constexpr float kTopArea = 90.f;    // title + gap above the body
    constexpr float kBottomArea = 70.f; // gap + "press any key" line below the body
    constexpr float kMinPanelW = 760.f, kMinPanelH = 340.f;

    float panelW = std::max(kMinPanelW, bodyBounds.size.x + kMargin * 2.f);
    float panelH = std::max(kMinPanelH, kTopArea + bodyBounds.size.y + kBottomArea);

    sf::RectangleShape panel(sf::Vector2f(panelW, panelH));
    panel.setPosition(sf::Vector2f((static_cast<float>(winSize.x) - panelW) / 2.f, (static_cast<float>(winSize.y) - panelH) / 2.f));
    panel.setFillColor(sf::Color(40, 44, 56));
    panel.setOutlineThickness(3.f);
    panel.setOutlineColor(sf::Color(232, 212, 120));
    window.draw(panel);

    sf::Text title(font_, toSfString(Localization::t("tutorial_title")), 26);
    title.setStyle(sf::Text::Bold);
    sf::FloatRect titleBounds = title.getLocalBounds();
    title.setPosition(sf::Vector2f(panel.getPosition().x + panel.getSize().x / 2.f - titleBounds.size.x / 2.f - titleBounds.position.x, panel.getPosition().y + 26.f));
    title.setFillColor(sf::Color(232, 212, 120));
    window.draw(title);

    float bodyTop = panel.getPosition().y + kTopArea;
    body.setPosition(sf::Vector2f(panel.getPosition().x + kMargin - bodyBounds.position.x, bodyTop - bodyBounds.position.y));
    window.draw(body);

    sf::Text cont(font_, toSfString(Localization::t("tutorial_continue")), 15);
    sf::FloatRect contBounds = cont.getLocalBounds();
    float bodyBottom = bodyTop + bodyBounds.size.y;
    float contY = std::max(bodyBottom + 18.f, panel.getPosition().y + panel.getSize().y - 42.f);
    cont.setPosition(sf::Vector2f(panel.getPosition().x + panel.getSize().x / 2.f - contBounds.size.x / 2.f - contBounds.position.x, contY));
    cont.setFillColor(sf::Color(200, 200, 200));
    window.draw(cont);

    window.display();
}

void GameWorld::beginClip(sf::RenderWindow& window, sf::FloatRect region) {
    // See the declaration comment in GameWorld.h. `region` is in the same
    // fixed logical windowSize_ space as every other draw call; gameView_'s
    // viewport maps that whole logical rect onto some sub-rectangle of the
    // real window (the letterbox from applyVideoMode). To clip to just
    // `region`, build a view whose own viewport is the proportional slice of
    // that same sub-rectangle -- pixel-for-pixel identical to gameView_
    // inside `region`, hard-cropped outside it by the GPU's own viewport clamp.
    const sf::FloatRect base = gameView_.getViewport();
    const float lw = static_cast<float>(windowSize_.x), lh = static_cast<float>(windowSize_.y);
    sf::Vector2f size = region.size;
    if (size.x < 1.f) size.x = 1.f;
    if (size.y < 1.f) size.y = 1.f;
    sf::View clipView(sf::FloatRect(region.position, size));
    clipView.setViewport(sf::FloatRect(
        sf::Vector2f(base.position.x + base.size.x * (region.position.x / lw),
                     base.position.y + base.size.y * (region.position.y / lh)),
        sf::Vector2f(base.size.x * (size.x / lw), base.size.y * (size.y / lh))));
    window.setView(clipView);
}

void GameWorld::endClip(sf::RenderWindow& window) {
    window.setView(gameView_);
}

// ---- Pixel-art world rendering ----
// Same "small role-char grid recolored through a palette" idea as the Recipe
// Book icons (drawGoodIcon below), but batched into one sf::VertexArray draw
// call per sprite/panel instead of one RectangleShape per pixel -- these run
// every frame for however many buildings/trees/NPCs are on screen at once,
// where per-pixel draw calls would actually add up (a good icon only ever
// draws a handful at a time, in the Recipe Book overlay).
void GameWorld::drawPixelSprite(sf::RenderWindow& window, const std::vector<std::string>& rows, sf::FloatRect area,
    const std::unordered_map<char, sf::Color>& palette, bool flipX) {
    if (rows.empty()) return;
    int rowCount = static_cast<int>(rows.size());
    int colCount = 0;
    for (const auto& r : rows) colCount = std::max(colCount, static_cast<int>(r.size()));
    if (rowCount == 0 || colCount == 0) return;

    float cw = area.size.x / static_cast<float>(colCount);
    float ch = area.size.y / static_cast<float>(rowCount);
    sf::VertexArray va(sf::PrimitiveType::Triangles);
    for (int r = 0; r < rowCount; ++r) {
        const std::string& line = rows[static_cast<size_t>(r)];
        for (int c = 0; c < static_cast<int>(line.size()); ++c) {
            auto it = palette.find(line[static_cast<size_t>(c)]);
            if (it == palette.end()) continue; // '.' or unmapped role -- transparent
            int col = flipX ? (colCount - 1 - c) : c;
            float x0 = area.position.x + static_cast<float>(col) * cw;
            float y0 = area.position.y + static_cast<float>(r) * ch;
            float x1 = x0 + cw + 0.6f, y1 = y0 + ch + 0.6f; // slight overlap hides seams between pixels
            sf::Vertex v0{ sf::Vector2f(x0, y0), it->second }, v1{ sf::Vector2f(x1, y0), it->second };
            sf::Vertex v2{ sf::Vector2f(x1, y1), it->second }, v3{ sf::Vector2f(x0, y1), it->second };
            va.append(v0); va.append(v1); va.append(v2);
            va.append(v0); va.append(v2); va.append(v3);
        }
    }
    window.draw(va);
}

void GameWorld::drawPixelPanel(sf::RenderWindow& window, sf::Vector2f pos, sf::Vector2f size, sf::Color baseColor,
    sf::Color outlineColor, sf::Vector2f seedPos, float pixelSize) {
    sf::Color hi = shade(baseColor, 32), sh = shade(baseColor, -32), speck = shade(baseColor, -55);
    int cols = std::max(1, static_cast<int>(size.x / pixelSize));
    int rows = std::max(1, static_cast<int>(size.y / pixelSize));
    float cw = size.x / static_cast<float>(cols), ch = size.y / static_cast<float>(rows);
    // Seeded off `seedPos` (a stable per-instance value, e.g. the building's
    // own position) rather than std::rand() -- the speckle pattern needs to
    // stay put frame to frame, not reroll into visible noise/flicker.
    int seed = static_cast<int>(seedPos.x) * 928371 + static_cast<int>(seedPos.y) * 137;

    sf::VertexArray va(sf::PrimitiveType::Triangles);
    for (int r = 0; r < rows; ++r) {
        float t = rows <= 1 ? 0.f : static_cast<float>(r) / static_cast<float>(rows - 1);
        sf::Color rowColor = t < 0.2f ? hi : (t > 0.72f ? sh : baseColor);
        for (int c = 0; c < cols; ++c) {
            sf::Color col = ((r * 928371 + c * 543212 + seed) % 17 == 0) ? speck : rowColor;
            float x0 = pos.x + static_cast<float>(c) * cw, y0 = pos.y + static_cast<float>(r) * ch;
            float x1 = x0 + cw + 0.6f, y1 = y0 + ch + 0.6f;
            sf::Vertex v0{ sf::Vector2f(x0, y0), col }, v1{ sf::Vector2f(x1, y0), col };
            sf::Vertex v2{ sf::Vector2f(x1, y1), col }, v3{ sf::Vector2f(x0, y1), col };
            va.append(v0); va.append(v1); va.append(v2);
            va.append(v0); va.append(v2); va.append(v3);
        }
    }
    window.draw(va);

    sf::RectangleShape outline(size);
    outline.setPosition(pos);
    outline.setFillColor(sf::Color::Transparent);
    outline.setOutlineThickness(2.f);
    outline.setOutlineColor(outlineColor);
    window.draw(outline);
}

void GameWorld::drawPixelRoof(sf::RenderWindow& window, sf::FloatRect area, sf::Color roofColor) {
    static const std::vector<std::string> rows = {
        "......OO......",
        ".....OHHO.....",
        "....OHBBHO....",
        "...OHBBBBHO...",
        "..OHBBBBBBHO..",
        ".OHBBBSSBBBHO.",
        "OHBBBBSSBBBBHO",
        "OSSSSSSSSSSSSO",
    };
    std::unordered_map<char, sf::Color> palette = {
        { 'O', sf::Color(25, 20, 15) },
        { 'H', shade(roofColor, 35) },
        { 'B', roofColor },
        { 'S', shade(roofColor, -35) },
    };
    drawPixelSprite(window, rows, area, palette);
}

void GameWorld::drawPixelMound(sf::RenderWindow& window, sf::FloatRect area, sf::Color rockColor) {
    // 'D' scatters a couple of darker ore-vein/pebble flecks through the
    // rock face -- fixed positions (not per-instance random), same reasoning
    // as the tree canopy's 'L' leaf clusters.
    static const std::vector<std::string> rows = {
        "......OOOO......",
        ".....OHHHHO.....",
        "....OHHBBHHO....",
        "...OHBBBBBBHO...",
        "..OHBBBBBBBBHO..",
        ".OHBBBBDDBBBBHO.",
        "OHBBBBBDDBBBBBHO",
        "OSSSSSSSSSSSSSSO",
        "OOOOOOOOOOOOOOOO",
    };
    std::unordered_map<char, sf::Color> palette = {
        { 'O', sf::Color(25, 20, 15) },
        { 'H', shade(rockColor, 32) },
        { 'B', rockColor },
        { 'D', shade(rockColor, -55) },
        { 'S', shade(rockColor, -32) },
    };
    drawPixelSprite(window, rows, area, palette);
}

void GameWorld::drawPixelBlob(sf::RenderWindow& window, sf::Vector2f center, float radius, sf::Color baseColor) {
    static const std::vector<std::string> rows = {
        "..OOOO..",
        ".OHHHHO.",
        "OHBBBBHO",
        "OBBBBBBO",
        "OBBBBBBO",
        "OHBBBBHO",
        ".OSSSSO.",
        "..OOOO..",
    };
    std::unordered_map<char, sf::Color> palette = {
        { 'O', shade(baseColor, -70) },
        { 'H', shade(baseColor, 35) },
        { 'B', baseColor },
        { 'S', shade(baseColor, -35) },
    };
    sf::Vector2f size(radius * 2.f, radius * 2.f);
    drawPixelSprite(window, rows, sf::FloatRect(sf::Vector2f(center.x - radius, center.y - radius), size), palette);
}

// Shared 12-wide pixel villager -- roles: 'O' outline, 'H' hair, 'K' skin,
// 'E' eye, 'C' shirt (the one role callers recolor -- player's signature
// yellow, or an Npc's own `color`), 'P' trousers, 'B' boots.
void GameWorld::drawPixelPerson(sf::RenderWindow& window, sf::Vector2f pos, sf::Color shirtColor, bool flipX, float walkPhase) {
    // Two leg frames -- legs together (the standing pose, also used whenever
    // walkPhase is 0) and legs apart -- swapped by sin(walkPhase) instead of
    // authoring a full mid-stride frame. Everything above the waist (torso
    // up) is identical between the two, so only that row range differs.
    static const std::vector<std::string> torsoRows = {
        "....OOOO....",
        "...OHHHHOO..",
        "..OHHHHHHHO.",
        "..OKKKKKKKO.",
        "..OKEKKKEKO.",
        "..OKKKKKKKO.",
        "..OOKKKKOO..",
        ".OCCCCCCCCO.",
        ".OCCCCCCCCO.",
        ".OCCCCCCCCO.",
    };
    static const std::vector<std::string> legsTogether = {
        "..OPPPPPPO..",
        "..OPPPPPPO..",
        "..OPPPPPPO..",
        "..OBBBBBBO..",
        "..OOOOOOOO..",
    };
    static const std::vector<std::string> legsApart = {
        "..OPPPPPPO..",
        "..OPP..PPO..",
        "..OPP..PPO..",
        "..OBB..BBO..",
        "..OOO..OOO..",
    };
    bool apart = std::sin(walkPhase) > 0.f;
    std::vector<std::string> rows = torsoRows;
    const auto& legs = apart ? legsApart : legsTogether;
    rows.insert(rows.end(), legs.begin(), legs.end());

    std::unordered_map<char, sf::Color> palette = {
        { 'O', sf::Color(30, 24, 18) },
        { 'H', sf::Color(92, 60, 32) },
        { 'K', sf::Color(235, 195, 150) },
        { 'E', sf::Color(20, 15, 15) },
        { 'C', shirtColor },
        { 'P', sf::Color(70, 90, 140) },
        { 'B', sf::Color(60, 40, 25) },
    };
    // 30x38 world footprint, feet-anchored at `pos` (same convention as
    // drawNpc/the old player square) -- a bit taller than kPlayerSize so the
    // sprite reads as a person rather than a token, but still centered on
    // the same collision box (see collidesWithBuilding/collidesWithTree
    // callers, all of which still key off kPlayerSize, unaffected by this).
    // The shadow stays put on the ground; only the sprite body bobs -- the
    // usual "feet don't actually leave the floor" pixel-art walk convention.
    float bob = std::abs(std::sin(walkPhase)) * 2.2f;
    sf::Vector2f spriteSize(30.f, 38.f);
    sf::Vector2f topLeft(pos.x - spriteSize.x / 2.f, pos.y - spriteSize.y + kPlayerSize / 2.f - bob);
    drawGroundShadow(window, pos, 11.f);
    drawPixelSprite(window, rows, sf::FloatRect(topLeft, spriteSize), palette, flipX);
}

void GameWorld::drawGroundShadow(sf::RenderWindow& window, sf::Vector2f feetCenter, float radiusX) {
    sf::CircleShape shadow(radiusX);
    shadow.setScale(sf::Vector2f(1.f, 0.4f)); // squashed into an ellipse -- reads as resting on the ground from this top-down angle
    shadow.setPosition(sf::Vector2f(feetCenter.x - radiusX, feetCenter.y - radiusX * 0.4f));
    shadow.setFillColor(sf::Color(10, 10, 10, 90));
    window.draw(shadow);
}

void GameWorld::drawPlayer(sf::RenderWindow& window, sf::Vector2f pos, bool facingLeft, float walkPhase) {
    // Player position is stored as the top-left of its kPlayerSize collision
    // box (see playerPos_ throughout run()) -- drawPixelPerson takes a feet
    // center point, so convert once here.
    sf::Vector2f feetCenter(pos.x + kPlayerSize / 2.f, pos.y + kPlayerSize);
    drawPixelPerson(window, feetCenter, sf::Color(250, 220, 60), facingLeft, walkPhase);
}

void GameWorld::drawGoodIcon(sf::RenderWindow& window, const std::string& goodId, sf::Vector2f pos, float size, sf::Color accent) {
    sf::RectangleShape card(sf::Vector2f(size, size));
    card.setPosition(pos);
    card.setFillColor(sf::Color(26, 28, 36));
    card.setOutlineThickness(2.f);
    card.setOutlineColor(sf::Color(232, 212, 120));
    window.draw(card);

    const PixelRows* rows = nullptr;
    sf::Color outline(24, 20, 16), highlight, base, shadow, accent1, accent2;
    const auto& defs = goodIconDefs();
    auto it = defs.find(goodId);
    if (it != defs.end()) {
        rows = it->second.shape;
        sf::Color seed = it->second.seed;
        highlight = shade(seed, 65);
        base = seed;
        shadow = shade(seed, -65);
        accent1 = sf::Color(250, 245, 232);   // cream/cork/lid — same across every hand-drawn icon
        accent2 = shade(seed, -25);           // subtle detail (cherry/seed/label/stripe)
    } else {
        // Generic fallback: a small shaded pixel "crate" tinted from the
        // production-chain accent color -- every processed good gets at
        // least this, even without a hand-drawn shape above.
        static const PixelRows kCrate = {
            "..OOOOOOOO..",
            ".OHHHHHHHHO.",
            ".OHBBBBBBHO.",
            "OHBBBBBBBBHO",
            "OBBBBBBBBBBO",
            "OBBBBSBBBBBO",
            "OBBBBSBBBBBO",
            "OBSSSSSSSBO.",
            ".OSSSSSSSO..",
            "..OOOOOOOO..",
        };
        rows = &kCrate;
        highlight = shade(accent, 65);
        base = accent;
        shadow = shade(accent, -65);
        accent1 = accent2 = accent;
    }

    int rowCount = static_cast<int>(rows->size());
    int colCount = 0;
    for (const auto& r : *rows) colCount = std::max(colCount, static_cast<int>(r.size()));
    if (rowCount == 0 || colCount == 0) return;

    float pad = size * 0.06f;
    float cell = (size - pad * 2.f) / static_cast<float>(std::max(rowCount, colCount));
    float gridW = cell * static_cast<float>(colCount), gridH = cell * static_cast<float>(rowCount);
    float offX = pos.x + (size - gridW) / 2.f, offY = pos.y + (size - gridH) / 2.f;

    sf::RectangleShape px(sf::Vector2f(cell + 0.6f, cell + 0.6f)); // slight overlap hides seams between pixels
    for (int r = 0; r < rowCount; ++r) {
        const std::string& line = (*rows)[static_cast<size_t>(r)];
        for (int c = 0; c < static_cast<int>(line.size()); ++c) {
            char role = line[static_cast<size_t>(c)];
            sf::Color color;
            switch (role) {
                case 'O': color = outline; break;
                case 'H': color = highlight; break;
                case 'B': color = base; break;
                case 'S': color = shadow; break;
                case 'A': color = accent1; break;
                case 'D': color = accent2; break;
                default: continue; // '.' or anything else -- transparent, card background shows through
            }
            px.setFillColor(color);
            px.setPosition(sf::Vector2f(offX + static_cast<float>(c) * cell, offY + static_cast<float>(r) * cell));
            window.draw(px);
        }
    }
}

sf::FloatRect GameWorld::uiPanelBg(sf::RenderWindow& window, sf::Vector2f pos, sf::Vector2f size) {
    sf::RectangleShape bg(size);
    bg.setPosition(pos);
    bg.setFillColor(sf::Color(35, 38, 48, 235));
    bg.setOutlineThickness(3.f);
    bg.setOutlineColor(sf::Color(232, 212, 120));
    window.draw(bg);
    return sf::FloatRect(pos, size);
}

void GameWorld::uiText(sf::RenderWindow& window, sf::Vector2f pos, const std::string& text, unsigned int size, sf::Color color, bool bold) {
    if (!fontLoaded_) return;
    sf::Text t(font_, toSfString(text), size);
    t.setPosition(pos);
    t.setFillColor(color);
    if (bold) t.setStyle(sf::Text::Bold);
    window.draw(t);
}

std::string GameWorld::applyKeyPlaceholders(const std::string& text) const {
    auto replaceAll = [](std::string s, const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.length(), to);
            pos += to.length();
        }
        return s;
    };
    std::string result = text;
    result = replaceAll(result, "{MOVE}", keyName(settings_.keys.moveUp) + "/" + keyName(settings_.keys.moveLeft) + "/" +
        keyName(settings_.keys.moveDown) + "/" + keyName(settings_.keys.moveRight));
    result = replaceAll(result, "{E}", keyName(settings_.keys.interact));
    result = replaceAll(result, "{U}", keyName(settings_.keys.quickUpgrade));
    result = replaceAll(result, "{M}", keyName(settings_.keys.minimap));
    result = replaceAll(result, "{F}", keyName(settings_.keys.minigame));
    return result;
}

void GameWorld::uiButton(sf::RenderWindow& window, sf::Vector2f pos, sf::Vector2f size, const std::string& label,
    std::function<void()> onClick, bool enabled) {
    sf::RectangleShape btn(size);
    btn.setPosition(pos);
    btn.setFillColor(enabled ? sf::Color(72, 58, 112) : sf::Color(55, 55, 60));
    btn.setOutlineThickness(2.f);
    btn.setOutlineColor(enabled ? sf::Color(232, 212, 120) : sf::Color(110, 110, 110));
    window.draw(btn);

    if (fontLoaded_) {
        sf::Text text(font_, toSfString(label), 14);
        sf::FloatRect b = text.getLocalBounds();
        text.setPosition(sf::Vector2f(pos.x + size.x / 2.f - b.size.x / 2.f - b.position.x, pos.y + size.y / 2.f - 10.f));
        text.setFillColor(enabled ? sf::Color(232, 212, 120) : sf::Color(150, 150, 150));
        window.draw(text);
    }

    if (enabled && onClick) {
        overlayClickRegions_.push_back(ClickRegion{ sf::FloatRect(pos, size), onClick });
    }
}

void GameWorld::performBuy(double qty) {
    auto goods = game_.goodInfos();
    if (selectedGoodIndex_ < 0 || selectedGoodIndex_ >= static_cast<int>(goods.size())) return;
    const std::string id = goods[static_cast<size_t>(selectedGoodIndex_)].id;
    double stockBefore = goods[static_cast<size_t>(selectedGoodIndex_)].stock;
    ActionResult r = game_.tryBuyGood(id, qty);
    if (r.success) {
        // tryBuyGood silently clamps qty to warehouse room, so report what
        // was actually bought (stock delta) rather than the requested amount.
        double actualQty = qty;
        for (const auto& g : game_.goodInfos()) {
            if (g.id == id) { actualQty = g.stock - stockBefore; break; }
        }
        setFeedback(Localization::t("bought_prefix") + formatNumber(actualQty) + " " + Localization::t(id), true);
    } else {
        setFeedback(Localization::t(r.messageKey), false);
    }
}

void GameWorld::performSell(double qty) {
    auto goods = game_.goodInfos();
    if (selectedGoodIndex_ < 0 || selectedGoodIndex_ >= static_cast<int>(goods.size())) return;
    const std::string id = goods[static_cast<size_t>(selectedGoodIndex_)].id;
    ActionResult r = game_.trySellGood(id, qty);
    if (r.success) setFeedback(Localization::t("sold_prefix") + formatNumber(qty) + " " + Localization::t(id), true);
    else setFeedback(Localization::t(r.messageKey), false);
}

void GameWorld::performEat(double qty) {
    ActionResult r = game_.tryEat(qty);
    if (r.success) setFeedback(Localization::t("ate_prefix") + formatNumber(qty), true);
    else setFeedback(Localization::t(r.messageKey), false);
}

void GameWorld::handleUpgradeResult(const ActionResult& r, const std::string& businessId) {
    if (r.success) {
        if (upgradeSound_) upgradeSound_->play();
        std::string msg = Localization::t("upgraded_prefix") + Localization::t(businessId);
        if (r.count > 1) msg += " x" + std::to_string(r.count);
        setFeedback(msg, true);
    } else {
        setFeedback(Localization::t(r.messageKey), false);
    }
}

void GameWorld::performBuildOrUpgrade(const std::string& businessId) {
    ConstructionInfo ci = game_.businessConstructionInfo(businessId);
    if (ci.requiresConstruction) {
        if (ci.inProgress) {
            setFeedback(Localization::t("construction_in_progress_hint"), false);
            return;
        }
        ActionResult r = game_.tryStartConstruction(businessId);
        if (r.success) {
            if (upgradeSound_) upgradeSound_->play();
            setFeedback(Localization::t("construction_started_prefix") + Localization::t(businessId), true);
        } else if (r.messageKey == "construction_missing_materials") {
            setFeedback(Localization::t(r.messageKey) + " " + Localization::t(r.goodId), false);
        } else if (r.messageKey == "not_enough_cash_prefix") {
            setFeedback(Localization::t(r.messageKey) + formatNumber(r.amount), false);
        } else {
            setFeedback(Localization::t(r.messageKey), false);
        }
        return;
    }
    handleUpgradeResult(game_.tryUpgradeBusinessBulk(businessId, 1), businessId);
}

void GameWorld::drawOverlayRoot(sf::RenderWindow& window) {
    if (currentOverlay_ == OverlayKind::None) return;

    sf::RectangleShape dim(sf::Vector2f(static_cast<float>(windowSize_.x), static_cast<float>(windowSize_.y)));
    dim.setFillColor(sf::Color(0, 0, 0, 150));
    window.draw(dim);

    overlayClickRegions_.clear();

    switch (currentOverlay_) {
        case OverlayKind::Businesses:   drawBusinessesOverlay(window); break;
        case OverlayKind::Tree:         drawTreeOverlay(window); break;
        case OverlayKind::Market:       drawMarketOverlay(window); break;
        case OverlayKind::Staff:        drawStaffOverlay(window); break;
        case OverlayKind::Sleep:        drawSleepOverlay(window); break;
        case OverlayKind::Eat:          drawEatOverlay(window); break;
        case OverlayKind::Doctor:       drawDoctorOverlay(window); break;
        case OverlayKind::FastForward:  drawFastForwardOverlay(window); break;
        case OverlayKind::Achievements: drawAchievementsOverlay(window); break;
        case OverlayKind::DeathNotice:  drawDeathNoticeOverlay(window); break;
        case OverlayKind::Legacy:       drawLegacyOverlay(window); break;
        case OverlayKind::Dialogue:     drawDialogueOverlay(window); break;
        case OverlayKind::Bank:         drawBankOverlay(window); break;
        case OverlayKind::Warehouse:    drawWarehouseOverlay(window); break;
        case OverlayKind::HowToPlay:    drawHowToPlayOverlay(window); break;
        case OverlayKind::CropPicker:   drawCropPickerOverlay(window); break;
        case OverlayKind::RecipeBook:   drawRecipeBookOverlay(window); break;
        case OverlayKind::TimingMinigame: drawTimingMinigameOverlay(window); break;
        case OverlayKind::Chopping:       drawChoppingOverlay(window); break;
        case OverlayKind::Brewing:        drawBrewingOverlay(window); break;
        case OverlayKind::Contracts:    drawContractsOverlay(window); break;
        case OverlayKind::Pause:        drawPauseOverlay(window); break;
        case OverlayKind::Settings:     drawSettingsOverlay(window); break;
        case OverlayKind::WelcomeBack:  drawWelcomeBackOverlay(window); break;
        case OverlayKind::AutoSell:     drawAutoSellOverlay(window); break;
        default: break;
    }
}

void GameWorld::drawBusinessesOverlay(sf::RenderWindow& window) {
    // Focused on whichever single building was walked up to (see
    // handleInteraction) -- not the full 12-row list, so upgrading here can
    // only ever affect the one building the player actually approached.
    // Keep the vector itself alive for the whole function -- businessInfos()
    // returns by value, and a range-for over a temporary only lifetime-extends
    // it for the loop itself, so `info` would otherwise dangle the moment the
    // loop below ends (use-after-free once anything below reads through it).
    const std::vector<BusinessInfo> infos = game_.businessInfos();
    const BusinessInfo* info = nullptr;
    for (const auto& b : infos) {
        if (b.id == focusedBusinessId_) { info = &b; break; }
    }
    if (!info) { closeOverlay(); return; } // shouldn't happen, but don't render garbage if it does

    sf::Vector2f pos(360.f, 230.f), size(600.f, 460.f);
    uiPanelBg(window, pos, size);
    uiButton(window, { pos.x + size.x - 120.f, pos.y + 14.f }, { 100.f, 34.f }, Localization::t("close_button"), [this]() { closeOverlay(); });

    sf::Color tierColor = info->tier <= 1 ? kTier1 : (info->tier == 2 ? kTier2 : kTier3);
    uiText(window, { pos.x + 24.f, pos.y + 16.f }, Localization::t(info->id), 22, tierColor, true);

    // Storefront is the one business whose outputGoodId is empty -- it pays
    // straight into money_ instead of stocking a market good (see
    // simulateElapsed's `if (outputGoodId.empty()) money_ += amount;`
    // branch). Everything else in this overlay looks identical either way,
    // so without this line Storefront reads as "does nothing".
    if (info->outputGoodId.empty()) {
        uiText(window, { pos.x + 24.f, pos.y + 46.f }, Localization::t("storefront_desc"), 13, sf::Color(160, 160, 160));
    }

    float y = pos.y + 74.f;
    uiText(window, { pos.x + 24.f, y }, Localization::t("col_level") + ": " + std::to_string(info->level), 16);
    y += 32.f;

    std::ostringstream rateOss;
    rateOss << std::fixed << std::setprecision(3) << info->ratePerSecond;
    uiText(window, { pos.x + 24.f, y }, Localization::t("col_rate") + ": " + rateOss.str(), 16);
    y += 20.f;
    uiText(window, { pos.x + 24.f, y }, Localization::t("rate_note"), 11, sf::Color(160, 160, 160));
    y += 24.f;

    // Live "need vs have" list (see BusinessInfo::inputs) -- primary input
    // first, then any BusinessType::extraInputs for a multi-input recipe
    // (Cake Shop, Artisan Bakery, ...). Zero stock of any one of these
    // means production is fully bottlenecked on it (see simulateElapsed's
    // Pass 2), which is what the red/green coloring flags, not a one-time
    // total the way the construction material list's coloring works.
    // Only shown once actually built -- an unbuilt (level 0) business shows
    // its construction material list instead (see the ConstructionInfo
    // block below), and showing both at once would be redundant/confusing.
    if (info->level > 0 && !info->inputs.empty()) {
        uiText(window, { pos.x + 24.f, y }, Localization::t("col_needs"), 13, sf::Color(200, 200, 200));
        y += 22.f;
        for (const auto& in : info->inputs) {
            bool producing = in.have > 0.0;
            std::ostringstream line;
            line << Localization::t(in.goodId) << " -" << std::fixed << std::setprecision(2) << in.required
                << " (" << Localization::t("input_have_label") << formatNumber(in.have) << ")";
            uiText(window, { pos.x + 24.f, y }, line.str(), 15, producing ? sf::Color(160, 220, 160) : sf::Color(220, 140, 140));
            y += 22.f;
        }
        y += 10.f;
    }
    uiText(window, { pos.x + 24.f, y }, Localization::t("col_cost") + ": $" + formatNumber(info->nextCost), 16);
    y += 40.f;

    ConstructionInfo ci = game_.businessConstructionInfo(info->id);

    if (info->locked) {
        // Not reachable in practice (handleInteraction filters locked
        // buildings out before opening this overlay) -- kept as a fallback
        // rather than assuming that check can never change.
        uiText(window, { pos.x + 24.f, y }, Localization::t("locked_label"), 16, sf::Color(180, 120, 120));
        y += 70.f;
    } else if (ci.requiresConstruction && info->level == 0) {
        // First build of a non-starter business (see Business::
        // constructionDaysRemaining) -- replaces the plain upgrade buttons
        // below with either a materials shopping list + Start Construction,
        // or (once started) a read-only progress readout.
        if (ci.inProgress) {
            int daysLeft = static_cast<int>(std::ceil(ci.daysRemaining));
            uiText(window, { pos.x + 24.f, y }, Localization::t("construction_site_days_left_prefix") +
                std::to_string(daysLeft) + Localization::t("construction_site_days_left_suffix"), 16, sf::Color(232, 212, 120));
            y += 34.f;
            float barW = size.x - 48.f, barH = 16.f;
            sf::RectangleShape barBg(sf::Vector2f(barW, barH));
            barBg.setPosition(sf::Vector2f(pos.x + 24.f, y));
            barBg.setFillColor(sf::Color(30, 30, 34));
            barBg.setOutlineThickness(1.f);
            barBg.setOutlineColor(sf::Color(90, 90, 96));
            window.draw(barBg);
            double totalDays = std::max(1, ci.totalDays);
            float progress = static_cast<float>(std::clamp(1.0 - (ci.daysRemaining / totalDays), 0.0, 1.0));
            sf::RectangleShape barFill(sf::Vector2f(barW * progress, barH));
            barFill.setPosition(sf::Vector2f(pos.x + 24.f, y));
            barFill.setFillColor(sf::Color(232, 212, 120));
            window.draw(barFill);
            y += barH + 16.f;
            // A real but not painless way out of a misclick -- half of
            // whatever was already spent (cash + materials) comes back (see
            // Game::tryCancelConstruction), not a free do-over.
            uiButton(window, { pos.x + 24.f, y }, { 220.f, 38.f }, Localization::t("cancel_construction_button"),
                [this, id = info->id]() {
                    ActionResult r = game_.tryCancelConstruction(id);
                    if (r.success) setFeedback(Localization::t("construction_cancelled_prefix") + formatNumber(r.amount), true);
                    else setFeedback(Localization::t(r.messageKey), false);
                });
            y += 54.f;
        } else {
            uiText(window, { pos.x + 24.f, y - 20.f }, Localization::t("construction_materials_header"), 13, sf::Color(200, 200, 200));
            bool allAffordable = game_.money() >= info->nextCost;
            for (const auto& m : ci.materials) {
                bool enough = m.have >= m.required;
                if (!enough) allAffordable = false;
                std::ostringstream line;
                line << Localization::t(m.goodId) << ": " << formatNumber(m.have) << " / " << formatNumber(m.required);
                uiText(window, { pos.x + 24.f, y }, line.str(), 14, enough ? sf::Color(160, 220, 160) : sf::Color(220, 140, 140));
                y += 22.f;
            }
            y += 12.f;
            uiButton(window, { pos.x + 24.f, y }, { 220.f, 46.f }, Localization::t("start_construction_button"),
                [this, id = info->id]() {
                    ActionResult r = game_.tryStartConstruction(id);
                    if (r.success) {
                        if (upgradeSound_) upgradeSound_->play();
                        setFeedback(Localization::t("construction_started_prefix") + Localization::t(id), true);
                    } else if (r.messageKey == "construction_missing_materials") {
                        setFeedback(Localization::t(r.messageKey) + " " + Localization::t(r.goodId), false);
                    } else if (r.messageKey == "not_enough_cash_prefix") {
                        setFeedback(Localization::t(r.messageKey) + formatNumber(r.amount), false);
                    } else {
                        setFeedback(Localization::t(r.messageKey), false);
                    }
                }, allAffordable);
            y += 60.f;
        }
    } else {
        uiText(window, { pos.x + 24.f, y - 20.f }, Localization::t("upgrade_button"), 13, sf::Color(200, 200, 200));
        float btnW = 168.f, btnH = 46.f, gap = 12.f;
        uiButton(window, { pos.x + 24.f, y }, { btnW, btnH }, Localization::t("qty_1"),
            [this, id = info->id]() { handleUpgradeResult(game_.tryUpgradeBusinessBulk(id, 1), id); });
        uiButton(window, { pos.x + 24.f + (btnW + gap), y }, { btnW, btnH }, Localization::t("qty_10"),
            [this, id = info->id]() { handleUpgradeResult(game_.tryUpgradeBusinessBulk(id, 10), id); });
        uiButton(window, { pos.x + 24.f + 2.f * (btnW + gap), y }, { btnW, btnH }, Localization::t("qty_all"),
            [this, id = info->id]() { handleUpgradeResult(game_.tryUpgradeBusinessBulk(id, -1), id); });
        y += 70.f;
    }

    // Per-business worker hiring (separate from the global Staff Office +
    // foreman focus, see Game::tryHireWorker) -- only meaningful once the
    // business is actually built.
    if (info->level > 0) {
        uiText(window, { pos.x + 24.f, y }, Localization::t("workers_label") + std::to_string(info->workers) +
            "/" + std::to_string(Game::kMaxWorkersPerBusiness), 15, sf::Color(200, 200, 200));
        y += 26.f;
        bool canHire = info->workers < Game::kMaxWorkersPerBusiness;
        std::string hireLabel = Localization::t("hire_worker_button");
        if (canHire) hireLabel += " ($" + formatNumber(info->workerCost) + ")";
        uiButton(window, { pos.x + 24.f, y }, { 240.f, 40.f }, hireLabel,
            [this, id = info->id]() {
                ActionResult r = game_.tryHireWorker(id);
                if (r.success) {
                    if (upgradeSound_) upgradeSound_->play();
                    setFeedback(Localization::t("worker_hired_prefix") + Localization::t(id), true);
                } else {
                    setFeedback(Localization::t(r.messageKey), false);
                }
            }, canHire);

        // The Farm alone also gets a crop picker (see Business::cropId) --
        // shown beside the hire-worker button, plus a line naming the
        // currently-active crop and whether it's in its favorite season.
        if (info->id == "farm") {
            uiButton(window, { pos.x + 24.f + 240.f + 16.f, y }, { 200.f, 40.f }, Localization::t("change_crop_button"),
                [this]() { openOverlay(OverlayKind::CropPicker); });
            y += 46.f;
            std::string cropLabel = Localization::t("current_crop_label") + Localization::t(info->cropId);
            if (info->seasonBonusActive) cropLabel += Localization::t("season_bonus_active");
            uiText(window, { pos.x + 24.f, y }, cropLabel, 14, sf::Color(232, 212, 120));
        }

        // The Port alone gets the ship-commission/sail step (see
        // Game::tryCommissionShip and GameWorld's Zone 7) -- shown beside
        // the hire-worker button just like the Farm's crop picker above.
        if (info->id == "port") {
            if (!game_.hasIslandShip()) {
                std::ostringstream label;
                label << Localization::t("commission_ship_button") << " ("
                    << Game::kShipCommissionShips << " " << Localization::t("ships") << " + $" << Game::kShipCommissionCash << ")";
                uiButton(window, { pos.x + 24.f + 240.f + 16.f, y }, { 260.f, 40.f }, label.str(),
                    [this]() {
                        ActionResult r = game_.tryCommissionShip();
                        if (r.success) {
                            if (upgradeSound_) upgradeSound_->play();
                            setFeedback(Localization::t("ship_commissioned_prefix"), true);
                        } else if (r.messageKey == "construction_missing_materials") {
                            setFeedback(Localization::t(r.messageKey) + " " + Localization::t(r.goodId), false);
                        } else if (r.messageKey == "not_enough_cash_prefix") {
                            setFeedback(Localization::t(r.messageKey) + formatNumber(r.amount), false);
                        } else {
                            setFeedback(Localization::t(r.messageKey), false);
                        }
                    });
            } else {
                uiButton(window, { pos.x + 24.f + 240.f + 16.f, y }, { 260.f, 40.f }, Localization::t("sail_button"),
                    [this]() {
                        closeOverlay();
                        currentZone_ = kFisherIsleZoneIndex;
                        playerPos_ = kFisherIsleArrivalPos;
                        game_.markIslandVisited();
                        setFeedback(Localization::t("arrived_at_isle"), true);
                    });
            }
        }

        // The Storefront alone gets an auto-sell config button (see
        // Business::autoSellGoodId/autoSellThreshold and
        // drawAutoSellOverlay), same "shown beside hire-worker" placement as
        // the Farm's crop picker and Port's ship button above.
        if (info->id == "storefront") {
            uiButton(window, { pos.x + 24.f + 240.f + 16.f, y }, { 240.f, 40.f }, Localization::t("autosell_configure_button"),
                [this]() { openOverlay(OverlayKind::AutoSell); });
            y += 46.f;
            StorefrontAutoSellInfo as = game_.storefrontAutoSellInfo();
            std::string summary = as.goodId.empty()
                ? Localization::t("autosell_disabled_label")
                : Localization::t("autosell_summary_prefix") + Localization::t(as.goodId) +
                  " @ $" + formatNumber(as.threshold) + " (" + formatNumber(as.capacityPerDay) + Localization::t("autosell_per_day_suffix") + ")";
            uiText(window, { pos.x + 24.f, y }, summary, 14, sf::Color(232, 212, 120));
        }
    }

    if (!overlayFeedback_.empty()) {
        uiText(window, { pos.x + 24.f, pos.y + size.y - 30.f }, overlayFeedback_, 15, overlayFeedbackColor_);
    }
}

void GameWorld::drawTreeOverlay(sf::RenderWindow& window) {
    sf::Vector2f pos(140.f, 40.f), size(1000.f, 740.f);
    uiPanelBg(window, pos, size);
    uiButton(window, { pos.x + size.x - 130.f, pos.y + 14.f }, { 110.f, 36.f }, Localization::t("close_button"), [this]() { closeOverlay(); });
    uiButton(window, { pos.x + size.x - 250.f, pos.y + 14.f }, { 110.f, 36.f }, Localization::t("legacy_button"), [this]() { openOverlay(OverlayKind::Legacy); });

    // The production tree has grown well past what fits in one screen (42
    // businesses now, vs. the dozen or so this panel was originally sized
    // for) -- scroll with the mouse wheel (see the MouseWheelScrolled
    // handler in run()) instead of just letting lines run off the bottom.
    constexpr float lineH = 24.f;
    constexpr float contentTop = 60.f;   // offset from pos.y where the first line sits
    constexpr float contentBottom = 20.f; // bottom margin reserved (also where the scroll hint sits)
    float visibleH = size.y - contentTop - contentBottom;

    std::vector<std::string> lines = game_.productionTreeLines();
    float contentH = static_cast<float>(lines.size()) * lineH;
    float maxScroll = std::max(0.f, contentH - visibleH);
    overlayScrollOffset_ = std::clamp(overlayScrollOffset_, 0.f, maxScroll);

    float y = pos.y + contentTop - overlayScrollOffset_;
    beginClip(window, sf::FloatRect(sf::Vector2f(pos.x, pos.y + contentTop), sf::Vector2f(size.x, visibleH)));
    for (const auto& line : lines) {
        // Skip lines scrolled out of view instead of drawing them over the
        // panel's own header/buttons above or past its bottom edge.
        if (y >= pos.y + contentTop - lineH && y <= pos.y + size.y - contentBottom) {
            // A small tier-colored square ahead of each real row (skips the
            // title/separator lines, which don't contain " -> ") -- depth is
            // just the leading-space count / 3, matching collectTreeLines'
            // own indent step (Business.cpp).
            if (line.find(" -> ") != std::string::npos) {
                size_t leading = line.find_first_not_of(' ');
                int depth = leading == std::string::npos ? 0 : static_cast<int>(leading / 3);
                sf::Color tierColor = depth <= 0 ? kTier1 : (depth == 1 ? kTier2 : kTier3);
                sf::RectangleShape icon(sf::Vector2f(9.f, 9.f));
                icon.setPosition(sf::Vector2f(pos.x + 8.f, y + 5.f));
                icon.setFillColor(tierColor);
                window.draw(icon);
            }
            uiText(window, { pos.x + 24.f, y }, line, 15, sf::Color::White);
        }
        y += lineH;
    }
    endClip(window);

    if (maxScroll > 0.f) {
        uiText(window, { pos.x + 24.f, pos.y + size.y - 18.f }, Localization::t("scroll_hint"), 12, sf::Color(160, 160, 160));
    }
}

void GameWorld::drawMarketOverlay(sf::RenderWindow& window) {
    sf::Vector2f pos(100.f, 40.f), size(1080.f, 740.f);
    uiPanelBg(window, pos, size);
    uiText(window, { pos.x + 24.f, pos.y + 16.f }, Localization::t("menu_market_header"), 20, sf::Color(232, 212, 120), true);
    uiButton(window, { pos.x + size.x - 130.f, pos.y + 14.f }, { 110.f, 36.f }, Localization::t("close_button"), [this]() { closeOverlay(); });
    uiButton(window, { pos.x + size.x - 260.f, pos.y + 14.f }, { 120.f, 36.f }, Localization::t("contracts_button"), [this]() { openOverlay(OverlayKind::Contracts); });

    auto goods = game_.goodInfos();
    if (selectedGoodIndex_ < 0 || selectedGoodIndex_ >= static_cast<int>(goods.size())) selectedGoodIndex_ = 0;

    // ---- Filter tabs + sort cycle button ----
    // The goods list grew to 61 entries in one flat unsorted block, which is
    // what actually made the shop feel "messy" -- these two controls don't
    // change what's for sale, just how it's browsed. Filter tabs by
    // production tier (raw material vs. processed, matching BusinessInfo::
    // tier) plus an "owned" tab; sort cycles through the orderings players
    // actually want a price list in (name, price, quantity), one click at a
    // time via a single button rather than a whole extra dropdown widget.
    auto businesses = game_.businessInfos();
    static const std::unordered_set<std::string> kCropIds = {
        "wheat", "strawberry", "corn", "watermelon", "pumpkin", "sweetpotato", "cabbage"
    };
    auto tierForGood = [&](const std::string& goodId) -> int {
        // Every crop id is a raw Farm output regardless of which one is
        // currently planted (businessInfos() only reports the *active*
        // crop's outputGoodId for "farm" -- see Business.cpp's
        // resolvedOutputGoodId), so the other six need this explicit fallback.
        if (kCropIds.count(goodId)) return 1;
        for (const auto& b : businesses) if (b.outputGoodId == goodId) return b.tier;
        return 1; // not produced by any business -- shouldn't happen, default to raw
    };

    auto drawTab = [&](sf::Vector2f p, sf::Vector2f sz, const std::string& label, bool active, std::function<void()> onClick) {
        sf::RectangleShape btn(sz);
        btn.setPosition(p);
        btn.setFillColor(active ? sf::Color(90, 76, 130) : sf::Color(50, 52, 62));
        btn.setOutlineThickness(2.f);
        btn.setOutlineColor(active ? sf::Color(232, 212, 120) : sf::Color(90, 90, 100));
        window.draw(btn);
        if (fontLoaded_) {
            sf::Text text(font_, toSfString(label), 13);
            sf::FloatRect b = text.getLocalBounds();
            text.setPosition(sf::Vector2f(p.x + sz.x / 2.f - b.size.x / 2.f - b.position.x, p.y + sz.y / 2.f - 9.f));
            text.setFillColor(active ? sf::Color(232, 212, 120) : sf::Color(210, 210, 210));
            window.draw(text);
        }
        overlayClickRegions_.push_back(ClickRegion{ sf::FloatRect(p, sz), onClick });
    };

    float tabsY = pos.y + 54.f, tabH = 30.f, tabGap = 8.f;
    struct FilterTab { MarketFilter filter; const char* labelKey; };
    static const FilterTab kFilterTabs[] = {
        { MarketFilter::All,       "market_filter_all" },
        { MarketFilter::Raw,       "market_filter_raw" },
        { MarketFilter::Processed, "market_filter_processed" },
        { MarketFilter::Owned,     "market_filter_owned" },
    };
    float tabX = pos.x + 24.f;
    for (const auto& tab : kFilterTabs) {
        std::string label = Localization::t(tab.labelKey);
        float tabW = fontLoaded_ ? std::max(90.f, sf::Text(font_, toSfString(label), 13).getLocalBounds().size.x + 28.f) : 120.f;
        drawTab(sf::Vector2f(tabX, tabsY), sf::Vector2f(tabW, tabH), label, marketFilter_ == tab.filter,
            [this, f = tab.filter]() { marketFilter_ = f; overlayScrollOffset_ = 0.f; });
        tabX += tabW + tabGap;
    }

    static const std::unordered_map<MarketSort, const char*> kSortLabels = {
        { MarketSort::Default,      "market_sort_default" },
        { MarketSort::NameAsc,      "market_sort_name" },
        { MarketSort::PriceHighLow, "market_sort_price_desc" },
        { MarketSort::PriceLowHigh, "market_sort_price_asc" },
        { MarketSort::StockHighLow, "market_sort_stock_desc" },
        { MarketSort::StockLowHigh, "market_sort_stock_asc" },
    };
    static const MarketSort kSortCycle[] = {
        MarketSort::Default, MarketSort::NameAsc, MarketSort::PriceHighLow,
        MarketSort::PriceLowHigh, MarketSort::StockHighLow, MarketSort::StockLowHigh
    };
    std::string sortLabel = Localization::t("market_sort_prefix") + Localization::t(kSortLabels.at(marketSort_));
    float sortW = 230.f;
    drawTab(sf::Vector2f(pos.x + size.x - 24.f - sortW, tabsY), sf::Vector2f(sortW, tabH), sortLabel, false, [this]() {
        size_t n = sizeof(kSortCycle) / sizeof(kSortCycle[0]);
        for (size_t i = 0; i < n; ++i) {
            if (kSortCycle[i] == marketSort_) { marketSort_ = kSortCycle[(i + 1) % n]; break; }
        }
    });

    float colName = pos.x + 24.f, colPrice = pos.x + 400.f, colHold = pos.x + 600.f;
    float headerY = tabsY + tabH + 14.f;
    uiText(window, { colName, headerY }, Localization::t("col_good"), 13, sf::Color(200, 200, 200));
    uiText(window, { colPrice, headerY }, Localization::t("col_price"), 13, sf::Color(200, 200, 200));
    uiText(window, { colHold, headerY }, Localization::t("col_hold"), 13, sf::Color(200, 200, 200));

    // Apply the active filter, then the active sort, onto a list of
    // (original index into `goods`, GoodInfo) pairs -- the original index is
    // what selectedGoodIndex_/performBuy/performSell actually key off of, so
    // it has to survive the reorder/filtering below.
    struct DisplayGood { size_t origIndex; GoodInfo info; };
    std::vector<DisplayGood> display;
    display.reserve(goods.size());
    for (size_t i = 0; i < goods.size(); ++i) {
        bool pass = true;
        switch (marketFilter_) {
            case MarketFilter::Raw:       pass = tierForGood(goods[i].id) <= 1; break;
            case MarketFilter::Processed: pass = tierForGood(goods[i].id) >= 2; break;
            case MarketFilter::Owned:     pass = goods[i].stock > 0.0001; break;
            default: break;
        }
        if (pass) display.push_back({ i, goods[i] });
    }
    switch (marketSort_) {
        case MarketSort::NameAsc:
            std::stable_sort(display.begin(), display.end(), [](const DisplayGood& a, const DisplayGood& b) {
                return Localization::t(a.info.id) < Localization::t(b.info.id);
            });
            break;
        case MarketSort::PriceHighLow:
            std::stable_sort(display.begin(), display.end(), [](const DisplayGood& a, const DisplayGood& b) { return a.info.price > b.info.price; });
            break;
        case MarketSort::PriceLowHigh:
            std::stable_sort(display.begin(), display.end(), [](const DisplayGood& a, const DisplayGood& b) { return a.info.price < b.info.price; });
            break;
        case MarketSort::StockHighLow:
            std::stable_sort(display.begin(), display.end(), [](const DisplayGood& a, const DisplayGood& b) { return a.info.stock > b.info.stock; });
            break;
        case MarketSort::StockLowHigh:
            std::stable_sort(display.begin(), display.end(), [](const DisplayGood& a, const DisplayGood& b) { return a.info.stock < b.info.stock; });
            break;
        default: break; // registration order, already the order goods[] is in
    }

    // The goods list has outgrown the panel (61 goods now, vs. the ~15 this
    // was sized for) -- bound it above the fixed buy/sell panel below and
    // scroll with the mouse wheel instead of drawing rows over top of it.
    float listTop = headerY + 26.f, rowH = 34.f;
    float actionY = pos.y + size.y - 130.f;
    float listBottom = actionY - 10.f;
    float contentH = static_cast<float>(display.size()) * rowH;
    float maxScroll = std::max(0.f, contentH - (listBottom - listTop));
    overlayScrollOffset_ = std::clamp(overlayScrollOffset_, 0.f, maxScroll);

    if (display.empty()) {
        uiText(window, { pos.x + 24.f, listTop }, Localization::t("market_filter_empty_hint"), 14, sf::Color(160, 160, 160));
    }

    float rowY = listTop - overlayScrollOffset_;
    beginClip(window, sf::FloatRect(sf::Vector2f(pos.x, listTop), sf::Vector2f(size.x, listBottom - listTop)));
    for (const auto& d : display) {
        if (rowY < listTop - rowH || rowY > listBottom) { rowY += rowH; continue; }
        const auto& g = d.info;
        bool selected = static_cast<int>(d.origIndex) == selectedGoodIndex_;
        sf::RectangleShape rowBg(sf::Vector2f(size.x - 48.f, rowH - 4.f));
        rowBg.setPosition(sf::Vector2f(pos.x + 24.f, rowY));
        rowBg.setFillColor(selected ? sf::Color(90, 76, 130) : sf::Color(50, 52, 62));
        window.draw(rowBg);
        // Click region is clamped to the visible list band too -- otherwise a
        // row scrolled half off the top/bottom edge would still catch clicks
        // in the space above/below the panel where it's no longer drawn.
        float clickTop = std::max(rowY, listTop), clickBottom = std::min(rowY + (rowH - 4.f), listBottom);
        if (clickBottom > clickTop) {
            size_t origIndex = d.origIndex;
            overlayClickRegions_.push_back(ClickRegion{
                sf::FloatRect(sf::Vector2f(pos.x + 24.f, clickTop), sf::Vector2f(size.x - 48.f, clickBottom - clickTop)),
                [this, origIndex]() { selectedGoodIndex_ = static_cast<int>(origIndex); } });
        }

        uiText(window, { colName, rowY + 6.f }, Localization::t(g.id), 15, sf::Color::White);
        uiText(window, { colPrice, rowY + 6.f }, "$" + formatNumber(g.price), 15);
        uiText(window, { colHold, rowY + 6.f }, formatNumber(g.stock), 15);
        rowY += rowH;
    }
    endClip(window);
    if (maxScroll > 0.f) {
        uiText(window, { colHold + 120.f, headerY }, Localization::t("scroll_hint"), 12, sf::Color(160, 160, 160));
    }

    const GoodInfo& sel = goods[static_cast<size_t>(selectedGoodIndex_)];
    uiText(window, { pos.x + 24.f, actionY },
        Localization::t("selected_good_label") + Localization::t(sel.id) + "  ($" + formatNumber(sel.price) + ")",
        16, sf::Color(232, 212, 120), true);

    float btnY = actionY + 36.f, btnW = 90.f, btnH = 36.f, gap = 10.f;
    uiText(window, { pos.x + 24.f, btnY - 20.f }, Localization::t("buy_button"), 13, sf::Color(200, 200, 200));
    uiButton(window, { pos.x + 24.f, btnY }, { btnW, btnH }, Localization::t("qty_1"), [this]() { performBuy(1.0); });
    uiButton(window, { pos.x + 24.f + (btnW + gap), btnY }, { btnW, btnH }, Localization::t("qty_10"), [this]() { performBuy(10.0); });
    uiButton(window, { pos.x + 24.f + 2.f * (btnW + gap), btnY }, { btnW, btnH }, Localization::t("qty_100"), [this]() { performBuy(100.0); });

    float sellX = pos.x + 24.f + 4.f * (btnW + gap);
    uiText(window, { sellX, btnY - 20.f }, Localization::t("sell_button"), 13, sf::Color(200, 200, 200));
    uiButton(window, { sellX, btnY }, { btnW, btnH }, Localization::t("qty_1"), [this]() { performSell(1.0); });
    uiButton(window, { sellX + (btnW + gap), btnY }, { btnW, btnH }, Localization::t("qty_10"), [this]() { performSell(10.0); });
    uiButton(window, { sellX + 2.f * (btnW + gap), btnY }, { btnW, btnH }, Localization::t("qty_100"), [this]() { performSell(100.0); });
    double stockForAll = sel.stock;
    uiButton(window, { sellX + 3.f * (btnW + gap), btnY }, { btnW, btnH }, Localization::t("qty_all"),
        [this, stockForAll]() { performSell(stockForAll); }, stockForAll > 0.0);

    if (!overlayFeedback_.empty()) {
        uiText(window, { pos.x + 24.f, pos.y + size.y - 30.f }, overlayFeedback_, 15, overlayFeedbackColor_);
    }
}

void GameWorld::drawStaffOverlay(sf::RenderWindow& window) {
    sf::Vector2f pos(320.f, 170.f), size(640.f, 480.f);
    uiPanelBg(window, pos, size);
    uiText(window, { pos.x + 24.f, pos.y + 16.f }, Localization::t("menu_staff_header"), 20, sf::Color(232, 212, 120), true);
    uiButton(window, { pos.x + size.x - 120.f, pos.y + 14.f }, { 100.f, 34.f }, Localization::t("close_button"), [this]() { closeOverlay(); });

    std::ostringstream info;
    info << Localization::t("staff_current_prefix") << game_.staffLevel()
        << Localization::t("staff_current_suffix") << std::fixed << std::setprecision(2) << game_.staffMultiplier() << ")";
    uiText(window, { pos.x + 24.f, pos.y + 70.f }, info.str(), 15);
    uiText(window, { pos.x + 24.f, pos.y + 104.f }, Localization::t("staff_cost_prefix") + formatNumber(game_.staffNextCost()), 15);

    uiButton(window, { pos.x + 24.f, pos.y + 140.f }, { 200.f, 44.f }, Localization::t("hire_button"), [this]() {
        ActionResult r = game_.tryHireStaff();
        if (r.success) setFeedback(Localization::t("staff_hired_prefix") + std::to_string(game_.staffLevel()), true);
        else setFeedback(Localization::t(r.messageKey), false);
    });

    std::string focusName = game_.staffFocusBusinessId().empty()
        ? Localization::t("staff_focus_none") : Localization::t(game_.staffFocusBusinessId());
    std::ostringstream focusOss;
    focusOss << Localization::t("staff_focus_label") << focusName
        << Localization::t("staff_focus_suffix") << std::fixed << std::setprecision(0) << game_.staffFocusBonusPercentPerLevel() << "%/lvl)";
    uiText(window, { pos.x + 24.f, pos.y + 208.f }, focusOss.str(), 15, sf::Color(232, 212, 120));
    // Focus (this screen) and per-business Workers (hired from that
    // business's own overlay) are two separate, stackable bonuses -- easy to
    // mistake for the same thing since both just multiply output.
    uiText(window, { pos.x + 24.f, pos.y + 230.f }, Localization::t("staff_focus_clarify"), 12, sf::Color(160, 160, 160));

    // Pick which owned business the foreman focuses on -- a compact wrapping
    // grid of buttons. Collected into a list first (rather than placed
    // as each is discovered) so the total count is known up front, needed
    // to scroll it now that a maxed-out empire (43 businesses) meaningfully
    // overruns the panel, which the original fixed 5-column grid didn't
    // account for.
    struct FocusOption { std::string label; std::function<void()> onClick; };
    std::vector<FocusOption> options;
    options.push_back({ Localization::t("staff_focus_none"), [this]() {
        game_.trySetStaffFocus("");
        setFeedback(Localization::t("staff_focus_cleared"), true);
    } });
    for (const auto& b : game_.businessInfos()) {
        if (b.level <= 0) continue;
        options.push_back({ Localization::t(b.id), [this, id = b.id]() {
            ActionResult r = game_.trySetStaffFocus(id);
            if (r.success) setFeedback(Localization::t("staff_focus_set_prefix") + Localization::t(id), true);
            else setFeedback(Localization::t(r.messageKey), false);
        } });
    }

    float startX = pos.x + 24.f;
    float btnW = 108.f, btnH = 30.f, gapX = 8.f, gapY = 8.f;
    constexpr int perRow = 5;
    int totalRows = static_cast<int>((options.size() + perRow - 1) / static_cast<size_t>(perRow));
    float gridTop = pos.y + 256.f; // pushed down 16px from before to clear the new staff_focus_clarify line above
    float gridBottom = pos.y + size.y - 40.f; // leaves room for the feedback line below
    float contentH = static_cast<float>(totalRows) * (btnH + gapY);
    float maxScroll = std::max(0.f, contentH - (gridBottom - gridTop));
    overlayScrollOffset_ = std::clamp(overlayScrollOffset_, 0.f, maxScroll);

    beginClip(window, sf::FloatRect(sf::Vector2f(pos.x, gridTop), sf::Vector2f(size.x, gridBottom - gridTop)));
    for (size_t i = 0; i < options.size(); ++i) {
        int row = static_cast<int>(i / static_cast<size_t>(perRow));
        int col = static_cast<int>(i % static_cast<size_t>(perRow));
        float rowY = gridTop + static_cast<float>(row) * (btnH + gapY) - overlayScrollOffset_;
        if (rowY < gridTop - btnH || rowY > gridBottom) continue;
        float colX = startX + static_cast<float>(col) * (btnW + gapX);
        // uiButton registers its own click region at the *unclipped* {colX,
        // rowY} rect; a row only partially in the visible band would
        // otherwise still catch clicks over its clipped-away portion, so
        // only register buttons that are fully inside the visible band
        // (drawn either way -- this only suppresses the click region).
        bool fullyVisible = rowY >= gridTop && rowY + btnH <= gridBottom;
        uiButton(window, { colX, rowY }, { btnW, btnH }, options[i].label, options[i].onClick, fullyVisible);
    }
    endClip(window);
    if (maxScroll > 0.f) {
        uiText(window, { pos.x + size.x - 210.f, pos.y + 210.f }, Localization::t("scroll_hint"), 12, sf::Color(160, 160, 160));
    }

    if (!overlayFeedback_.empty()) {
        uiText(window, { pos.x + 24.f, pos.y + size.y - 34.f }, overlayFeedback_, 15, overlayFeedbackColor_);
    }
}

void GameWorld::drawSleepOverlay(sf::RenderWindow& window) {
    sf::Vector2f pos(360.f, 260.f), size(560.f, 300.f);
    uiPanelBg(window, pos, size);
    uiText(window, { pos.x + 24.f, pos.y + 16.f }, Localization::t("menu_sleep_header"), 20, sf::Color(232, 212, 120), true);
    uiButton(window, { pos.x + size.x - 120.f, pos.y + 14.f }, { 100.f, 34.f }, Localization::t("close_button"), [this]() { closeOverlay(); });

    uiText(window, { pos.x + 24.f, pos.y + 70.f }, Localization::t("sleep_desc_prefix"), 14);
    uiText(window, { pos.x + 24.f, pos.y + 100.f }, Localization::t("sleep_desc_suffix"), 14);

    uiButton(window, { pos.x + 24.f, pos.y + 160.f }, { 200.f, 44.f }, Localization::t("sleep_button"), [this]() {
        TickOutcome outcome = game_.trySleep();
        if (outcome.died) { handleTickOutcome(outcome); return; }
        setFeedback(Localization::t("sleep_woke"), true);
    });

    if (!overlayFeedback_.empty()) {
        uiText(window, { pos.x + 24.f, pos.y + size.y - 34.f }, overlayFeedback_, 15, overlayFeedbackColor_);
    }
}

void GameWorld::drawEatOverlay(sf::RenderWindow& window) {
    sf::Vector2f pos(360.f, 260.f), size(560.f, 300.f);
    uiPanelBg(window, pos, size);
    uiText(window, { pos.x + 24.f, pos.y + 16.f }, Localization::t("menu_eat_header"), 20, sf::Color(232, 212, 120), true);
    uiButton(window, { pos.x + size.x - 120.f, pos.y + 14.f }, { 100.f, 34.f }, Localization::t("close_button"), [this]() { closeOverlay(); });

    double wheatStock = 0.0;
    for (const auto& g : game_.goodInfos()) {
        if (g.id == "wheat") wheatStock = g.stock;
    }

    uiText(window, { pos.x + 24.f, pos.y + 70.f }, Localization::t("hunger_label") + std::to_string(static_cast<int>(game_.hunger())) + "/100", 15);
    uiText(window, { pos.x + 24.f, pos.y + 100.f },
        Localization::t("eat_have_prefix") + formatNumber(wheatStock) + Localization::t("eat_have_mid")
        + std::to_string(static_cast<int>(game_.hungerRestorePerUnit())) + Localization::t("eat_have_suffix"), 14);

    float btnY = pos.y + 150.f, btnW = 90.f, btnH = 44.f, gap = 12.f;
    uiButton(window, { pos.x + 24.f, btnY }, { btnW, btnH }, Localization::t("qty_1"), [this]() { performEat(1.0); });
    uiButton(window, { pos.x + 24.f + (btnW + gap), btnY }, { btnW, btnH }, Localization::t("qty_10"), [this]() { performEat(10.0); });
    uiButton(window, { pos.x + 24.f + 2.f * (btnW + gap), btnY }, { btnW, btnH }, Localization::t("qty_all"),
        [this, wheatStock]() { performEat(wheatStock); }, wheatStock > 0.0);

    if (!overlayFeedback_.empty()) {
        uiText(window, { pos.x + 24.f, pos.y + size.y - 34.f }, overlayFeedback_, 15, overlayFeedbackColor_);
    }
}

void GameWorld::drawDoctorOverlay(sf::RenderWindow& window) {
    // Taller than before -- this used to just say "You're not sick" and stop,
    // with no explanation of what illness even is, what it costs you while
    // it's active, or that it can kill you. Always shows the same 3-line
    // explainer regardless of sick/not-sick, then the sick-only status below.
    sf::Vector2f pos(340.f, 190.f), size(600.f, 440.f);
    uiPanelBg(window, pos, size);
    uiText(window, { pos.x + 24.f, pos.y + 16.f }, Localization::t("menu_doctor_header"), 20, sf::Color(232, 212, 120), true);
    uiButton(window, { pos.x + size.x - 120.f, pos.y + 14.f }, { 100.f, 34.f }, Localization::t("close_button"), [this]() { closeOverlay(); });

    uiText(window, { pos.x + 24.f, pos.y + 56.f }, Localization::t("doctor_desc_line1"), 13, sf::Color(210, 210, 210));
    uiText(window, { pos.x + 24.f, pos.y + 78.f }, Localization::t("doctor_desc_line2"), 13, sf::Color(210, 210, 210));
    uiText(window, { pos.x + 24.f, pos.y + 100.f }, Localization::t("doctor_desc_line3"), 13, sf::Color(210, 210, 210));

    if (!game_.isSick()) {
        uiText(window, { pos.x + 24.f, pos.y + 150.f }, Localization::t("not_sick"), 15, sf::Color(150, 220, 150));
        return;
    }

    std::ostringstream info;
    info << Localization::t("sick_for_prefix") << std::fixed << std::setprecision(1) << game_.sickDays()
        << Localization::t("sick_for_suffix") << game_.sicknessDeathDays();
    uiText(window, { pos.x + 24.f, pos.y + 150.f }, info.str(), 14, sf::Color(230, 170, 100));
    uiText(window, { pos.x + 24.f, pos.y + 176.f }, Localization::t("sick_penalty_note"), 13, sf::Color(220, 140, 140));
    uiText(window, { pos.x + 24.f, pos.y + 208.f }, Localization::t("treatment_cost_prefix") + formatNumber(game_.doctorTreatmentCost()), 15);

    uiButton(window, { pos.x + 24.f, pos.y + 260.f }, { 200.f, 44.f }, Localization::t("treat_button"), [this]() {
        ActionResult r = game_.tryVisitDoctor();
        if (r.success) setFeedback(Localization::t("all_better"), true);
        else setFeedback(Localization::t(r.messageKey), false);
    });

    if (!overlayFeedback_.empty()) {
        uiText(window, { pos.x + 24.f, pos.y + size.y - 34.f }, overlayFeedback_, 15, overlayFeedbackColor_);
    }
}

void GameWorld::drawFastForwardOverlay(sf::RenderWindow& window) {
    sf::Vector2f pos(300.f, 260.f), size(680.f, 300.f);
    uiPanelBg(window, pos, size);
    uiText(window, { pos.x + 24.f, pos.y + 16.f }, Localization::t("menu_fastforward_header"), 20, sf::Color(232, 212, 120), true);
    uiButton(window, { pos.x + size.x - 120.f, pos.y + 14.f }, { 100.f, 34.f }, Localization::t("close_button"), [this]() { closeOverlay(); });

    struct Preset { const char* key; double minutes; };
    const Preset presets[] = {
        { "ff_15m", 15.0 }, { "ff_1h", 60.0 }, { "ff_4h", 240.0 }, { "ff_1d", 1440.0 }, { "ff_1w", 10080.0 },
    };
    float btnY = pos.y + 100.f, btnW = 110.f, btnH = 44.f, gap = 14.f;
    float x = pos.x + 24.f;
    for (const auto& p : presets) {
        uiButton(window, { x, btnY }, { btnW, btnH }, Localization::t(p.key), [this, minutes = p.minutes, key = std::string(p.key)]() {
            TickOutcome outcome = game_.tryFastForward(minutes);
            if (outcome.died) { handleTickOutcome(outcome); return; }
            setFeedback(Localization::t("simulated_prefix") + Localization::t(key), true);
        });
        x += btnW + gap;
    }

    if (!overlayFeedback_.empty()) {
        uiText(window, { pos.x + 24.f, pos.y + size.y - 34.f }, overlayFeedback_, 15, overlayFeedbackColor_);
    }
}

void GameWorld::drawAchievementsOverlay(sf::RenderWindow& window) {
    sf::Vector2f pos(180.f, 40.f), size(920.f, 740.f);
    uiPanelBg(window, pos, size);
    uiText(window, { pos.x + 24.f, pos.y + 16.f }, Localization::t("menu_achievements_header"), 20, sf::Color(232, 212, 120), true);
    uiButton(window, { pos.x + size.x - 130.f, pos.y + 14.f }, { 110.f, 36.f }, Localization::t("close_button"), [this]() { closeOverlay(); });

    // 20+ achievements now (started at 12, grown several times this
    // session) -- outgrew the panel, so scroll it the same way Market/Staff/
    // Tree/How to Play already do. Grouped into categories (see
    // achievementCategoryFor above) with a header row per group, instead of
    // one flat undifferentiated list.
    std::vector<AchievementInfo> achievements = game_.achievementInfos();
    int unlockedCount = 0;
    for (const auto& a : achievements) if (a.unlocked) unlockedCount++;
    uiText(window, { pos.x + 260.f, pos.y + 20.f },
        Localization::t("achievements_progress_prefix") + std::to_string(unlockedCount) + "/" + std::to_string(achievements.size()),
        15, sf::Color(200, 200, 200));
    constexpr float rowH = 52.f, headerH = 30.f, listTop = 66.f, listBottom = 20.f;

    struct AchRow { bool isHeader; AchievementInfo info; const char* headerLabelKey; sf::Color color; };
    std::vector<AchRow> rows;
    for (const char* catKey : kAchievementCategoryOrder) {
        bool any = false;
        for (const auto& a : achievements) if (achievementCategoryFor(a.id).labelKey == std::string(catKey)) { any = true; break; }
        if (!any) continue;
        AchievementCategory cat{ catKey, sf::Color::White };
        rows.push_back(AchRow{ true, AchievementInfo{}, catKey, sf::Color(200, 200, 200) });
        for (const auto& a : achievements) {
            AchievementCategory info = achievementCategoryFor(a.id);
            if (info.labelKey != std::string(catKey)) continue;
            rows.push_back(AchRow{ false, a, catKey, info.color });
        }
    }

    float visibleH = size.y - listTop - listBottom;
    float contentH = 0.f;
    for (const auto& r : rows) contentH += r.isHeader ? headerH : rowH;
    float maxScroll = std::max(0.f, contentH - visibleH);
    overlayScrollOffset_ = std::clamp(overlayScrollOffset_, 0.f, maxScroll);

    float y = pos.y + listTop - overlayScrollOffset_;
    beginClip(window, sf::FloatRect(sf::Vector2f(pos.x, pos.y + listTop), sf::Vector2f(size.x, visibleH)));
    for (const auto& r : rows) {
        float h = r.isHeader ? headerH : rowH;
        if (y < pos.y + listTop - h || y > pos.y + size.y - listBottom) { y += h; continue; }
        if (r.isHeader) {
            uiText(window, { pos.x + 24.f, y + 6.f }, Localization::t(r.headerLabelKey), 14, sf::Color(232, 212, 120), true);
        } else {
            const AchievementInfo& a = r.info;
            sf::RectangleShape rowBg(sf::Vector2f(size.x - 48.f, rowH - 6.f));
            rowBg.setPosition(sf::Vector2f(pos.x + 24.f, y));
            rowBg.setFillColor(a.unlocked ? sf::Color(48, 70, 50) : sf::Color(50, 52, 62));
            window.draw(rowBg);

            sf::CircleShape dot(5.f);
            dot.setPosition(sf::Vector2f(pos.x + 32.f, y + 10.f));
            dot.setFillColor(r.color);
            window.draw(dot);

            std::string mark = a.unlocked ? "[X] " : "[ ] ";
            uiText(window, { pos.x + 48.f, y + 4.f }, mark + Localization::t("ach_" + a.id + "_name"), 15,
                a.unlocked ? sf::Color(150, 230, 150) : sf::Color::White, true);
            std::string descLine = Localization::t("ach_" + a.id + "_desc");
            if (!a.unlocked) descLine += "  (" + Localization::t("reward_label") + formatNumber(a.reward) + ")";
            uiText(window, { pos.x + 48.f, y + 26.f }, descLine, 13, sf::Color(200, 200, 200));
        }
        y += h;
    }
    endClip(window);
    if (maxScroll > 0.f) {
        uiText(window, { pos.x + size.x - 260.f, pos.y + 16.f }, Localization::t("scroll_hint"), 12, sf::Color(160, 160, 160));
    }
}

void GameWorld::drawPauseOverlay(sf::RenderWindow& window) {
    sf::Vector2f pos(380.f, 190.f), size(520.f, 440.f);
    uiPanelBg(window, pos, size);
    uiText(window, { pos.x + 24.f, pos.y + 20.f }, Localization::t("pause_title"), 22, sf::Color(232, 212, 120), true);
    uiText(window, { pos.x + 24.f, pos.y + 66.f }, Localization::t("pause_save_prompt"), 16, sf::Color::White);

    float btnW = size.x - 48.f, btnH = 48.f, gap = 16.f;
    float y = pos.y + 108.f;
    uiButton(window, { pos.x + 24.f, y }, { btnW, btnH }, Localization::t("pause_save_button"), [this]() {
        game_.saveNow();
        setFeedback(Localization::t("pause_saved_feedback"), true);
    });
    y += btnH + gap;
    uiButton(window, { pos.x + 24.f, y }, { btnW, btnH }, Localization::t("pause_resume_button"), [this]() { closeOverlay(); });
    y += btnH + gap;
    uiButton(window, { pos.x + 24.f, y }, { btnW, btnH }, Localization::t("pause_settings_button"), [this]() { openOverlay(OverlayKind::Settings); });
    y += btnH + gap;
    uiButton(window, { pos.x + 24.f, y }, { btnW, btnH }, Localization::t("pause_quit_button"), [this, &window]() {
        game_.exitAndSave();
        window.close();
    });

    if (!overlayFeedback_.empty()) {
        uiText(window, { pos.x + 24.f, pos.y + size.y - 30.f }, overlayFeedback_, 15, overlayFeedbackColor_);
    }
}

void GameWorld::drawSettingsOverlay(sf::RenderWindow& window) {
    sf::Vector2f pos(180.f, 40.f), size(920.f, 740.f);
    uiPanelBg(window, pos, size);
    uiText(window, { pos.x + 24.f, pos.y + 16.f }, Localization::t("settings_title"), 20, sf::Color(232, 212, 120), true);
    uiButton(window, { pos.x + size.x - 130.f, pos.y + 14.f }, { 110.f, 36.f }, Localization::t("settings_back_button"),
        [this]() { openOverlay(OverlayKind::Pause); });
    uiButton(window, { pos.x + size.x - 270.f, pos.y + 14.f }, { 130.f, 36.f }, Localization::t("settings_reset_button"),
        [this, &window]() {
            settings_ = Settings{}; // fresh defaults, including KeyBindings' own default member initializers
            applyVideoMode(window);
            applySfxVolume();
            applyMusicVolume();
            SettingsManager::save(settings_);
            setFeedback(Localization::t("settings_reset_feedback"), true);
        });

    float labelX = pos.x + 24.f, valueX = pos.x + 320.f, ctrlX = pos.x + 480.f, rowH = 50.f;
    float y = pos.y + 72.f;

    // ---- SFX volume: +/- 10% steps rather than a drag slider, matching the
    // rest of the game's plain-button UI (no drag widgets anywhere else). ----
    uiText(window, { labelX, y + 8.f }, Localization::t("settings_volume_label"), 15, sf::Color(200, 200, 200));
    uiText(window, { valueX, y + 8.f }, std::to_string(static_cast<int>(settings_.sfxVolumePercent)) + "%", 16, sf::Color::White, true);
    uiButton(window, { ctrlX, y }, { 40.f, 36.f }, "-", [this]() {
        settings_.sfxVolumePercent = std::max(0.f, settings_.sfxVolumePercent - 10.f);
        applySfxVolume();
        SettingsManager::save(settings_);
    });
    uiButton(window, { ctrlX + 50.f, y }, { 40.f, 36.f }, "+", [this]() {
        settings_.sfxVolumePercent = std::min(100.f, settings_.sfxVolumePercent + 10.f);
        applySfxVolume();
        SettingsManager::save(settings_);
    });
    y += rowH;

    // ---- Music volume: same +/-10% pattern, independent of SFX volume
    // above -- the looping per-season background melody (see initAudio/
    // playMusicForSeason). ----
    uiText(window, { labelX, y + 8.f }, Localization::t("settings_music_volume_label"), 15, sf::Color(200, 200, 200));
    uiText(window, { valueX, y + 8.f }, std::to_string(static_cast<int>(settings_.musicVolumePercent)) + "%", 16, sf::Color::White, true);
    uiButton(window, { ctrlX, y }, { 40.f, 36.f }, "-", [this]() {
        settings_.musicVolumePercent = std::max(0.f, settings_.musicVolumePercent - 10.f);
        applyMusicVolume();
        SettingsManager::save(settings_);
    });
    uiButton(window, { ctrlX + 50.f, y }, { 40.f, 36.f }, "+", [this]() {
        settings_.musicVolumePercent = std::min(100.f, settings_.musicVolumePercent + 10.f);
        applyMusicVolume();
        SettingsManager::save(settings_);
    });
    y += rowH;

    // ---- Resolution: cycles through kResolutionPresets; disabled while
    // Fullscreen is on since it wouldn't do anything visible. ----
    bool resolutionEnabled = !settings_.fullscreen;
    sf::Vector2u res = kResolutionPresets[std::clamp(settings_.resolutionIndex, 0, kResolutionPresetCount - 1)];
    uiText(window, { labelX, y + 8.f }, Localization::t("settings_resolution_label"), 15, sf::Color(200, 200, 200));
    uiText(window, { valueX, y + 8.f }, std::to_string(res.x) + " x " + std::to_string(res.y), 16,
        resolutionEnabled ? sf::Color::White : sf::Color(120, 120, 120), true);
    uiButton(window, { ctrlX, y }, { 40.f, 36.f }, "<", [this, &window]() {
        settings_.resolutionIndex = (settings_.resolutionIndex - 1 + kResolutionPresetCount) % kResolutionPresetCount;
        applyVideoMode(window);
        SettingsManager::save(settings_);
    }, resolutionEnabled);
    uiButton(window, { ctrlX + 50.f, y }, { 40.f, 36.f }, ">", [this, &window]() {
        settings_.resolutionIndex = (settings_.resolutionIndex + 1) % kResolutionPresetCount;
        applyVideoMode(window);
        SettingsManager::save(settings_);
    }, resolutionEnabled);
    y += rowH;

    // ---- Fullscreen toggle ----
    uiText(window, { labelX, y + 8.f }, Localization::t("settings_fullscreen_label"), 15, sf::Color(200, 200, 200));
    uiButton(window, { ctrlX, y }, { 90.f, 36.f },
        Localization::t(settings_.fullscreen ? "toggle_on" : "toggle_off"), [this, &window]() {
            settings_.fullscreen = !settings_.fullscreen;
            applyVideoMode(window);
            SettingsManager::save(settings_);
        });
    y += rowH + 10.f;

    // ---- Key bindings: click "Rebind", then press any key to bind it to
    // that action (Esc cancels instead -- see the KeyPressed handler in
    // run()). Picking a key already used by another bindable action swaps
    // the two instead of leaving a silent collision. ----
    uiText(window, { labelX, y }, Localization::t("settings_keybinds_label"), 16, sf::Color(232, 212, 120), true);
    y += 34.f;

    struct KeyRow { RebindAction action; const char* labelKey; sf::Keyboard::Key* key; };
    KeyRow rows[] = {
        { RebindAction::MoveUp,       "key_move_up",       &settings_.keys.moveUp },
        { RebindAction::MoveDown,     "key_move_down",     &settings_.keys.moveDown },
        { RebindAction::MoveLeft,     "key_move_left",     &settings_.keys.moveLeft },
        { RebindAction::MoveRight,    "key_move_right",    &settings_.keys.moveRight },
        { RebindAction::Interact,     "key_interact",      &settings_.keys.interact },
        { RebindAction::QuickUpgrade, "key_quick_upgrade", &settings_.keys.quickUpgrade },
        { RebindAction::Minimap,      "key_minimap",       &settings_.keys.minimap },
        { RebindAction::Minigame,     "key_minigame",      &settings_.keys.minigame },
    };
    for (const auto& row : rows) {
        uiText(window, { labelX, y + 8.f }, Localization::t(row.labelKey), 15, sf::Color(200, 200, 200));
        bool waiting = (awaitingRebind_ == row.action);
        if (waiting) {
            uiText(window, { valueX, y + 8.f }, Localization::t("settings_rebind_waiting"), 14, sf::Color(230, 190, 90), true);
        } else {
            uiText(window, { valueX, y + 8.f }, keyName(*row.key), 16, sf::Color::White, true);
            RebindAction action = row.action;
            uiButton(window, { ctrlX, y }, { 140.f, 36.f }, Localization::t("settings_rebind_button"),
                [this, action]() { awaitingRebind_ = action; });
        }
        y += rowH - 4.f;
    }

    if (!overlayFeedback_.empty()) {
        uiText(window, { pos.x + 24.f, pos.y + size.y - 30.f }, overlayFeedback_, 15, overlayFeedbackColor_);
    }
}

void GameWorld::drawWelcomeBackOverlay(sf::RenderWindow& window) {
    const WelcomeBackInfo& info = game_.lastWelcomeBack();

    if (!welcomeBackExpanded_) {
        // Collapsed: just a title banner + hint, same "click to open" idea
        // as the Achievements/How to Play/Recipe Book buttons floating over
        // the world -- the whole panel is one big click target.
        sf::Vector2f pos(340.f, 300.f), size(600.f, 160.f);
        sf::FloatRect bounds = uiPanelBg(window, pos, size);

        if (fontLoaded_) {
            sf::Text title(font_, toSfString(Localization::t("welcomeback_title")), 26);
            title.setStyle(sf::Text::Bold);
            sf::FloatRect tb = title.getLocalBounds();
            title.setPosition(sf::Vector2f(pos.x + size.x / 2.f - tb.size.x / 2.f - tb.position.x, pos.y + 44.f));
            title.setFillColor(sf::Color(232, 212, 120));
            window.draw(title);

            sf::Text hint(font_, toSfString(Localization::t("welcomeback_hint")), 14);
            sf::FloatRect hb = hint.getLocalBounds();
            hint.setPosition(sf::Vector2f(pos.x + size.x / 2.f - hb.size.x / 2.f - hb.position.x, pos.y + 96.f));
            hint.setFillColor(sf::Color(200, 200, 200));
            window.draw(hint);
        }

        overlayClickRegions_.push_back(ClickRegion{ bounds, [this]() { welcomeBackExpanded_ = true; } });
        return;
    }

    // Expanded: how long the player was away, idle earnings, and the same
    // event log printEventLog would otherwise only ever print to a console
    // window the player probably isn't looking at once the SFML window is up.
    sf::Vector2f pos(280.f, 100.f), size(720.f, 600.f);
    uiPanelBg(window, pos, size);
    uiText(window, { pos.x + 24.f, pos.y + 16.f }, Localization::t("welcomeback_title"), 22, sf::Color(232, 212, 120), true);
    uiButton(window, { pos.x + size.x - 130.f, pos.y + 14.f }, { 110.f, 36.f }, Localization::t("close_button"), [this]() { closeOverlay(); });

    uiText(window, { pos.x + 24.f, pos.y + 60.f },
        Localization::t("welcomeback_away_prefix") + info.elapsedFormatted, 16, sf::Color(220, 220, 220));
    uiText(window, { pos.x + 24.f, pos.y + 86.f },
        Localization::t("idle_earnings_prefix") + formatNumber(info.idleEarnings), 16, sf::Color(150, 220, 150), true);

    constexpr float lineH = 22.f, contentTop = 150.f, contentBottom = 20.f;
    float visibleH = size.y - contentTop - contentBottom;

    if (info.eventLog.empty()) {
        uiText(window, { pos.x + 24.f, pos.y + contentTop }, Localization::t("welcomeback_nothing_happened"), 14, sf::Color(180, 180, 180));
    } else {
        float contentH = static_cast<float>(info.eventLog.size()) * lineH;
        float maxScroll = std::max(0.f, contentH - visibleH);
        overlayScrollOffset_ = std::clamp(overlayScrollOffset_, 0.f, maxScroll);

        float y = pos.y + contentTop - overlayScrollOffset_;
        beginClip(window, sf::FloatRect(sf::Vector2f(pos.x, pos.y + contentTop), sf::Vector2f(size.x, visibleH)));
        for (const auto& line : info.eventLog) {
            if (y >= pos.y + contentTop - lineH && y <= pos.y + size.y - contentBottom) {
                uiText(window, { pos.x + 24.f, y }, line, 14, sf::Color(230, 230, 230));
            }
            y += lineH;
        }
        endClip(window);

        if (maxScroll > 0.f) {
            uiText(window, { pos.x + size.x - 210.f, pos.y + contentTop - 22.f }, Localization::t("scroll_hint"), 12, sf::Color(160, 160, 160));
        }
    }
}

void GameWorld::drawAutoSellOverlay(sf::RenderWindow& window) {
    sf::Vector2f pos(100.f, 40.f), size(1080.f, 740.f);
    uiPanelBg(window, pos, size);
    uiText(window, { pos.x + 24.f, pos.y + 16.f }, Localization::t("autosell_title"), 20, sf::Color(232, 212, 120), true);
    uiButton(window, { pos.x + size.x - 130.f, pos.y + 14.f }, { 110.f, 36.f }, Localization::t("close_button"), [this]() { closeOverlay(); });

    // Full explanation up front, not just a label -- this is a mechanic with
    // real rules (threshold direction, no price hints baked in, capacity
    // scaling), not something a bare "Auto-Sell" button name would convey.
    uiText(window, { pos.x + 24.f, pos.y + 50.f }, Localization::t("autosell_desc_line1"), 13, sf::Color(210, 210, 210));
    uiText(window, { pos.x + 24.f, pos.y + 70.f }, Localization::t("autosell_desc_line2"), 13, sf::Color(210, 210, 210));
    uiText(window, { pos.x + 24.f, pos.y + 90.f }, Localization::t("autosell_desc_line3"), 13, sf::Color(210, 210, 210));

    StorefrontAutoSellInfo as = game_.storefrontAutoSellInfo();
    if (!as.built) {
        uiText(window, { pos.x + 24.f, pos.y + 130.f }, Localization::t("autosell_not_built"), 15, sf::Color(220, 140, 140));
        return;
    }

    uiText(window, { pos.x + 24.f, pos.y + 122.f },
        Localization::t("autosell_level_prefix") + std::to_string(as.level) +
        Localization::t("autosell_capacity_prefix") + formatNumber(as.capacityPerDay) + Localization::t("autosell_per_day_suffix"),
        14, sf::Color(200, 220, 255), true);

    // ---- Left: scrollable goods list (name + current price), click to select ----
    constexpr float listW = 560.f, rowH = 32.f;
    float listTop = pos.y + 160.f, listBottom = pos.y + size.y - 24.f;
    auto goods = game_.goodInfos();

    uiText(window, { pos.x + 24.f, listTop - 24.f }, Localization::t("autosell_pick_good_label"), 13, sf::Color(200, 200, 200));

    float contentH = static_cast<float>(goods.size()) * rowH;
    float maxScroll = std::max(0.f, contentH - (listBottom - listTop));
    overlayScrollOffset_ = std::clamp(overlayScrollOffset_, 0.f, maxScroll);

    float rowY = listTop - overlayScrollOffset_;
    beginClip(window, sf::FloatRect(sf::Vector2f(pos.x, listTop), sf::Vector2f(listW, listBottom - listTop)));
    for (size_t i = 0; i < goods.size(); ++i) {
        if (rowY < listTop - rowH || rowY > listBottom) { rowY += rowH; continue; }
        const auto& g = goods[i];
        bool selected = g.id == as.goodId;
        sf::RectangleShape rowBg(sf::Vector2f(listW - 12.f, rowH - 4.f));
        rowBg.setPosition(sf::Vector2f(pos.x + 24.f, rowY));
        rowBg.setFillColor(selected ? sf::Color(90, 76, 130) : sf::Color(50, 52, 62));
        window.draw(rowBg);

        float clickTop = std::max(rowY, listTop), clickBottom = std::min(rowY + (rowH - 4.f), listBottom);
        if (clickBottom > clickTop) {
            std::string goodId = g.id;
            double currentPrice = g.price;
            overlayClickRegions_.push_back(ClickRegion{
                sf::FloatRect(sf::Vector2f(pos.x + 24.f, clickTop), sf::Vector2f(listW - 12.f, clickBottom - clickTop)),
                [this, goodId, currentPrice]() {
                    // Selecting a good defaults its threshold to the price it's
                    // at right now -- a starting point the player then nudges
                    // with the +/-% buttons below, not a suggestion that this
                    // particular number is "good".
                    ActionResult r = game_.trySetStorefrontAutoSell(goodId, currentPrice);
                    if (!r.success) setFeedback(Localization::t(r.messageKey), false);
                } });
        }

        uiText(window, { pos.x + 32.f, rowY + 6.f }, Localization::t(g.id), 14, sf::Color::White);
        uiText(window, { pos.x + 24.f + listW - 120.f, rowY + 6.f }, "$" + formatNumber(g.price), 14, sf::Color(200, 220, 200));
        rowY += rowH;
    }
    endClip(window);
    if (maxScroll > 0.f) {
        uiText(window, { pos.x + 24.f, listTop - 24.f + 200.f }, Localization::t("scroll_hint"), 12, sf::Color(160, 160, 160));
    }

    // ---- Right: current selection + threshold controls ----
    float rx = pos.x + 24.f + listW + 24.f, ry = listTop;
    float rw = pos.x + size.x - 24.f - rx;

    if (as.goodId.empty()) {
        uiText(window, { rx, ry }, Localization::t("autosell_disabled_label"), 16, sf::Color(200, 200, 200), true);
    } else {
        double currentPrice = 0.0;
        for (const auto& g : goods) if (g.id == as.goodId) { currentPrice = g.price; break; }

        uiText(window, { rx, ry }, Localization::t("autosell_selected_prefix") + Localization::t(as.goodId), 16, sf::Color(232, 212, 120), true);
        ry += 30.f;
        uiText(window, { rx, ry }, Localization::t("autosell_current_price_prefix") + "$" + formatNumber(currentPrice), 14, sf::Color(200, 220, 200));
        ry += 24.f;
        bool armed = currentPrice >= as.threshold;
        uiText(window, { rx, ry }, Localization::t("autosell_threshold_prefix") + "$" + formatNumber(as.threshold), 16,
            armed ? sf::Color(150, 220, 150) : sf::Color(230, 170, 100), true);
        ry += 22.f;
        uiText(window, { rx, ry }, Localization::t(armed ? "autosell_status_armed" : "autosell_status_waiting"), 12,
            armed ? sf::Color(150, 220, 150) : sf::Color(180, 180, 180));
        ry += 34.f;

        auto adjustBtn = [&](float x, float w, const std::string& label, double factorOrDelta, bool isPercent) {
            uiButton(window, { x, ry }, { w, 36.f }, label, [this, factorOrDelta, isPercent]() {
                StorefrontAutoSellInfo cur = game_.storefrontAutoSellInfo();
                if (cur.goodId.empty()) return;
                double next = isPercent ? cur.threshold * (1.0 + factorOrDelta) : cur.threshold + factorOrDelta;
                game_.trySetStorefrontAutoSell(cur.goodId, std::max(0.0, next));
            });
        };
        float bw = (rw - 3.f * 8.f) / 4.f;
        adjustBtn(rx, bw, Localization::t("autosell_minus10pct"), -0.10, true);
        adjustBtn(rx + (bw + 8.f), bw, Localization::t("autosell_minus1pct"), -0.01, true);
        adjustBtn(rx + 2.f * (bw + 8.f), bw, Localization::t("autosell_plus1pct"), 0.01, true);
        adjustBtn(rx + 3.f * (bw + 8.f), bw, Localization::t("autosell_plus10pct"), 0.10, true);
        ry += 46.f;

        uiButton(window, { rx, ry }, { rw, 38.f }, Localization::t("autosell_set_to_current_button"),
            [this, currentPrice]() {
                StorefrontAutoSellInfo cur = game_.storefrontAutoSellInfo();
                if (cur.goodId.empty()) return;
                game_.trySetStorefrontAutoSell(cur.goodId, currentPrice);
            });
        ry += 54.f;

        uiButton(window, { rx, ry }, { rw, 40.f }, Localization::t("autosell_disable_button"),
            [this]() { game_.trySetStorefrontAutoSell("", 0.0); });
    }

    if (!overlayFeedback_.empty()) {
        uiText(window, { pos.x + 24.f, pos.y + size.y - 30.f }, overlayFeedback_, 15, overlayFeedbackColor_);
    }
}

void GameWorld::drawHowToPlayOverlay(sf::RenderWindow& window) {
    sf::Vector2f pos(180.f, 40.f), size(920.f, 740.f);
    uiPanelBg(window, pos, size);
    uiText(window, { pos.x + 24.f, pos.y + 16.f }, Localization::t("howtoplay_title"), 20, sf::Color(232, 212, 120), true);
    uiButton(window, { pos.x + size.x - 130.f, pos.y + 14.f }, { 110.f, 36.f }, Localization::t("close_button"), [this]() { closeOverlay(); });

    if (!fontLoaded_) return;

    // This text has grown past what fits in one screen (seasons/crops/
    // fishing added since it was first written) -- scroll it the same way
    // drawTreeOverlay does, line by line, instead of one big sf::Text
    // block that just overflows the panel.
    constexpr float lineH = 24.f;
    constexpr float contentTop = 64.f;
    constexpr float contentBottom = 20.f;
    float visibleH = size.y - contentTop - contentBottom;

    std::string body = applyKeyPlaceholders(Localization::t("howtoplay_body"));
    std::vector<std::string> lines;
    size_t start = 0;
    while (true) {
        size_t nl = body.find('\n', start);
        if (nl == std::string::npos) { lines.push_back(body.substr(start)); break; }
        lines.push_back(body.substr(start, nl - start));
        start = nl + 1;
    }

    float contentH = static_cast<float>(lines.size()) * lineH;
    float maxScroll = std::max(0.f, contentH - visibleH);
    overlayScrollOffset_ = std::clamp(overlayScrollOffset_, 0.f, maxScroll);

    float y = pos.y + contentTop - overlayScrollOffset_;
    beginClip(window, sf::FloatRect(sf::Vector2f(pos.x, pos.y + contentTop), sf::Vector2f(size.x, visibleH)));
    for (const auto& line : lines) {
        if (y >= pos.y + contentTop - lineH && y <= pos.y + size.y - contentBottom) {
            uiText(window, { pos.x + 24.f, y }, line, 15, sf::Color::White);
        }
        y += lineH;
    }
    endClip(window);

    if (maxScroll > 0.f) {
        uiText(window, { pos.x + 24.f, pos.y + size.y - 18.f }, Localization::t("scroll_hint"), 12, sf::Color(160, 160, 160));
    }
}

void GameWorld::drawCropPickerOverlay(sf::RenderWindow& window) {
    sf::Vector2f pos(300.f, 250.f), size(680.f, 340.f);
    uiPanelBg(window, pos, size);
    uiText(window, { pos.x + 24.f, pos.y + 16.f }, Localization::t("crop_picker_header"), 20, sf::Color(232, 212, 120), true);
    uiButton(window, { pos.x + size.x - 120.f, pos.y + 14.f }, { 100.f, 34.f }, Localization::t("close_button"),
        [this]() { openOverlay(OverlayKind::Businesses); });

    // Switching crops already costs money (Game::kCropSwitchCost) -- this
    // panel just never said so anywhere, which read as "replanting is free".
    uiText(window, { pos.x + 24.f, pos.y + 46.f }, Localization::t("crop_switch_cost_prefix") + formatNumber(game_.cropSwitchCost()), 13, sf::Color(200, 200, 200));

    std::string currentCropId;
    for (const auto& b : game_.businessInfos()) {
        if (b.id == "farm") { currentCropId = b.cropId; break; }
    }

    // Compact 3-per-row button grid, same style as drawStaffOverlay's
    // foreman-focus grid -- 7 crops fits comfortably in 3 rows.
    float startX = pos.x + 24.f, x = startX, y = pos.y + 70.f;
    float btnW = 200.f, btnH = 66.f, gapX = 12.f, gapY = 12.f;
    int perRow = 3, placed = 0;
    for (const auto& crop : game_.cropOptions()) {
        bool isCurrent = crop.id == currentCropId;
        std::string label = Localization::t(crop.id) + "\n" + Localization::t("crop_favorite_prefix") +
            Localization::t(seasonKey(crop.favoriteSeason)) + Localization::t("crop_favorite_suffix") +
            (isCurrent ? " *" : "");
        uiButton(window, { x, y }, { btnW, btnH }, label, [this, id = crop.id]() {
            ActionResult r = game_.tryChangeCrop(id);
            if (r.success) {
                if (upgradeSound_) upgradeSound_->play();
                openOverlay(OverlayKind::Businesses);
                setFeedback(Localization::t("crop_changed_prefix") + Localization::t(id) + " (-$" + formatNumber(r.amount) + ")", true);
            } else {
                setFeedback(Localization::t(r.messageKey), false);
            }
        }, !isCurrent);
        placed++;
        if (placed % perRow == 0) { x = startX; y += btnH + gapY; }
        else { x += btnW + gapX; }
    }

    if (!overlayFeedback_.empty()) {
        uiText(window, { pos.x + 24.f, pos.y + size.y - 30.f }, overlayFeedback_, 15, overlayFeedbackColor_);
    }
}

void GameWorld::drawRecipeBookOverlay(sf::RenderWindow& window) {
    sf::Vector2f pos(160.f, 40.f), size(960.f, 740.f);
    uiPanelBg(window, pos, size);
    uiButton(window, { pos.x + size.x - 130.f, pos.y + 14.f }, { 110.f, 36.f }, Localization::t("close_button"), [this]() { closeOverlay(); });

    // Keep the vector alive for the whole function -- businessInfos()
    // returns by value, same reasoning as drawBusinessesOverlay's `infos`.
    const std::vector<BusinessInfo> infos = game_.businessInfos();

    if (recipeBookSelectedGoodId_.empty()) {
        uiText(window, { pos.x + 24.f, pos.y + 16.f }, Localization::t("recipebook_header"), 18, sf::Color(232, 212, 120), true);

        // Every processed good -- the output of a business that itself has
        // at least one input. Raw materials (wheat/wood/stone/ore/...) have
        // no recipe and aren't listed here at all.
        struct Entry { std::string goodId; sf::Color color; };
        std::vector<Entry> entries;
        for (const auto& info : infos) {
            if (info.inputGoodId.empty() || info.outputGoodId.empty()) continue;
            entries.push_back({ info.outputGoodId, accentFor(info.id).color });
        }

        constexpr int perRow = 3;
        constexpr float cellW = 300.f, cellH = 116.f, iconSize = 56.f;
        float startX = pos.x + 30.f, startY = pos.y + 70.f;
        float gridBottom = pos.y + size.y - 24.f;
        int totalRows = static_cast<int>((entries.size() + static_cast<size_t>(perRow) - 1) / static_cast<size_t>(perRow));
        float contentH = static_cast<float>(totalRows) * cellH;
        float maxScroll = std::max(0.f, contentH - (gridBottom - startY));
        overlayScrollOffset_ = std::clamp(overlayScrollOffset_, 0.f, maxScroll);

        beginClip(window, sf::FloatRect(sf::Vector2f(pos.x, startY), sf::Vector2f(size.x, gridBottom - startY)));
        for (size_t i = 0; i < entries.size(); ++i) {
            int row = static_cast<int>(i / static_cast<size_t>(perRow));
            int col = static_cast<int>(i % static_cast<size_t>(perRow));
            float cellX = startX + static_cast<float>(col) * cellW;
            float cellY = startY + static_cast<float>(row) * cellH - overlayScrollOffset_;
            if (cellY < startY - cellH || cellY > gridBottom) continue;

            drawGoodIcon(window, entries[i].goodId, sf::Vector2f(cellX, cellY), iconSize, entries[i].color);

            if (fontLoaded_) {
                sf::Text label(font_, toSfString(Localization::t(entries[i].goodId)), 13);
                sf::FloatRect b = label.getLocalBounds();
                label.setPosition(sf::Vector2f(cellX + iconSize / 2.f - b.size.x / 2.f - b.position.x, cellY + iconSize + 6.f));
                label.setFillColor(sf::Color::White);
                window.draw(label);
            }

            // Clamp the click region to the visible grid band -- a cell
            // scrolled half off the top/bottom edge otherwise still catches
            // clicks over the sliver that's now clipped away (see beginClip).
            float top = std::max(cellY, startY), bottom = std::min(cellY + iconSize + 26.f, gridBottom);
            if (bottom > top) {
                std::string goodId = entries[i].goodId;
                overlayClickRegions_.push_back(ClickRegion{
                    sf::FloatRect(sf::Vector2f(cellX, top), sf::Vector2f(iconSize, bottom - top)),
                    [this, goodId]() { recipeBookSelectedGoodId_ = goodId; overlayScrollOffset_ = 0.f; } });
            }
        }
        endClip(window);
        if (maxScroll > 0.f) {
            uiText(window, { pos.x + size.x - 210.f, pos.y + 46.f }, Localization::t("scroll_hint"), 12, sf::Color(160, 160, 160));
        }
    } else {
        // Detail view: find the one business that makes this good, and show
        // its recipe (primary input + any BusinessType::extraInputs) with
        // live stock -- same "have vs need" format the Businesses overlay's
        // production section uses.
        const BusinessInfo* info = nullptr;
        for (const auto& i : infos) {
            if (i.outputGoodId == recipeBookSelectedGoodId_) { info = &i; break; }
        }
        uiButton(window, { pos.x + 24.f, pos.y + 14.f }, { 100.f, 36.f }, Localization::t("back_button"),
            [this]() { recipeBookSelectedGoodId_.clear(); });
        uiText(window, { pos.x + 140.f, pos.y + 20.f }, Localization::t(recipeBookSelectedGoodId_), 22, sf::Color(232, 212, 120), true);

        if (info) {
            float y = pos.y + 90.f;
            uiText(window, { pos.x + 24.f, y }, Localization::t("recipebook_made_at_prefix") + Localization::t(info->id), 15, sf::Color(200, 200, 200));
            y += 40.f;
            uiText(window, { pos.x + 24.f, y }, Localization::t("col_needs"), 15, sf::Color(200, 200, 200), true);
            y += 30.f;
            for (const auto& in : info->inputs) {
                bool producing = in.have > 0.0;
                std::ostringstream line;
                line << Localization::t(in.goodId) << " -" << std::fixed << std::setprecision(2) << in.required
                    << " (" << Localization::t("input_have_label") << formatNumber(in.have) << ")";
                uiText(window, { pos.x + 40.f, y }, line.str(), 16, producing ? sf::Color(160, 220, 160) : sf::Color(220, 140, 140));
                y += 30.f;
            }
        }
    }

    if (!overlayFeedback_.empty()) {
        uiText(window, { pos.x + 24.f, pos.y + size.y - 30.f }, overlayFeedback_, 15, overlayFeedbackColor_);
    }
}

void GameWorld::drawTimingMinigameOverlay(sf::RenderWindow& window) {
    MinigameFlavor flavor = minigameFlavorFor(minigameBusinessId_);
    sf::Vector2f pos(440.f, 280.f), size(400.f, 240.f);
    uiPanelBg(window, pos, size);
    uiText(window, { pos.x + 24.f, pos.y + 14.f }, Localization::t(flavor.titleKey), 18, sf::Color(232, 212, 120), true);
    uiButton(window, { pos.x + size.x - 110.f, pos.y + 12.f }, { 90.f, 32.f }, Localization::t("close_button"), [this]() { closeOverlay(); });

    uiText(window, { pos.x + 24.f, pos.y + 52.f }, Localization::t("fishing_hint"), 12, sf::Color(200, 200, 200));

    float barX = pos.x + 24.f, barY = pos.y + 96.f, barW = size.x - 48.f, barH = 26.f;
    sf::RectangleShape barBg(sf::Vector2f(barW, barH));
    barBg.setPosition(sf::Vector2f(barX, barY));
    barBg.setFillColor(sf::Color(40, 40, 50));
    barBg.setOutlineThickness(2.f);
    barBg.setOutlineColor(sf::Color(25, 20, 15));
    window.draw(barBg);

    float targetX = barX + (minigameTargetCenter_ - minigameTargetHalfWidth_) * barW;
    float targetW = minigameTargetHalfWidth_ * 2.f * barW;
    sf::RectangleShape targetZone(sf::Vector2f(targetW, barH));
    targetZone.setPosition(sf::Vector2f(targetX, barY));
    targetZone.setFillColor(sf::Color(90, 200, 110, 190));
    window.draw(targetZone);

    float indicatorPos = 0.5f + 0.5f * std::sin(minigameIndicatorPhase_ * kMinigameIndicatorSpeed);
    float indicatorX = barX + indicatorPos * barW;
    sf::RectangleShape indicator(sf::Vector2f(4.f, barH + 10.f));
    indicator.setPosition(sf::Vector2f(indicatorX - 2.f, barY - 5.f));
    indicator.setFillColor(flavor.accent);
    window.draw(indicator);

    uiButton(window, { pos.x + 24.f, barY + 50.f }, { size.x - 48.f, 44.f }, Localization::t(flavor.buttonKey),
        [this]() { resolveTimingMinigame(); });

    if (!overlayFeedback_.empty()) {
        uiText(window, { pos.x + 24.f, pos.y + size.y - 28.f }, overlayFeedback_, 13, overlayFeedbackColor_);
    }
}

void GameWorld::tryStartTimingMinigame(const std::string& businessId) {
    bool built = false;
    for (const auto& info : game_.businessInfos()) {
        if (info.id == businessId) { built = info.level > 0; break; }
    }
    float& cooldown = (businessId == "fishing") ? fishingCooldown_ : miningCooldown_;
    if (!built) {
        setFeedback(Localization::t("minigame_needs_building"), false);
    } else if (cooldown > 0.f) {
        std::ostringstream oss;
        oss << Localization::t("fishing_cooldown_prefix") << static_cast<int>(std::ceil(cooldown))
            << Localization::t("fishing_cooldown_suffix");
        setFeedback(oss.str(), false);
    } else {
        minigameBusinessId_ = businessId;
        minigameIndicatorPhase_ = 0.f;
        minigameTargetCenter_ = randRange(0.2f, 0.8f);
        openOverlay(OverlayKind::TimingMinigame);
    }
}

void GameWorld::resolveTimingMinigame() {
    float indicatorPos = 0.5f + 0.5f * std::sin(minigameIndicatorPhase_ * kMinigameIndicatorSpeed);
    bool hit = std::abs(indicatorPos - minigameTargetCenter_) <= minigameTargetHalfWidth_;
    ActionResult r = game_.tryMinigameBonus(minigameBusinessId_, hit);
    closeOverlay();
    float& cooldown = (minigameBusinessId_ == "fishing") ? fishingCooldown_ : miningCooldown_;
    cooldown = kMinigameCooldownSeconds;
    if (r.success) {
        if (hit && upgradeSound_) upgradeSound_->play();
        std::string msg = Localization::t(r.rare ? "minigame_rare_prefix" : (hit ? "fishing_hit_prefix" : "fishing_miss_prefix"))
            + formatNumber(r.amount) + " " + Localization::t(r.goodId) + Localization::t("minigame_result_suffix");
        setFeedback(msg, hit);
    } else {
        setFeedback(Localization::t(r.messageKey), false);
    }
}

void GameWorld::tryStartChopping() {
    bool built = false;
    for (const auto& info : game_.businessInfos()) {
        if (info.id == "lumber") { built = info.level > 0; break; }
    }
    if (!built) {
        setFeedback(Localization::t("minigame_needs_building"), false);
    } else if (lumberCooldown_ > 0.f) {
        std::ostringstream oss;
        oss << Localization::t("fishing_cooldown_prefix") << static_cast<int>(std::ceil(lumberCooldown_))
            << Localization::t("fishing_cooldown_suffix");
        setFeedback(oss.str(), false);
    } else {
        choppingClicks_ = 0;
        choppingTimeLeft_ = kChoppingTimeWindow;
        openOverlay(OverlayKind::Chopping);
    }
}

void GameWorld::updateChopping(float dt) {
    choppingTimeLeft_ -= dt;
    if (choppingTimeLeft_ <= 0.f) resolveChopping();
}

void GameWorld::resolveChopping() {
    bool hit = choppingClicks_ >= kChoppingClicksTarget;
    ActionResult r = game_.tryMinigameBonus("lumber", hit);
    closeOverlay();
    lumberCooldown_ = kLumberCooldownSeconds;
    if (r.success) {
        if (hit && upgradeSound_) upgradeSound_->play();
        std::string msg = Localization::t(hit ? "fishing_hit_prefix" : "fishing_miss_prefix")
            + formatNumber(r.amount) + " " + Localization::t(r.goodId) + Localization::t("minigame_result_suffix");
        setFeedback(msg, hit);
    } else {
        setFeedback(Localization::t(r.messageKey), false);
    }
}

void GameWorld::drawChoppingOverlay(sf::RenderWindow& window) {
    sf::Vector2f pos(440.f, 280.f), size(400.f, 240.f);
    uiPanelBg(window, pos, size);
    uiText(window, { pos.x + 24.f, pos.y + 14.f }, Localization::t("chopping_title"), 18, sf::Color(232, 212, 120), true);
    uiButton(window, { pos.x + size.x - 110.f, pos.y + 12.f }, { 90.f, 32.f }, Localization::t("close_button"), [this]() { closeOverlay(); });

    uiText(window, { pos.x + 24.f, pos.y + 52.f }, Localization::t("chopping_hint"), 12, sf::Color(200, 200, 200));

    std::ostringstream progressOss;
    progressOss << choppingClicks_ << " / " << kChoppingClicksTarget;
    uiText(window, { pos.x + 24.f, pos.y + 86.f }, Localization::t("chopping_progress_prefix") + progressOss.str(), 16, sf::Color::White);

    // Progress bar (clicks) and a shrinking time bar underneath it.
    float barX = pos.x + 24.f, barW = size.x - 48.f;
    float clickBarY = pos.y + 116.f, barH = 22.f;
    sf::RectangleShape clickBg(sf::Vector2f(barW, barH));
    clickBg.setPosition(sf::Vector2f(barX, clickBarY));
    clickBg.setFillColor(sf::Color(40, 40, 50));
    clickBg.setOutlineThickness(2.f);
    clickBg.setOutlineColor(sf::Color(25, 20, 15));
    window.draw(clickBg);
    float clickFrac = std::clamp(static_cast<float>(choppingClicks_) / static_cast<float>(kChoppingClicksTarget), 0.f, 1.f);
    sf::RectangleShape clickFill(sf::Vector2f(barW * clickFrac, barH));
    clickFill.setPosition(sf::Vector2f(barX, clickBarY));
    clickFill.setFillColor(sf::Color(160, 120, 70));
    window.draw(clickFill);

    float timeBarY = clickBarY + barH + 10.f;
    sf::RectangleShape timeBg(sf::Vector2f(barW, 10.f));
    timeBg.setPosition(sf::Vector2f(barX, timeBarY));
    timeBg.setFillColor(sf::Color(40, 40, 50));
    window.draw(timeBg);
    float timeFrac = std::clamp(choppingTimeLeft_ / kChoppingTimeWindow, 0.f, 1.f);
    sf::RectangleShape timeFill(sf::Vector2f(barW * timeFrac, 10.f));
    timeFill.setPosition(sf::Vector2f(barX, timeBarY));
    timeFill.setFillColor(sf::Color(200, 90, 80));
    window.draw(timeFill);

    uiButton(window, { pos.x + 24.f, timeBarY + 30.f }, { size.x - 48.f, 44.f }, Localization::t("chopping_button"),
        [this]() { choppingClicks_++; });

    if (!overlayFeedback_.empty()) {
        uiText(window, { pos.x + 24.f, pos.y + size.y - 28.f }, overlayFeedback_, 13, overlayFeedbackColor_);
    }
}

void GameWorld::tryStartBrewing(const std::string& businessId) {
    bool built = false;
    for (const auto& info : game_.businessInfos()) {
        if (info.id == businessId) { built = info.level > 0; break; }
    }
    float& cooldown = (businessId == "alchemist") ? alchemistCooldown_ : wineryCooldown_;
    if (!built) {
        setFeedback(Localization::t("minigame_needs_building"), false);
    } else if (cooldown > 0.f) {
        std::ostringstream oss;
        oss << Localization::t("fishing_cooldown_prefix") << static_cast<int>(std::ceil(cooldown))
            << Localization::t("fishing_cooldown_suffix");
        setFeedback(oss.str(), false);
    } else {
        minigameBusinessId_ = businessId;
        brewSequence_.clear();
        for (int i = 0; i < kBrewSequenceLength; ++i) brewSequence_.push_back(std::rand() % 4);
        brewStep_ = 0;
        brewShowTimer_ = kBrewShowSeconds;
        brewShowingSequence_ = true;
        openOverlay(OverlayKind::Brewing);
    }
}

void GameWorld::updateBrewing(float dt) {
    if (!brewShowingSequence_) return;
    brewShowTimer_ -= dt;
    if (brewShowTimer_ <= 0.f) {
        brewShowingSequence_ = false;
        brewStep_ = 0;
    }
}

void GameWorld::handleBrewClick(int colorIndex) {
    if (brewShowingSequence_ || brewStep_ >= brewSequence_.size()) return;
    if (colorIndex != brewSequence_[brewStep_]) {
        resolveBrewing(false);
        return;
    }
    brewStep_++;
    if (brewStep_ >= brewSequence_.size()) resolveBrewing(true);
}

void GameWorld::resolveBrewing(bool hit) {
    ActionResult r = game_.tryMinigameBonus(minigameBusinessId_, hit);
    closeOverlay();
    float& cooldown = (minigameBusinessId_ == "alchemist") ? alchemistCooldown_ : wineryCooldown_;
    cooldown = kMinigameCooldownSeconds;
    if (r.success) {
        if (hit && upgradeSound_) upgradeSound_->play();
        std::string msg = Localization::t(hit ? "fishing_hit_prefix" : "fishing_miss_prefix")
            + formatNumber(r.amount) + " " + Localization::t(r.goodId) + Localization::t("minigame_result_suffix");
        setFeedback(msg, hit);
    } else {
        setFeedback(Localization::t(r.messageKey), false);
    }
}

void GameWorld::drawBrewingOverlay(sf::RenderWindow& window) {
    static const sf::Color kBrewColors[4] = {
        sf::Color(210, 90, 90), sf::Color(90, 150, 210), sf::Color(110, 200, 120), sf::Color(220, 200, 90)
    };

    sf::Vector2f pos(440.f, 260.f), size(400.f, 280.f);
    uiPanelBg(window, pos, size);
    uiText(window, { pos.x + 24.f, pos.y + 14.f }, Localization::t("brewing_title"), 18, sf::Color(232, 212, 120), true);
    uiButton(window, { pos.x + size.x - 110.f, pos.y + 12.f }, { 90.f, 32.f }, Localization::t("close_button"), [this]() { closeOverlay(); });

    uiText(window, { pos.x + 24.f, pos.y + 52.f },
        Localization::t(brewShowingSequence_ ? "brewing_hint_memorize" : "brewing_hint_repeat"), 12, sf::Color(200, 200, 200));

    // The sequence row: full-color while memorizing, and while repeating it
    // shows a filled-in swatch for each already-correct step (so the player
    // can see their own progress) and a dim placeholder for the rest.
    float swatchSize = 44.f, gap = 12.f;
    float rowW = static_cast<float>(kBrewSequenceLength) * swatchSize + static_cast<float>(kBrewSequenceLength - 1) * gap;
    float rowX = pos.x + (size.x - rowW) / 2.f, rowY = pos.y + 90.f;
    for (int i = 0; i < kBrewSequenceLength; ++i) {
        sf::RectangleShape swatch(sf::Vector2f(swatchSize, swatchSize));
        swatch.setPosition(sf::Vector2f(rowX + static_cast<float>(i) * (swatchSize + gap), rowY));
        bool reveal = brewShowingSequence_ || static_cast<size_t>(i) < brewStep_;
        swatch.setFillColor(reveal ? kBrewColors[static_cast<size_t>(brewSequence_[static_cast<size_t>(i)])] : sf::Color(50, 50, 58));
        swatch.setOutlineThickness(2.f);
        swatch.setOutlineColor(sf::Color(25, 20, 15));
        window.draw(swatch);
    }

    if (!brewShowingSequence_) {
        // Four clickable color buttons to reproduce the sequence with.
        float btnSize = 70.f, btnGap = 14.f;
        float btnRowW = 4.f * btnSize + 3.f * btnGap;
        float btnX = pos.x + (size.x - btnRowW) / 2.f, btnY = pos.y + 170.f;
        for (int i = 0; i < 4; ++i) {
            sf::Vector2f btnPos(btnX + static_cast<float>(i) * (btnSize + btnGap), btnY);
            sf::RectangleShape btn(sf::Vector2f(btnSize, btnSize));
            btn.setPosition(btnPos);
            btn.setFillColor(kBrewColors[static_cast<size_t>(i)]);
            btn.setOutlineThickness(2.f);
            btn.setOutlineColor(sf::Color(232, 212, 120));
            window.draw(btn);
            overlayClickRegions_.push_back(ClickRegion{ sf::FloatRect(btnPos, sf::Vector2f(btnSize, btnSize)),
                [this, i]() { handleBrewClick(i); } });
        }
    }

    if (!overlayFeedback_.empty()) {
        uiText(window, { pos.x + 24.f, pos.y + size.y - 28.f }, overlayFeedback_, 13, overlayFeedbackColor_);
    }
}

void GameWorld::drawDeathNoticeOverlay(sf::RenderWindow& window) {
    sf::Vector2f pos(320.f, 210.f), size(640.f, 360.f);
    uiPanelBg(window, pos, size);
    uiText(window, { pos.x + 24.f, pos.y + 20.f }, Localization::t("death_notice_title"), 22, sf::Color(230, 120, 110), true);
    uiText(window, { pos.x + 24.f, pos.y + 70.f }, deathNoticeMessage_, 15);

    // handleDeath() already reset Game's own peakMoney() to the new
    // generation's starting cash by the time this draws, so the life that
    // just ended is read back from the history entry it recorded instead
    // (its `generation` is deathNoticeGeneration_ - 1, since that counter
    // was already incremented too).
    double peak = 0.0;
    for (const auto& rec : game_.generationHistory()) {
        if (rec.generation == deathNoticeGeneration_ - 1) { peak = rec.peakMoney; break; }
    }
    uiText(window, { pos.x + 24.f, pos.y + 106.f }, Localization::t("death_peak_prefix") + formatNumber(peak), 14);
    uiText(window, { pos.x + 24.f, pos.y + 132.f },
        Localization::t("achievements_button") + ": " + std::to_string(game_.unlockedAchievementCount()), 14);
    uiText(window, { pos.x + 24.f, pos.y + 166.f },
        Localization::t("generation_prefix") + std::to_string(deathNoticeGeneration_) + Localization::t("generation_suffix"), 14);

    uiButton(window, { pos.x + size.x / 2.f - 100.f, pos.y + size.y - 70.f }, { 200.f, 44.f },
        Localization::t("death_notice_continue"), [this]() { closeOverlay(); });
}

void GameWorld::drawLegacyOverlay(sf::RenderWindow& window) {
    sf::Vector2f pos(260.f, 90.f), size(760.f, 680.f);
    uiPanelBg(window, pos, size);
    uiText(window, { pos.x + 24.f, pos.y + 16.f }, Localization::t("menu_legacy_header"), 20, sf::Color(232, 212, 120), true);
    uiButton(window, { pos.x + size.x - 120.f, pos.y + 14.f }, { 100.f, 34.f }, Localization::t("close_button"), [this]() { closeOverlay(); });

    uiText(window, { pos.x + 24.f, pos.y + 70.f }, Localization::t("legacy_points_label") + std::to_string(game_.legacyPoints()), 17, sf::Color(232, 212, 120), true);

    float y = pos.y + 116.f;
    uiText(window, { pos.x + 24.f, y },
        Localization::t("legacy_cash_option_prefix") + formatNumber(static_cast<double>(game_.legacyCashLevel()) * game_.legacyCashBonusPerLevel()), 15);
    y += 26.f;
    uiButton(window, { pos.x + 24.f, y }, { 320.f, 44.f },
        Localization::t("upgrade_button") + " (" + std::to_string(game_.legacyCashUpgradeCost()) + Localization::t("legacy_points_suffix"),
        [this]() {
            ActionResult r = game_.tryBuyLegacyCashLevel();
            if (r.success) setFeedback(Localization::t("legacy_bought_prefix") + std::to_string(r.count), true);
            else setFeedback(Localization::t(r.messageKey), false);
        });
    y += 74.f;

    std::ostringstream prodOss;
    prodOss << std::fixed << std::setprecision(0) << (static_cast<double>(game_.legacyProdLevel()) * game_.legacyProdBonusPercentPerLevel());
    uiText(window, { pos.x + 24.f, y }, Localization::t("legacy_prod_option_prefix") + prodOss.str() + "%", 15);
    y += 26.f;
    uiButton(window, { pos.x + 24.f, y }, { 320.f, 44.f },
        Localization::t("upgrade_button") + " (" + std::to_string(game_.legacyProdUpgradeCost()) + Localization::t("legacy_points_suffix"),
        [this]() {
            ActionResult r = game_.tryBuyLegacyProdLevel();
            if (r.success) setFeedback(Localization::t("legacy_bought_prefix") + std::to_string(r.count), true);
            else setFeedback(Localization::t(r.messageKey), false);
        });
    y += 74.f;

    bool seasonMaxed = game_.legacySeasonLevel() >= Game::kLegacySeasonMaxLevel;
    std::ostringstream seasonOss;
    seasonOss << std::fixed << std::setprecision(0) << (game_.legacySeasonNegation() * 100.0);
    uiText(window, { pos.x + 24.f, y }, Localization::t("legacy_season_option_prefix") + seasonOss.str() + "%", 15);
    y += 26.f;
    uiButton(window, { pos.x + 24.f, y }, { 320.f, 44.f },
        seasonMaxed ? Localization::t("legacy_season_maxed_suffix")
                    : Localization::t("upgrade_button") + " (" + std::to_string(game_.legacySeasonUpgradeCost()) + Localization::t("legacy_points_suffix"),
        [this]() {
            ActionResult r = game_.tryBuyLegacySeasonLevel();
            if (r.success) setFeedback(Localization::t("legacy_bought_prefix") + std::to_string(r.count), true);
            else setFeedback(Localization::t(r.messageKey), false);
        }, !seasonMaxed);
    y += 74.f;

    uiText(window, { pos.x + 24.f, y }, Localization::t("history_header"), 16, sf::Color(232, 212, 120), true);
    y += 28.f;
    auto history = game_.generationHistory();
    if (history.empty()) {
        uiText(window, { pos.x + 24.f, y }, Localization::t("contract_none_active"), 14, sf::Color(180, 180, 180));
    } else {
        // A horizontal dot-per-generation timeline above the existing detail
        // lines -- brighter/bigger dot means a higher peak net worth that
        // life, relative to the others currently shown (capped at
        // kMaxHistoryEntries, so this never needs its own scrolling).
        double maxPeak = 0.0;
        for (const auto& rec : history) maxPeak = std::max(maxPeak, rec.peakMoney);
        if (maxPeak < 1.0) maxPeak = 1.0;

        float lineY = y + 10.f;
        float startX = pos.x + 24.f, endX = pos.x + size.x - 24.f;
        sf::Vertex axisLine[] = {
            sf::Vertex{ sf::Vector2f(startX, lineY), sf::Color(120, 120, 130) },
            sf::Vertex{ sf::Vector2f(endX, lineY), sf::Color(120, 120, 130) },
        };
        window.draw(axisLine, 2, sf::PrimitiveType::Lines);

        float stepX = history.size() > 1 ? (endX - startX) / static_cast<float>(history.size() - 1) : 0.f;
        for (size_t i = 0; i < history.size(); ++i) {
            float dx = history.size() > 1 ? startX + stepX * static_cast<float>(i) : (startX + endX) / 2.f;
            float frac = static_cast<float>(history[i].peakMoney / maxPeak);
            sf::Color dotColor(
                static_cast<std::uint8_t>(120.f + 110.f * frac),
                static_cast<std::uint8_t>(130.f + 70.f * frac),
                80);
            float radius = 5.f + 4.f * frac;
            sf::CircleShape dot(radius);
            dot.setPosition(sf::Vector2f(dx - radius, lineY - radius));
            dot.setFillColor(dotColor);
            dot.setOutlineThickness(1.5f);
            dot.setOutlineColor(sf::Color(25, 20, 15));
            window.draw(dot);

            std::string genLabel = std::to_string(history[i].generation);
            uiText(window, { dx - 4.f * static_cast<float>(genLabel.size()), lineY + 10.f }, genLabel, 11, sf::Color(200, 200, 200));
        }
        y += 40.f;

        for (const auto& rec : history) {
            std::ostringstream oss;
            oss << Localization::t("history_entry_prefix") << rec.generation
                << Localization::t("history_entry_mid1") << formatNumber(rec.peakMoney)
                << Localization::t("history_entry_mid2") << std::fixed << std::setprecision(1) << rec.ageYears
                << Localization::t("history_entry_mid3") << rec.cause;
            uiText(window, { pos.x + 24.f, y }, oss.str(), 13, sf::Color(210, 210, 210));
            y += 22.f;
        }
    }

    if (!overlayFeedback_.empty()) {
        uiText(window, { pos.x + 24.f, pos.y + size.y - 30.f }, overlayFeedback_, 15, overlayFeedbackColor_);
    }
}

void GameWorld::drawDialogueOverlay(sf::RenderWindow& window) {
    // Classic bottom-of-screen dialogue box, sized to sit just above the HUD
    // bar. NPC lines are short enough (checked against the panel width) that
    // this doesn't need word-wrapping.
    sf::Vector2f pos(140.f, 540.f), size(1000.f, 200.f);
    uiPanelBg(window, pos, size);
    uiButton(window, { pos.x + size.x - 120.f, pos.y + 14.f }, { 100.f, 34.f }, Localization::t("close_button"), [this]() { closeOverlay(); });

    uiText(window, { pos.x + 24.f, pos.y + 18.f }, dialogueSpeaker_, 19, sf::Color(232, 212, 120), true);
    uiText(window, { pos.x + 24.f, pos.y + 64.f }, dialogueText_, 16, sf::Color::White);

    if (dialogueNpc_ && dialogueNpc_->hasQuest) {
        double have = 0.0;
        for (const auto& g : game_.goodInfos()) {
            if (g.id == dialogueNpc_->questGoodId) { have = g.stock; break; }
        }
        bool canTurnIn = have >= dialogueNpc_->questQty;
        uiButton(window, { pos.x + 24.f, pos.y + 120.f }, { 200.f, 44.f }, Localization::t("quest_turn_in_button"),
            [this]() {
                Npc* npc = dialogueNpc_;
                if (!npc) return;
                ActionResult r = game_.tryFulfillQuest(npc->questGoodId, npc->questQty, npc->questReward);
                if (r.success) {
                    npc->hasQuest = false;
                    npc->questRollTimer = randRange(30.f, 60.f);
                    dialogueText_ = Localization::t("quest_thanks");
                    setFeedback(Localization::t("quest_completed_prefix") + formatNumber(r.amount), true);
                } else {
                    setFeedback(Localization::t(r.messageKey), false);
                }
            }, canTurnIn);
        if (!canTurnIn) {
            std::ostringstream needOss;
            needOss << Localization::t("quest_need_prefix") << formatNumber(dialogueNpc_->questQty - have) << " " << Localization::t("quest_need_suffix");
            uiText(window, { pos.x + 240.f, pos.y + 134.f }, needOss.str(), 13, sf::Color(200, 150, 150));
        }
    }

    if (dialogueNpc_ && dialogueNpc_->hasDeal) {
        bool canAfford = game_.money() >= dialogueNpc_->dealTotalPrice;
        uiButton(window, { pos.x + 24.f, pos.y + 120.f }, { 200.f, 44.f }, Localization::t("deal_buy_button"),
            [this]() {
                Npc* npc = dialogueNpc_;
                if (!npc) return;
                ActionResult r = game_.tryBuyDeal(npc->dealGoodId, npc->dealQty, npc->dealTotalPrice);
                if (r.success) {
                    npc->hasDeal = false;
                    npc->dealRollTimer = randRange(40.f, 80.f);
                    dialogueText_ = Localization::t("deal_thanks");
                    setFeedback(Localization::t("deal_bought_prefix") + formatNumber(r.amount), true);
                } else {
                    setFeedback(Localization::t(r.messageKey), false);
                }
            }, canAfford);
        if (!canAfford) {
            uiText(window, { pos.x + 240.f, pos.y + 134.f }, Localization::t("not_enough_cash_prefix") + formatNumber(dialogueNpc_->dealTotalPrice), 13, sf::Color(200, 150, 150));
        }
    }

    if (!overlayFeedback_.empty()) {
        uiText(window, { pos.x + 24.f, pos.y + 170.f }, overlayFeedback_, 14, overlayFeedbackColor_);
    }

    uiText(window, { pos.x + 24.f, pos.y + size.y - 30.f }, Localization::t("dialogue_continue_hint"), 13, sf::Color(180, 180, 180));
}

void GameWorld::drawBankOverlay(sf::RenderWindow& window) {
    sf::Vector2f pos(360.f, 220.f), size(560.f, 360.f);
    uiPanelBg(window, pos, size);
    uiText(window, { pos.x + 24.f, pos.y + 16.f }, Localization::t("menu_bank_header"), 20, sf::Color(232, 212, 120), true);
    uiButton(window, { pos.x + size.x - 120.f, pos.y + 14.f }, { 100.f, 34.f }, Localization::t("close_button"), [this]() { closeOverlay(); });

    uiText(window, { pos.x + 24.f, pos.y + 70.f }, Localization::t("bank_cash_label") + formatNumber(game_.money()), 15);
    std::ostringstream feeOss;
    feeOss << Localization::t("bank_balance_label") << formatNumber(game_.bankBalance())
        << Localization::t("bank_fee_note") << std::fixed << std::setprecision(0) << (game_.bankWithdrawFeeRate() * 100.0) << "%)";
    uiText(window, { pos.x + 24.f, pos.y + 100.f }, feeOss.str(), 15);

    auto doDeposit = [this](double amt) {
        ActionResult r = game_.tryBankDeposit(amt);
        if (r.success) setFeedback(Localization::t("bank_deposited_prefix") + formatNumber(r.amount), true);
        else setFeedback(Localization::t(r.messageKey), false);
    };
    auto doWithdraw = [this](double amt) {
        ActionResult r = game_.tryBankWithdraw(amt);
        if (r.success) setFeedback(Localization::t("bank_withdrew_prefix") + formatNumber(r.amount), true);
        else setFeedback(Localization::t(r.messageKey), false);
    };

    float btnW = 110.f, btnH = 42.f, gap = 10.f;
    float depositY = pos.y + 160.f;
    uiText(window, { pos.x + 24.f, depositY - 20.f }, Localization::t("deposit_button"), 13, sf::Color(200, 200, 200));
    uiButton(window, { pos.x + 24.f, depositY }, { btnW, btnH }, "$50", [doDeposit]() { doDeposit(50.0); });
    uiButton(window, { pos.x + 24.f + (btnW + gap), depositY }, { btnW, btnH }, "$200", [doDeposit]() { doDeposit(200.0); });
    uiButton(window, { pos.x + 24.f + 2.f * (btnW + gap), depositY }, { btnW, btnH }, Localization::t("qty_all"),
        [this, doDeposit]() { doDeposit(game_.money()); });

    float withdrawY = depositY + btnH + 34.f;
    uiText(window, { pos.x + 24.f, withdrawY - 20.f }, Localization::t("withdraw_button"), 13, sf::Color(200, 200, 200));
    uiButton(window, { pos.x + 24.f, withdrawY }, { btnW, btnH }, "$50", [doWithdraw]() { doWithdraw(50.0); });
    uiButton(window, { pos.x + 24.f + (btnW + gap), withdrawY }, { btnW, btnH }, "$200", [doWithdraw]() { doWithdraw(200.0); });
    uiButton(window, { pos.x + 24.f + 2.f * (btnW + gap), withdrawY }, { btnW, btnH }, Localization::t("qty_all"),
        [this, doWithdraw]() { doWithdraw(game_.bankBalance()); });

    if (!overlayFeedback_.empty()) {
        uiText(window, { pos.x + 24.f, pos.y + size.y - 30.f }, overlayFeedback_, 15, overlayFeedbackColor_);
    }
}

void GameWorld::drawWarehouseOverlay(sf::RenderWindow& window) {
    // Taller than before -- now leads with what's actually sitting in
    // storage (see the scrollable list below) instead of jumping straight
    // to the upgrade pitch, with Upgrade pushed down under the list so the
    // reading order is "see what you have, then decide whether to expand".
    sf::Vector2f pos(360.f, 110.f), size(560.f, 600.f);
    uiPanelBg(window, pos, size);
    uiText(window, { pos.x + 24.f, pos.y + 16.f }, Localization::t("menu_warehouse_header"), 20, sf::Color(232, 212, 120), true);
    uiButton(window, { pos.x + size.x - 120.f, pos.y + 14.f }, { 100.f, 34.f }, Localization::t("close_button"), [this]() { closeOverlay(); });

    std::ostringstream info;
    info << Localization::t("warehouse_level_label") << game_.warehouseLevel()
        << Localization::t("warehouse_cap_label") << formatNumber(game_.maxStockPerGood()) << ")";
    uiText(window, { pos.x + 24.f, pos.y + 70.f }, info.str(), 15);

    uiText(window, { pos.x + 24.f, pos.y + 104.f }, Localization::t("warehouse_inventory_header"), 15, sf::Color(200, 200, 200), true);

    std::vector<GoodInfo> held;
    for (const auto& g : game_.goodInfos()) {
        if (g.stock > 0.0001) held.push_back(g);
    }

    constexpr float kListTopOffset = 132.f, kBottomReserved = 130.f;
    float listTop = pos.y + kListTopOffset;
    float listBottom = pos.y + size.y - kBottomReserved;

    if (held.empty()) {
        uiText(window, { pos.x + 24.f, listTop }, Localization::t("warehouse_empty_hint"), 14, sf::Color(160, 160, 160));
    } else {
        constexpr float rowH = 30.f;
        float contentH = static_cast<float>(held.size()) * rowH;
        float maxScroll = std::max(0.f, contentH - (listBottom - listTop));
        overlayScrollOffset_ = std::clamp(overlayScrollOffset_, 0.f, maxScroll);

        float rowY = listTop - overlayScrollOffset_;
        beginClip(window, sf::FloatRect(sf::Vector2f(pos.x, listTop), sf::Vector2f(size.x, listBottom - listTop)));
        for (const auto& g : held) {
            if (rowY >= listTop - rowH && rowY <= listBottom) {
                uiText(window, { pos.x + 24.f, rowY }, Localization::t(g.id), 14, sf::Color::White);
                uiText(window, { pos.x + size.x - 160.f, rowY }, formatNumber(g.stock), 14, sf::Color(220, 220, 220));
            }
            rowY += rowH;
        }
        endClip(window);
        if (maxScroll > 0.f) {
            uiText(window, { pos.x + size.x - 210.f, pos.y + kListTopOffset - 22.f }, Localization::t("scroll_hint"), 12, sf::Color(160, 160, 160));
        }
    }

    float costY = pos.y + size.y - kBottomReserved + 14.f;
    uiText(window, { pos.x + 24.f, costY }, Localization::t("warehouse_cost_prefix") + formatNumber(game_.warehouseNextCost()), 15);

    uiButton(window, { pos.x + 24.f, costY + 34.f }, { 200.f, 44.f }, Localization::t("upgrade_button"), [this]() {
        ActionResult r = game_.tryUpgradeWarehouse();
        if (r.success) setFeedback(Localization::t("warehouse_upgraded_prefix") + std::to_string(game_.warehouseLevel()), true);
        else setFeedback(Localization::t(r.messageKey), false);
    });

    if (!overlayFeedback_.empty()) {
        uiText(window, { pos.x + 24.f, pos.y + size.y - 24.f }, overlayFeedback_, 15, overlayFeedbackColor_);
    }
}

void GameWorld::drawContractsOverlay(sf::RenderWindow& window) {
    sf::Vector2f pos(360.f, 190.f), size(600.f, 440.f);
    uiPanelBg(window, pos, size);
    uiText(window, { pos.x + 24.f, pos.y + 16.f }, Localization::t("menu_contracts_header"), 20, sf::Color(232, 212, 120), true);
    uiButton(window, { pos.x + size.x - 120.f, pos.y + 14.f }, { 100.f, 34.f }, Localization::t("close_button"), [this]() { closeOverlay(); });

    auto list = game_.contracts();
    float y = pos.y + 70.f, rowH = 50.f;
    for (size_t i = 0; i < list.size(); ++i) {
        sf::RectangleShape rowBg(sf::Vector2f(size.x - 48.f, rowH - 8.f));
        rowBg.setPosition(sf::Vector2f(pos.x + 24.f, y));
        rowBg.setFillColor(sf::Color(50, 52, 62));
        window.draw(rowBg);
        uiText(window, { pos.x + 34.f, y + 12.f }, Localization::t(list[i].goodId) + " @ $" + formatNumber(list[i].lockedPrice), 15);
        uiButton(window, { pos.x + size.x - 170.f, y + 3.f }, { 130.f, rowH - 16.f }, Localization::t("fulfill_button"),
            [this, idx = static_cast<int>(i)]() {
                ActionResult r = game_.tryFulfillContract(idx);
                if (r.success) setFeedback(Localization::t("contract_fulfilled_prefix") + formatNumber(r.amount), true);
                else setFeedback(Localization::t(r.messageKey), false);
            });
        y += rowH;
    }
    if (list.empty()) {
        uiText(window, { pos.x + 24.f, y }, Localization::t("contract_none_active"), 14, sf::Color(180, 180, 180));
        y += rowH;
    }

    y += 20.f;
    auto goods = game_.goodInfos();
    bool haveSlot = static_cast<int>(list.size()) < 3;
    if (!haveSlot) {
        uiText(window, { pos.x + 24.f, y }, Localization::t("contract_slots_full"), 14, sf::Color(180, 120, 120));
    } else if (selectedGoodIndex_ >= 0 && selectedGoodIndex_ < static_cast<int>(goods.size())) {
        const auto& sel = goods[static_cast<size_t>(selectedGoodIndex_)];
        uiText(window, { pos.x + 24.f, y },
            Localization::t("contract_sign_selected_prefix") + Localization::t(sel.id) + " ($" + formatNumber(sel.price) + ")", 14);
        y += 26.f;
        uiButton(window, { pos.x + 24.f, y }, { 240.f, 44.f }, Localization::t("contract_sign_option"),
            [this, id = sel.id]() {
                ActionResult r = game_.trySignContract(id);
                if (r.success) setFeedback(Localization::t("contract_signed_prefix") + Localization::t(id), true);
                else setFeedback(Localization::t(r.messageKey), false);
            });
    }

    if (!overlayFeedback_.empty()) {
        uiText(window, { pos.x + 24.f, pos.y + size.y - 30.f }, overlayFeedback_, 15, overlayFeedbackColor_);
    }
}

void GameWorld::run() {
    // Every building/UI position and the walk-off-edge bounds are absolute
    // pixel coordinates sized for the logical windowSize_ (fixed at
    // 1280x820), not the real window -- gameView_ (see applyVideoMode) maps
    // one onto the other, so the player's chosen resolution/fullscreen never
    // desyncs mouse clicks or the movement bounds from what's drawn.
    settings_ = SettingsManager::load();
    UpdateChecker::startCheck(); // no-op if Main.cpp's console path already started it
    sf::RenderWindow window;
    applyVideoMode(window);

    // Microsoft YaHei covers both Chinese and Latin glyphs; Arial (Latin-only)
    // is just the fallback for systems without it, and won't render Chinese text.
    fontLoaded_ = font_.openFromFile("C:/Windows/Fonts/msyh.ttc");
    if (!fontLoaded_) fontLoaded_ = font_.openFromFile("C:/Windows/Fonts/arial.ttf");
    if (!fontLoaded_) {
        std::cout << "[GameWorld] Could not load a system font; building/HUD labels will be blank, but the game still works.\n";
    }

    initAudio();
    buildZones();
    currentZone_ = 0;
    playerPos_ = sf::Vector2f(620.f, 400.f);
    // Baseline so a mid-cycle load doesn't fire a spurious transition on the
    // very first frame the season is actually checked (see the tick loop below).
    lastSeason_ = game_.currentSeason();

    while (window.isOpen() && showingTutorial_) {
        while (const std::optional event = window.pollEvent()) {
            if (event->is<sf::Event::Closed>()) window.close();
            else if (event->is<sf::Event::KeyPressed>() || event->is<sf::Event::MouseButtonPressed>()) showingTutorial_ = false;
        }
        drawTutorial(window);
    }

    // Game::startSession() (called before GameWorld was even constructed --
    // see Main.cpp) already ran the offline catch-up and would otherwise
    // only have printed what happened to a console window the player isn't
    // looking at anymore -- show it in-window instead, once, right as the
    // world actually opens.
    if (window.isOpen() && game_.lastWelcomeBack().elapsedSeconds > 0) {
        openOverlay(OverlayKind::WelcomeBack);
    }

    sf::Clock clock;
    while (window.isOpen()) {
        while (const std::optional event = window.pollEvent()) {
            if (event->is<sf::Event::Closed>()) {
                window.close();
            } else if (const auto* keyPressed = event->getIf<sf::Event::KeyPressed>()) {
                if (awaitingRebind_ != RebindAction::None) {
                    // Settings overlay is waiting for a key to (re)bind (see
                    // drawSettingsOverlay's "Rebind" buttons) -- this keypress
                    // is consumed entirely here, not passed through to the
                    // normal handling below (so binding Interact to E, say,
                    // doesn't also trigger an E-interact this same frame).
                    if (keyPressed->code == sf::Keyboard::Key::Escape) {
                        awaitingRebind_ = RebindAction::None; // cancel, no change
                    } else if (sf::Keyboard::Key* target = settings_.keys.find(awaitingRebind_)) {
                        sf::Keyboard::Key newKey = keyPressed->code;
                        sf::Keyboard::Key oldKey = *target;
                        // Already used by a different bindable action -> swap
                        // instead of leaving both actions on the same key.
                        RebindAction swappedWith = RebindAction::None;
                        for (RebindAction other : { RebindAction::MoveUp, RebindAction::MoveDown, RebindAction::MoveLeft,
                            RebindAction::MoveRight, RebindAction::Interact, RebindAction::QuickUpgrade,
                            RebindAction::Minimap, RebindAction::Minigame }) {
                            if (other == awaitingRebind_) continue;
                            if (sf::Keyboard::Key* otherKey = settings_.keys.find(other); otherKey && *otherKey == newKey) {
                                *otherKey = oldKey;
                                swappedWith = other;
                                break;
                            }
                        }
                        *target = newKey;
                        SettingsManager::save(settings_);
                        setFeedback(swappedWith != RebindAction::None
                            ? Localization::t("settings_rebind_swapped_prefix") + Localization::t(rebindActionLabelKey(swappedWith)) + Localization::t("settings_rebind_swapped_suffix")
                            : Localization::t("settings_rebind_updated"), true);
                        awaitingRebind_ = RebindAction::None;
                    }
                } else if (keyPressed->code == sf::Keyboard::Key::Escape && currentOverlay_ == OverlayKind::None) {
                    openOverlay(OverlayKind::Pause);
                } else if (keyPressed->code == sf::Keyboard::Key::Escape && currentOverlay_ != OverlayKind::None
                    && currentOverlay_ != OverlayKind::DeathNotice) {
                    closeOverlay();
                } else if (keyPressed->code == settings_.keys.interact && currentOverlay_ == OverlayKind::None) {
                    if (Npc* npc = findNearbyNpc(kInteractRadius)) {
                        handleNpcTalk(*npc);
                    } else if (Forageable* f = findNearbyForageable(kForageRadius)) {
                        ActionResult r = game_.tryForage();
                        if (r.success) {
                            f->active = false;
                            f->respawnTimer = kForageRespawnSeconds;
                            if (interactSound_) interactSound_->play();
                            setFeedback(Localization::t("forage_prefix") + formatNumber(r.amount) + " " + Localization::t(r.goodId), true);
                        }
                    } else if (const WorldBuilding* b = findNearbyBuilding(kInteractRadius)) {
                        handleInteraction(*b);
                    }
                } else if (keyPressed->code == settings_.keys.interact && currentOverlay_ == OverlayKind::Dialogue) {
                    // "Press E to continue" -- symmetric with E opening it in the first place.
                    closeOverlay();
                } else if (keyPressed->code == settings_.keys.minimap && currentOverlay_ == OverlayKind::None) {
                    showMinimap_ = !showMinimap_;
                } else if (keyPressed->code == settings_.keys.quickUpgrade && currentOverlay_ == OverlayKind::None) {
                    // Quick-upgrade: one level on whichever unlocked business
                    // building is nearby, without opening its panel -- for a
                    // player who already knows what they're doing and doesn't
                    // want to stop and click through it every time. Guarded
                    // against service buildings (market, staff, ...): those
                    // aren't in businessInfos() at all, so isBusinessLocked()
                    // alone would silently say "not locked" for them too.
                    // Routed through performBuildOrUpgrade rather than calling
                    // tryUpgradeBusinessBulk directly so pressing U at an
                    // unstarted plot starts construction (with feedback)
                    // instead of silently failing.
                    if (const WorldBuilding* b = findNearbyBuilding(kInteractRadius)) {
                        bool isBusiness = false;
                        for (const auto& info : game_.businessInfos()) {
                            if (info.id == b->id) { isBusiness = true; break; }
                        }
                        if (isBusiness && !game_.isBusinessLocked(b->id)) {
                            performBuildOrUpgrade(b->id);
                        }
                    }
                } else if (keyPressed->code == settings_.keys.minigame && currentOverlay_ == OverlayKind::None) {
                    // Minigames: only meaningful right next to the matching
                    // built business. Fishing Dock and Mine/Gold Mine share
                    // the timing-bar mechanic; Lumber Camp has its own.
                    if (const WorldBuilding* b = findNearbyBuilding(kInteractRadius)) {
                        if (b->id == "fishing" || b->id == "mine" || b->id == "goldmine") {
                            tryStartTimingMinigame(b->id);
                        } else if (b->id == "lumber") {
                            tryStartChopping();
                        } else if (b->id == "alchemist" || b->id == "winery") {
                            tryStartBrewing(b->id);
                        }
                    }
                } else if (keyPressed->code == sf::Keyboard::Key::Space && currentOverlay_ == OverlayKind::TimingMinigame) {
                    resolveTimingMinigame();
                } else if (keyPressed->code == sf::Keyboard::Key::Space && currentOverlay_ == OverlayKind::Chopping) {
                    choppingClicks_++;
                }
            } else if (const auto* mousePressed = event->getIf<sf::Event::MouseButtonPressed>()) {
                if (mousePressed->button == sf::Mouse::Button::Left) {
                    // Real window pixel -> logical windowSize_ space, through
                    // whatever letterboxed view applyVideoMode set up -- keeps
                    // every ClickRegion/building hit test correct regardless
                    // of the player's chosen resolution/fullscreen.
                    sf::Vector2f click = window.mapPixelToCoords(mousePressed->position, gameView_);
                    if (currentOverlay_ != OverlayKind::None) {
                        // Click regions were registered by last frame's draw of this same
                        // overlay (immediate-mode style) — the layout doesn't move frame to
                        // frame, so testing against them here is safe and simple.
                        for (const auto& region : overlayClickRegions_) {
                            if (region.bounds.contains(click)) { region.onClick(); break; }
                        }
                    } else if (achievementsButtonBounds().contains(click)) {
                        openOverlay(OverlayKind::Achievements);
                    } else if (howToPlayButtonBounds().contains(click)) {
                        openOverlay(OverlayKind::HowToPlay);
                    } else if (recipeBookButtonBounds().contains(click)) {
                        openOverlay(OverlayKind::RecipeBook);
                    } else {
                        bool handled = false;
                        for (auto& npc : zones_[currentZone_].npcs) {
                            sf::FloatRect npcRect(npc.pos - sf::Vector2f(kPlayerSize / 2.f, kPlayerSize / 2.f), sf::Vector2f(kPlayerSize, kPlayerSize));
                            if (npcRect.contains(click)) { handleNpcTalk(npc); handled = true; break; }
                        }
                        if (!handled) {
                            for (const auto& b : zones_[currentZone_].buildings) {
                                if (sf::FloatRect(b.position, b.size).contains(click)) { handleInteraction(b); break; }
                            }
                        }
                    }
                }
            } else if (const auto* wheel = event->getIf<sf::Event::MouseWheelScrolled>()) {
                // Shared by every scrollable overlay (Tree, How to Play,
                // Market, Staff's focus grid) -- each one's own draw
                // function clamps this against that frame's actual content
                // height, so accumulating the raw delta here is harmless
                // even for overlays that don't happen to need scrolling.
                if (currentOverlay_ != OverlayKind::None) {
                    overlayScrollOffset_ -= wheel->delta * 3.f * 24.f;
                    if (overlayScrollOffset_ < 0.f) overlayScrollOffset_ = 0.f;
                }
            }
        }

        float dt = clock.restart().asSeconds();

        historySampleTimer_ -= dt;
        if (historySampleTimer_ <= 0.f) {
            historySampleTimer_ = kHistorySampleInterval;
            moneyHistory_.push_back(static_cast<float>(game_.money()));
            if (moneyHistory_.size() > kMaxHistorySamples) moneyHistory_.erase(moneyHistory_.begin());
        }

        // Keeps counting down even while some other menu is open, unlike
        // the movement/world-tick block below.
        if (fishingCooldown_ > 0.f) fishingCooldown_ -= dt;
        if (miningCooldown_ > 0.f) miningCooldown_ -= dt;
        if (lumberCooldown_ > 0.f) lumberCooldown_ -= dt;
        if (alchemistCooldown_ > 0.f) alchemistCooldown_ -= dt;
        if (wineryCooldown_ > 0.f) wineryCooldown_ -= dt;
        if (currentOverlay_ == OverlayKind::TimingMinigame) minigameIndicatorPhase_ += dt;
        if (currentOverlay_ == OverlayKind::Chopping) updateChopping(dt);
        if (currentOverlay_ == OverlayKind::Brewing) updateBrewing(dt);
        updateForaging(dt);

        // Movement, zone transitions, and the world tick are all paused while
        // an overlay is open — mirroring how the classic console version
        // effectively blocked on a menu until it returned.
        if (currentOverlay_ == OverlayKind::None) {
            updateDayNightAndWeather(dt);
            waterWaveTimer_ += dt;

            // Arrow keys always work as a fixed second binding on top of
            // whichever key settings_.keys assigns to each direction (see
            // Settings.h) -- so rebinding never leaves the player with no way
            // to move, even mid-rebind.
            sf::Vector2f move(0.f, 0.f);
            if (sf::Keyboard::isKeyPressed(settings_.keys.moveUp) || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Up))       move.y -= 1.f;
            if (sf::Keyboard::isKeyPressed(settings_.keys.moveDown) || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Down))   move.y += 1.f;
            if (sf::Keyboard::isKeyPressed(settings_.keys.moveLeft) || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Left))   move.x -= 1.f;
            if (sf::Keyboard::isKeyPressed(settings_.keys.moveRight) || sf::Keyboard::isKeyPressed(sf::Keyboard::Key::Right)) move.x += 1.f;

            if (move.x < 0.f) playerFacingLeft_ = true;
            else if (move.x > 0.f) playerFacingLeft_ = false;

            bool isMoving = (move.x != 0.f || move.y != 0.f);
            if (isMoving) {
                float len = std::sqrt(move.x * move.x + move.y * move.y);
                move.x /= len;
                move.y /= len;
            }
            bool sprinting = sf::Keyboard::isKeyPressed(sf::Keyboard::Key::LShift) ||
                              sf::Keyboard::isKeyPressed(sf::Keyboard::Key::RShift);
            float speed = kMoveSpeed * (sprinting ? kSprintMultiplier : 1.f);
            sf::Vector2f delta = move * speed * dt;

            if (isMoving) {
                footstepTimer_ -= dt;
                if (footstepTimer_ <= 0.f) {
                    if (footstepSound_) footstepSound_->play();
                    footstepTimer_ = sprinting ? 0.32f / kSprintMultiplier : 0.32f;
                }
                // Drives drawPlayer's walk bob (see drawPixelPerson) -- only
                // advances while actually moving, so standing still holds a
                // stable pose instead of endlessly bobbing in place.
                playerWalkTimer_ += dt * (sprinting ? 16.f : 10.f);
            } else {
                footstepTimer_ = 0.f; // ready to play immediately the moment they move again
                playerWalkTimer_ = 0.f;
            }

            sf::Vector2f tryX = playerPos_ + sf::Vector2f(delta.x, 0.f);
            if (!collidesWithBuilding(tryX, kPlayerSize) && !collidesWithTree(tryX, kPlayerSize) && !collidesWithWater(tryX, kPlayerSize)) playerPos_.x = tryX.x;
            sf::Vector2f tryY = playerPos_ + sf::Vector2f(0.f, delta.y);
            if (!collidesWithBuilding(tryY, kPlayerSize) && !collidesWithTree(tryY, kPlayerSize) && !collidesWithWater(tryY, kPlayerSize)) playerPos_.y = tryY.y;

            // Zone edge transitions.
            const Zone& z = zones_[currentZone_];
            float maxX = static_cast<float>(windowSize_.x) - kPlayerSize;
            float maxY = static_cast<float>(windowSize_.y) - kPlayerSize;
            if (playerPos_.x < 0.f) {
                if (z.west >= 0) { currentZone_ = z.west; playerPos_.x = maxX - kEdgeMargin; }
                else playerPos_.x = 0.f;
            } else if (playerPos_.x > maxX) {
                if (z.east >= 0) { currentZone_ = z.east; playerPos_.x = kEdgeMargin; }
                else playerPos_.x = maxX;
            }
            if (playerPos_.y < 0.f) {
                if (z.north >= 0) { currentZone_ = z.north; playerPos_.y = maxY - kEdgeMargin; }
                else playerPos_.y = 0.f;
            } else if (playerPos_.y > maxY) {
                if (z.south >= 0) { currentZone_ = z.south; playerPos_.y = kEdgeMargin; }
                else playerPos_.y = maxY;
            }

            updateNpcs(dt);

            // Live-only nudge to the Farm's output: rain helps crops outside
            // Winter, snow during Winter hurts them (see Game::tickBackground's
            // weatherMult doc comment) -- purely a "while actually playing"
            // flavor effect, not something offline catch-up tries to guess at.
            double weatherMult = 1.0;
            if (raining_) {
                weatherMult = (game_.currentSeason() == Season::Winter) ? Game::kSnowPenaltyMultiplier : Game::kRainBonusMultiplier;
            }
            TickOutcome outcome = game_.tickBackground(weatherMult);
            // Queue any newly-unlocked achievements for the toast (see
            // updateAchievementToast/drawAchievementToast) -- replaces the
            // old "did the unlocked count go up" check with the actual ids,
            // needed now that the toast has to say which one it was.
            for (const std::string& id : game_.drainNewlyUnlockedAchievements()) {
                achievementToastQueue_.push_back(id);
            }
            // Same idea for completed construction sites (see Business::
            // constructionDaysRemaining) -- a general "X built!" toast even
            // when the player isn't standing next to it.
            {
                std::vector<std::string> completed = game_.drainCompletedConstructions();
                if (!completed.empty()) {
                    std::string msg = Localization::t("construction_completed_prefix") + Localization::t(completed[0]);
                    for (size_t i = 1; i < completed.size(); ++i) msg += ", " + Localization::t(completed[i]);
                    setFeedback(msg, true);
                }
            }
            handleTickOutcome(outcome); // opens the death-notice overlay if outcome.died

            // The season can only actually change while the world tick above
            // is running (i.e. no overlay/pause), so this is the only place
            // that needs to check for it.
            Season nowSeason = game_.currentSeason();
            if (nowSeason != lastSeason_) {
                lastSeason_ = nowSeason;
                seasonTransitionTo_ = nowSeason;
                seasonTransitionActive_ = true;
                seasonTransitionTimer_ = 0.f;
                playMusicForSeason(nowSeason);
            }
        }
        updateSeasonTransition(dt); // ticks (and, near the end, resolves) regardless of overlay state
        updateMusicCrossfade(dt);   // same -- background music keeps fading/playing through a paused menu
        updateAchievementToast(dt); // same -- an unlock while a menu is open still counts down and shows
        updateEventToast(dt);       // same -- a queued event line still counts down and shows through a paused menu

        // Ticks down regardless of overlay state: previously this only ran in
        // the "overlay open" branch above (feedback used to only ever be set
        // from inside an overlay's own button callbacks), but the locked-
        // building world-view toast sets it while no overlay is open too.
        if (overlayFeedbackTimer_ > 0.f) {
            overlayFeedbackTimer_ -= dt;
            if (overlayFeedbackTimer_ <= 0.f) overlayFeedback_.clear();
        }

        if (!updateAvailable_) { // cheap mutex check, not the network call itself -- fine every frame
            UpdateChecker::Result r;
            if (UpdateChecker::pollResult(r)) {
                updateAvailable_ = true;
                updateLatestVersion_ = r.latestVersion;
                updateReleaseUrl_ = r.releaseUrl;
            }
        }

        window.clear(sf::Color(58, 128, 68));
        drawZone(window);

        if (currentOverlay_ == OverlayKind::None) {
            if (const WorldBuilding* near = findNearbyBuilding(kInteractRadius)) {
                sf::RectangleShape highlight(near->size);
                highlight.setPosition(near->position);
                highlight.setFillColor(sf::Color::Transparent);
                highlight.setOutlineThickness(3.f);
                highlight.setOutlineColor(sf::Color::Yellow);
                window.draw(highlight);
            }
        }

        drawPlayer(window, playerPos_, playerFacingLeft_, playerWalkTimer_);

        // Tints/rains on the world (and the player standing in it), but not
        // the legend/minimap/HUD/overlays drawn after this -- those stay
        // fully readable regardless of time of day or weather.
        drawDayNightOverlay(window);
        drawWeather(window);
        drawSeasonalAmbient(window);

        drawLegend(window);
        if (showMinimap_) drawMinimap(window);
        drawHud(window);
        drawNetWorthPanel(window);
        drawLifeStatusPanel(window);
        drawAchievementsButton(window);
        drawHowToPlayButton(window);
        drawRecipeBookButton(window);
        drawUpdateBanner(window);
        drawOverlayRoot(window); // drawn last so it sits on top of everything else
        drawSeasonTransitionOverlay(window); // drawn even later -- covers overlays too, so a season change is never missed
        drawAchievementToast(window); // drawn last of all -- sits above overlays and the season transition alike
        drawEventToast(window);       // same layer, opposite corner

        window.display();
    }

    game_.exitAndSave();
}
