#pragma once
#include <SFML/Graphics.hpp>
#include <SFML/Audio.hpp>
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include "Game.h"
#include "Settings.h"

// A single interactable structure. `id` maps to a menu overlay (see
// OverlayKind) rather than the console anymore.
// `labelColor` is the tier-coded color for the name text (green/orange/
// purple/blue) — kept independent of the shape's own colors now that
// tier-1 resource buildings (farm, mine, etc.) use realistic shapes/palettes
// instead of a flat tier-colored block.
struct WorldBuilding {
    std::string id;
    std::string labelKey;
    sf::Vector2f position; // top-left corner
    sf::Vector2f size;
    sf::Color labelColor;
};

// Non-interactive scenery. Trees block movement (obstacles); bushes/patches don't.
struct Decoration {
    enum class Kind { Tree, Bush, GrassPatch, Path, Water };
    Kind kind;
    sf::Vector2f position;
    sf::Vector2f size; // used by Path/GrassPatch/Water; trees/bushes use position as center
};

// A wandering villager with a few localized dialogue lines, and occasionally
// a simple fetch quest ("bring me N of X for $Y") -- session-only, not saved,
// generated at a slow random interval whenever the NPC doesn't have one active.
struct Npc {
    std::string nameKey;
    sf::Vector2f home;
    sf::Vector2f pos;
    sf::Vector2f target;
    float wanderTimer = 0.f;
    sf::Color color;
    std::vector<std::string> linesEn;
    std::vector<std::string> linesZh;
    int lineIndex = 0;

    bool hasQuest = false;
    std::string questGoodId;
    double questQty = 0.0;
    double questReward = 0.0;
    float questRollTimer = 10.f; // counts down to the next chance of offering a quest

    // Traveling merchant deal (npc_trader only -- see updateNpcs/generateDealFor):
    // a limited-time bundle of `dealQty` of `dealGoodId` for a flat
    // `dealTotalPrice` (a discount off buying it normally at market price).
    // Mutually exclusive with the quest system above; npc_trader rolls deals
    // instead of quests, everyone else rolls quests instead of deals.
    bool hasDeal = false;
    std::string dealGoodId;
    double dealQty = 0.0;
    double dealTotalPrice = 0.0;
    float dealRollTimer = 10.f;
};

// A walk-up-and-collect pickup (Highlands foraging) -- distinct from
// Decoration in that it's interactive and has a collected/respawning state.
// `home` never changes; `active` toggles off on collection and back on once
// `respawnTimer` counts down to 0.
struct Forageable {
    sf::Vector2f home;
    bool active = true;
    float respawnTimer = 0.f;
};

// One screen-sized area of the town. Walking off an edge that has a linked
// zone (>= 0) travels there, entering from the opposite side; an edge with no
// link (-1) is just a wall.
struct Zone {
    std::string nameKey;
    std::vector<WorldBuilding> buildings;
    std::vector<Decoration> decorations;
    std::vector<Npc> npcs;
    std::vector<Forageable> forageables;
    int north = -1, south = -1, east = -1, west = -1;
};

// Which in-window menu overlay (if any) currently owns input. While one is
// open, player movement and the world tick are both paused — mirroring how
// the console version effectively blocks on a menu until it returns.
enum class OverlayKind {
    None, Businesses, Tree, Market, Staff, Sleep, Eat, Doctor, FastForward, Achievements, DeathNotice, Legacy, Dialogue,
    Bank, Warehouse, Contracts, HowToPlay, CropPicker, RecipeBook,
    TimingMinigame, // fishing (Fishing Dock) or mining (Mine/Gold Mine) -- see minigameBusinessId_
    Chopping,       // lumber (Lumber Camp) -- a different mash-to-target mechanic
    Brewing,        // alchemist or winery -- memorize-then-repeat a color sequence
    Pause,          // opened by Escape from the walking-around world -- "save or keep playing", plus a way into Settings
    Settings        // volume/resolution/fullscreen/key rebinding -- reached from Pause
};

// A clickable rectangle registered by whichever overlay is currently drawn,
// rebuilt fresh every frame since content (business list length, etc.) can change.
struct ClickRegion {
    sf::FloatRect bounds;
    std::function<void()> onClick;
};

// The graphical, real-time layer: a set of connected top-down zones the
// player walks around with WASD (or arrow keys), interacting with buildings
// by walking up and pressing E, or by clicking them directly, and bumping
// into wandering NPCs for a bit of flavor dialogue. Menu interactions open an
// in-window overlay (see OverlayKind) instead of switching to the console —
// all backed by Game's non-console data/action API. This class only owns
// movement, collision, rendering, and dispatching interactions; Game owns
// all actual economy/life state.
class GameWorld {
public:
    explicit GameWorld(Game& game);

    // Opens the window and runs until the player closes it (saving on exit).
    void run();

private:
    Game& game_;
    std::vector<Zone> zones_;
    int currentZone_ = 0;
    sf::Vector2f playerPos_;
    sf::Font font_;
    bool fontLoaded_ = false;
    sf::Vector2u windowSize_{ 1280u, 820u }; // logical resolution -- every drawing coordinate in this file is in this space, regardless of the real window size below
    sf::Vector2u actualWindowSize_{ 1280u, 820u }; // the real OS window/fullscreen size (see applyVideoMode) -- gameView_ maps logical space onto it
    sf::View gameView_;
    Settings settings_; // player-level prefs (volume/resolution/fullscreen/keybinds) -- loaded in run(), saved on every change (see SettingsManager)
    RebindAction awaitingRebind_ = RebindAction::None; // set while the Settings overlay is waiting for the next keypress to (re)bind an action
    bool showingTutorial_ = true;
    bool showMinimap_ = false; // toggled with M -- hidden by default so it doesn't sit over the Legend panel
    // Shared by every scrollable overlay (Tree, How to Play, Market, Staff's
    // focus grid) -- only one overlay is ever open at a time, so one offset
    // is enough; each draw function computes its own maxScroll and clamps
    // against it independently. Reset to 0 whenever any overlay opens.
    float overlayScrollOffset_ = 0.f;
    float waterWaveTimer_ = 0.f; // drives the animated shimmer on Decoration::Kind::Water (Harbor's shoreline)
    // OverlayKind::RecipeBook's grid/detail toggle (see drawRecipeBookOverlay)
    // -- empty = grid of every processed good, non-empty = detail view of
    // that one good's recipe. Reset to empty whenever the overlay (re)opens.
    std::string recipeBookSelectedGoodId_;

    // ---- Timing-bar minigames (see OverlayKind::TimingMinigame /
    // drawTimingMinigameOverlay): an indicator sweeps back and forth over a
    // bar; catching it inside the highlighted target zone (Space, or the
    // button) is a better result than missing. minigameBusinessId_ says
    // which building's own output good is rewarded (see
    // Game::tryMinigameBonus) -- "fishing" at the Fishing Dock, "mine"/
    // "goldmine" at the Mine/Gold Mine, sharing this same mechanic and
    // overlay, just reskinned by whichever id is active. Fishing and mining
    // have separate cooldowns (so playing one doesn't lock out the other),
    // each ticking down every frame regardless of overlay state so they
    // keep counting even while some other menu is open. ----
    std::string minigameBusinessId_;
    float fishingCooldown_ = 0.f;
    float miningCooldown_ = 0.f;
    float minigameIndicatorPhase_ = 0.f;
    float minigameTargetCenter_ = 0.5f;   // 0..1 along the bar
    float minigameTargetHalfWidth_ = 0.08f;

    // ---- Lumber chopping minigame (see OverlayKind::Chopping): mash Space
    // (or click) to reach a click target before a short timer runs out --
    // a different mechanic from the timing bar above, same "press F near
    // the building" entry point and cooldown pattern. ----
    static constexpr int kChoppingClicksTarget = 10;
    static constexpr float kChoppingTimeWindow = 4.f;
    float lumberCooldown_ = 0.f;
    int choppingClicks_ = 0;
    float choppingTimeLeft_ = 0.f;

    // ---- Highlands foraging (see Zone::forageables / Forageable): walk up
    // and press E to collect a small random bonus good, distinct from the
    // timing/mashing minigames above in being exploration-paced rather than
    // reflex-paced. Respawn timing is per-spot (see Forageable), not a
    // single cooldown. ----
    static constexpr float kForageRadius = 30.f;
    static constexpr float kForageRespawnSeconds = 45.f;

    // ---- Brewing minigame (see OverlayKind::Brewing): memorize a short
    // color sequence, then reproduce it by clicking -- a third minigame
    // mechanic (Alchemist/Winery), reusing minigameBusinessId_ from the
    // timing bar above since only one minigame overlay is ever open at once.
    // Alchemist and Winery get separate cooldowns, same reasoning as
    // fishing/mining having separate ones. ----
    static constexpr int kBrewSequenceLength = 4;
    static constexpr float kBrewShowSeconds = 3.f;
    std::vector<int> brewSequence_;      // each 0..3, one of 4 colors
    size_t brewStep_ = 0;                // how many correct clicks so far, during input phase
    float brewShowTimer_ = 0.f;
    bool brewShowingSequence_ = true;
    float alchemistCooldown_ = 0.f;
    float wineryCooldown_ = 0.f;

    // ---- Day/night cycle + weather: purely visual (flat-color overlays,
    // matching the rest of the world's plain-shape look -- no gradients or
    // particle-system glow), paused whenever an overlay/menu is open, same
    // as movement and the world tick. ----
    float dayNightTimer_ = 0.f;      // seconds into the current cycle
    bool raining_ = false;
    float weatherCheckTimer_ = 30.f; // counts down to the next rain/clear roll
    float rainTime_ = 0.f;           // drives the falling-rain animation while raining_
    float seasonalAmbientTimer_ = 0.f; // drives the always-on per-season particles (snow/leaves/petals/haze), independent of raining_

    // ---- Season-change color-block transition (see updateSeasonTransition/
    // drawSeasonTransitionOverlay): three bars sweep in, hold on the new
    // season's name, then sweep back out. Ticks and draws regardless of
    // overlay state so it's never missed behind a paused menu. ----
    static constexpr float kSeasonTransitionDuration = 1.6f;
    Season lastSeason_ = Season::Spring;   // set once in run() before the loop so the first frame never falsely "changes"
    bool seasonTransitionActive_ = false;
    float seasonTransitionTimer_ = 0.f;    // 0..kSeasonTransitionDuration
    Season seasonTransitionTo_ = Season::Spring;

    // Procedurally-synthesized sound effects (no asset files) — a footstep
    // tick while moving, a ding on interaction, and a fanfare on achievement
    // unlock. sf::Sound has no default constructor in SFML 3 (it needs a
    // buffer up front), so these stay unset until initAudio() builds the buffers.
    sf::SoundBuffer footstepBuffer_;
    sf::SoundBuffer interactBuffer_;
    sf::SoundBuffer achievementBuffer_;
    sf::SoundBuffer upgradeBuffer_;
    std::optional<sf::Sound> footstepSound_;
    std::optional<sf::Sound> interactSound_;
    std::optional<sf::Sound> achievementSound_;
    std::optional<sf::Sound> upgradeSound_;
    float footstepTimer_ = 0.f;

    // ---- Achievement unlock toast (see Game::drainNewlyUnlockedAchievements
    // and drawAchievementToast): a Minecraft-style "Achievement Unlocked!"
    // box in the bottom-left corner. Multiple unlocks in the same tick (e.g.
    // a big offline catch-up crossing several thresholds at once) queue up
    // and show one at a time rather than overlapping. ----
    static constexpr float kAchievementToastSeconds = 3.f;
    std::vector<std::string> achievementToastQueue_; // ids waiting to be shown, oldest first
    std::string currentAchievementToastId_;          // empty = nothing currently shown
    float achievementToastTimer_ = 0.f;              // counts down while currentAchievementToastId_ is non-empty

    // Ambient weather noise (synthesized white noise, looped) -- plays while
    // raining_ is true (see updateDayNightAndWeather), covering both the
    // rain and re-skinned Winter snow visuals since both just check raining_.
    sf::SoundBuffer ambientRainBuffer_;
    std::optional<sf::Sound> ambientRainSound_;

    // ---- Background music: one short looping melody per season (same
    // synthesis technique as the SFX above -- no asset files), indexed by
    // static_cast<int>(Season). Switches tracks the moment the season does
    // (see the season-change detection in run()), so the mood changes right
    // alongside the color-block transition and the ambient particles.
    // Two alternating Sound slots (rather than one) so the outgoing track
    // can fade out while the incoming one fades in, instead of a hard cut. ----
    sf::SoundBuffer musicBuffers_[4];
    std::optional<sf::Sound> musicSounds_[2];
    int musicActiveIndex_ = 0;
    bool musicCrossfadeActive_ = false;
    float musicCrossfadeTimer_ = 0.f;
    int musicCrossfadeFromIndex_ = 0;
    static constexpr float kMusicCrossfadeDuration = 0.6f;
    void playMusicForSeason(Season season);
    void updateMusicCrossfade(float dt);
    void applyMusicVolume();

    // ---- Net worth history (session-only, not saved) for the sparkline panel ----
    std::vector<float> moneyHistory_;
    float historySampleTimer_ = 0.f;

    // ---- Overlay UI state ----
    OverlayKind currentOverlay_ = OverlayKind::None;
    std::vector<ClickRegion> overlayClickRegions_;
    int selectedGoodIndex_ = 0;       // Market overlay: which good is selected for buy/sell
    std::string focusedBusinessId_;   // Businesses overlay: which single building was walked up to
    std::string overlayFeedback_;     // transient result text ("Upgraded!", "Not enough cash", ...)
    sf::Color overlayFeedbackColor_ = sf::Color::White;
    float overlayFeedbackTimer_ = 0.f;
    std::string deathNoticeMessage_;  // populated when a TickOutcome says the character died
    int deathNoticeGeneration_ = 0;
    std::string dialogueSpeaker_;     // Dialogue overlay: NPC name for the current line
    std::string dialogueText_;        // Dialogue overlay: the current line itself
    // Set only when the current dialogue is showing an active quest, so the
    // "Turn In" button has something to mutate on success. Safe to hold: all
    // zones' Npc vectors are fully populated once by buildZones() at startup
    // and never resized afterward, so no push_back ever invalidates this.
    Npc* dialogueNpc_ = nullptr;

    void buildZones();
    void initAudio();
    void generateQuestFor(Npc& npc);
    void generateDealFor(Npc& npc); // npc_trader only -- see Npc::hasDeal

    void openOverlay(OverlayKind kind);
    void closeOverlay();
    void setFeedback(const std::string& text, bool success);
    void handleTickOutcome(const TickOutcome& outcome); // shows the death overlay if outcome.died
    void performBuy(double qty);  // acts on the Market overlay's selectedGoodIndex_
    void performSell(double qty);
    void performEat(double qty);
    // Shared by the Businesses overlay's upgrade buttons and the near-a-
    // building keyboard shortcut: plays the upgrade chime on success, sets
    // the feedback toast either way.
    void handleUpgradeResult(const ActionResult& r, const std::string& businessId);
    // Shared by the Businesses overlay's button area and the U-key quick
    // action (see OverlayKind::None's keyPressed handling): routes to
    // tryStartConstruction if this business is still an unstarted level-0
    // plot that requiresConstruction(), a "still building" toast if a site
    // is already in progress, or the plain tryUpgradeBusinessBulk(id, 1)
    // path for everything else (free starters, or already built).
    void performBuildOrUpgrade(const std::string& businessId);

    void handleInteraction(const WorldBuilding& building);
    void handleNpcTalk(Npc& npc);
    // Farmer Nell's every-other-visit line: names the current season and
    // the Farm's actively-planted crop instead of pulling from her fixed
    // linesEn/linesZh (see handleNpcTalk).
    std::string farmerSeasonLine() const;
    const WorldBuilding* findNearbyBuilding(float radius) const;
    Npc* findNearbyNpc(float radius);
    Forageable* findNearbyForageable(float radius);
    void updateNpcs(float dt);
    // `size` is the side length of the square hitbox (player) or the
    // diameter-ish reference used for the tree's circular check (see
    // collidesWithTree's minDist math) -- callers pass their own actor's
    // size so NPCs (see updateNpcs) can reuse the same checks as the player.
    bool collidesWithBuilding(sf::Vector2f pos, float size) const;
    bool collidesWithTree(sf::Vector2f pos, float size) const;
    sf::FloatRect achievementsButtonBounds() const;
    sf::FloatRect howToPlayButtonBounds() const;
    sf::FloatRect recipeBookButtonBounds() const;
    void drawHowToPlayButton(sf::RenderWindow& window);
    void drawRecipeBookButton(sf::RenderWindow& window);
    // Grid of every processed good (anything produced by a business with at
    // least one input -- raw materials have no recipe and aren't listed) as
    // a small color-block icon + name, 3 per row; clicking one switches to a
    // detail view of that good's recipe (see recipeBookSelectedGoodId_).
    void drawRecipeBookOverlay(sf::RenderWindow& window);

    void drawZone(sf::RenderWindow& window);
    void drawBuilding(sf::RenderWindow& window, const WorldBuilding& b);
    void drawCottageShape(sf::RenderWindow& window, const WorldBuilding& b);
    void drawFarmShape(sf::RenderWindow& window, const WorldBuilding& b);
    void drawMineShape(sf::RenderWindow& window, const WorldBuilding& b);
    void drawLumberShape(sf::RenderWindow& window, const WorldBuilding& b);
    void drawQuarryShape(sf::RenderWindow& window, const WorldBuilding& b);
    // Dedicated shapes for the other single-building tier-1 producers that
    // (like the farm/mine/lumber/quarry above) are common/important enough
    // to earn their own look rather than sharing an archetype.
    void drawPastureShape(sf::RenderWindow& window, const WorldBuilding& b);    // sheep
    void drawOrchardShape(sf::RenderWindow& window, const WorldBuilding& b);    // orchard
    void drawHerbGardenShape(sf::RenderWindow& window, const WorldBuilding& b); // herbgarden
    void drawVineyardShape(sf::RenderWindow& window, const WorldBuilding& b);   // vineyard
    void drawGoldMineShape(sf::RenderWindow& window, const WorldBuilding& b);   // goldmine
    // Reusable archetypes shared by many building ids, each taking a small
    // "accent glyph" kind + color (see the Accent constants and
    // drawAccentGlyph in GameWorld.cpp) so buildings sharing a base shape
    // still read as visually distinct.
    void drawFieldShape(sf::RenderWindow& window, const WorldBuilding& b, int accentKind, sf::Color accentColor);
    void drawWorkshopShape(sf::RenderWindow& window, const WorldBuilding& b, int accentKind, sf::Color accentColor);
    void drawDockShape(sf::RenderWindow& window, const WorldBuilding& b, int accentKind, sf::Color accentColor);
    void drawServiceHallShape(sf::RenderWindow& window, const WorldBuilding& b, int accentKind, sf::Color accentColor);
    void drawAccentGlyph(sf::RenderWindow& window, const WorldBuilding& b, int accentKind, sf::Color color);
    void drawTree(sf::RenderWindow& window, sf::Vector2f pos);
    void drawBush(sf::RenderWindow& window, sf::Vector2f pos);
    void drawNpc(sf::RenderWindow& window, const Npc& npc);
    void drawLockOverlay(sf::RenderWindow& window, const WorldBuilding& b);
    // First-build construction (see Business::constructionDaysRemaining /
    // Game::ConstructionInfo): drawn instead of the building's normal shape
    // while level == 0 and requiresConstruction() is true -- an unstarted
    // plot gets a signboard naming what goes there, a started one gets a
    // scaffolding/progress-bar site. Neither applies to the 4 free starters
    // or to a prerequisite-locked building (that still just gets
    // drawLockOverlay on top of its full shape, unchanged).
    void drawEmptyPlotShape(sf::RenderWindow& window, const WorldBuilding& b, const ConstructionInfo& ci);
    void drawConstructionSiteShape(sf::RenderWindow& window, const WorldBuilding& b, const ConstructionInfo& ci);
    void drawLegend(sf::RenderWindow& window);
    void drawMinimap(sf::RenderWindow& window);
    void drawHud(sf::RenderWindow& window);
    void drawNetWorthPanel(sf::RenderWindow& window);
    void updateDayNightAndWeather(float dt);
    sf::Color dayNightTint() const;
    sf::Color seasonTint() const; // subtle per-season wash, layered under the day/night tint
    void drawDayNightOverlay(sf::RenderWindow& window);
    void drawWeather(sf::RenderWindow& window);
    // Always-on per-season ambience (snow/leaves/petals/heat haze) -- distinct
    // from drawWeather's occasional rain/snow event above.
    void drawSeasonalAmbient(sf::RenderWindow& window);
    // Ticks seasonTransitionTimer_ (called every frame, even while an overlay
    // has paused the world) and draws the sweeping color-block wipe while active.
    void updateSeasonTransition(float dt);
    void drawSeasonTransitionOverlay(sf::RenderWindow& window);
    // Advances achievementToastTimer_ / pops the next queued id -- called
    // once per frame regardless of overlay state, same as
    // updateSeasonTransition.
    void updateAchievementToast(float dt);
    void drawAchievementToast(sf::RenderWindow& window);
    void drawAchievementsButton(sf::RenderWindow& window);
    void drawTutorial(sf::RenderWindow& window);

    // ---- Update check (see UpdateChecker.h): polled once per frame
    // (cheap -- a mutex-guarded flag check, no network on this thread) in
    // run()'s loop; once it reports an update, a small dismissible banner
    // stays up for the rest of the session offering to open the release
    // page in the player's browser. Never downloads/installs anything itself. ----
    bool updateAvailable_ = false;
    bool updateBannerDismissed_ = false;
    std::string updateLatestVersion_;
    std::string updateReleaseUrl_;
    void drawUpdateBanner(sf::RenderWindow& window);

    // ---- Window/video mode (see Settings::resolutionIndex/fullscreen):
    // (re)creates `window` at the chosen size/mode and rebuilds gameView_ as
    // a letterboxed view of the fixed logical windowSize_, so every existing
    // pixel-coordinate drawing/click call keeps working unchanged regardless
    // of the real window size. ----
    void applyVideoMode(sf::RenderWindow& window);
    // Rescales each synthesized sound's own baseline volume (see initAudio)
    // by settings_.sfxVolumePercent -- called once at startup and again
    // whenever the Settings overlay's volume buttons change it.
    void applySfxVolume();

    // ---- Menu overlay UI ----
    // Shared building blocks: a button registers a click region if `onClick`
    // is set and `enabled`, styled consistently across every overlay.
    sf::FloatRect uiPanelBg(sf::RenderWindow& window, sf::Vector2f pos, sf::Vector2f size);
    void uiButton(sf::RenderWindow& window, sf::Vector2f pos, sf::Vector2f size, const std::string& label,
        std::function<void()> onClick, bool enabled = true);
    void uiText(sf::RenderWindow& window, sf::Vector2f pos, const std::string& text, unsigned int size,
        sf::Color color = sf::Color::White, bool bold = false);
    // Substitutes {MOVE}/{E}/{U}/{M}/{F} placeholders in help text (hud_help/
    // tutorial_body/howtoplay_body) with whatever settings_.keys actually
    // binds right now, so rebinding a key doesn't leave the on-screen
    // instructions pointing at the wrong one.
    std::string applyKeyPlaceholders(const std::string& text) const;

    void drawOverlayRoot(sf::RenderWindow& window); // dims background + dispatches to the open overlay
    void drawBusinessesOverlay(sf::RenderWindow& window);
    void drawTreeOverlay(sf::RenderWindow& window);
    void drawMarketOverlay(sf::RenderWindow& window);
    void drawStaffOverlay(sf::RenderWindow& window);
    void drawSleepOverlay(sf::RenderWindow& window);
    void drawEatOverlay(sf::RenderWindow& window);
    void drawDoctorOverlay(sf::RenderWindow& window);
    void drawFastForwardOverlay(sf::RenderWindow& window);
    void drawAchievementsOverlay(sf::RenderWindow& window);
    void drawDeathNoticeOverlay(sf::RenderWindow& window);
    void drawLegacyOverlay(sf::RenderWindow& window);
    void drawDialogueOverlay(sf::RenderWindow& window);
    void drawBankOverlay(sf::RenderWindow& window);
    void drawWarehouseOverlay(sf::RenderWindow& window);
    void drawContractsOverlay(sf::RenderWindow& window);
    void drawPauseOverlay(sf::RenderWindow& window);
    void drawSettingsOverlay(sf::RenderWindow& window);
    void drawHowToPlayOverlay(sf::RenderWindow& window);
    void drawCropPickerOverlay(sf::RenderWindow& window);
    void drawTimingMinigameOverlay(sf::RenderWindow& window);
    void drawChoppingOverlay(sf::RenderWindow& window);

    // Attempts to start the timing-bar minigame for `businessId` ("fishing",
    // "mine", or "goldmine") -- checks it's actually built and its own
    // cooldown, and shows a toast instead of opening the overlay if not.
    void tryStartTimingMinigame(const std::string& businessId);
    // Reads the indicator's current position, resolves hit/miss via
    // Game::tryMinigameBonus, closes the overlay, sets that business's
    // cooldown, and shows the result via setFeedback. Shared by the
    // Space-key and the "Catch!"/"Mine!" button.
    void resolveTimingMinigame();

    // Attempts to start the Lumber Camp's chopping minigame (same checks as
    // tryStartTimingMinigame, its own cooldown/mechanic).
    void tryStartChopping();
    // Called every frame while OverlayKind::Chopping is open; auto-resolves
    // (same result path as a manual click reaching 0 clicksLeft would) once
    // choppingTimeLeft_ runs out.
    void updateChopping(float dt);
    void resolveChopping();

    // Attempts to start the Brewing minigame for `businessId` ("alchemist"
    // or "winery") -- same built/cooldown checks as the other minigame
    // starters.
    void tryStartBrewing(const std::string& businessId);
    void drawBrewingOverlay(sf::RenderWindow& window);
    // Called every frame while OverlayKind::Brewing is open, during the
    // memorize phase only (counts down brewShowTimer_, then flips to the
    // input phase).
    void updateBrewing(float dt);
    // A color button click during the input phase: advances brewStep_ on a
    // match, resolves as a miss immediately on a mismatch, resolves as a
    // hit once the whole sequence has been reproduced.
    void handleBrewClick(int colorIndex);
    void resolveBrewing(bool hit);

    // Per-frame Highlands foraging upkeep: respawns collected spots whose
    // timer has elapsed. Collection itself happens inline in the E-key
    // handler (mirrors how NPC talk / building interact already work).
    void updateForaging(float dt);
    void drawForageable(sf::RenderWindow& window, const Forageable& f);
};
