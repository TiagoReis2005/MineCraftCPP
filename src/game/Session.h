#pragma once

#include "core/GameTime.h"
#include "core/Weather.h"
#include "game/Drops.h"
#include "game/Inventory.h"
#include "game/Mining.h"
#include "game/Player.h"
#include "world/Block.h"
#include "world/Raycast.h"
#include "world/World.h"
#include "world/WorldSave.h"

#include <memory>
#include <string>
#include <vector>

struct GLFWwindow;

namespace mc {

class BlockRegistry;
class Renderer;
class VkContext;
struct Settings;

// Everything that exists ONLY while playing a world: the world and its save, the
// player, inventory, ground drops, break progress, the clock and the weather. The app
// creates a Session when a world is entered and destroys it on Save & Quit — the title
// screen simply has no Session, so nothing can tick, take input, or render there.
//
// Future gameplay systems live here, not in main: entities/mobs, equipment slots
// (armor, offhand, pouch/shulker), open-container state (chest/furnace/crafting).
class Session {
public:
    // Player tuning sourced from app-level data (the Bedrock camera preset JSONs).
    struct PlayerTuning {
        float thirdPersonRadius = 4.0f;
        float thirdPersonMinDist = 0.3f;
        bool invertFrontLookX = false;
    };

    // Per-frame facts from the app about who owns the input devices.
    struct FrameInput {
        bool inventoryOpen = false; // screen owns mouse/keys; simulation still runs
        double scroll = 0.0;        // wheel notches for the hotbar / spectator bar
        double now = 0.0;           // glfwGetTime (weather shader clock)
    };

    // Loads (or begins) the world at saves/<dir>: reads level.txt, boots the streaming
    // world with its seed, restores player/inventory/cracks/drops.
    void start(BlockRegistry& registry, const std::string& saveDir, const Settings& settings,
               const PlayerTuning& tuning);

    // One gameplay step: player physics + input, block interaction, mining cracks,
    // clock/weather, chunk streaming, drops — and feeds the renderer's per-frame world
    // state (cracks, drops, sky, weather). The app skips this call entirely while the
    // pause menu is open, which freezes the whole session for free.
    void update(VkContext& ctx, GLFWwindow* window, Renderer& renderer, float dt,
                const FrameInput& in);

    // Persist level.txt + still-loaded edited chunks (autosave and quit paths).
    void save();

    // Save, then tear the world down (waits for the GPU, joins the worker threads).
    // The Session must not be used afterwards; the app resets its pointer.
    void destroy(VkContext& ctx);

    // Live options changes (FOV / sensitivity / render distance).
    void applySettings(const Settings& s);

    // Throw a stack out of the player's face (inventory-screen Q drops).
    void throwStack(const ItemStack& s);

    // Ignore mouse break/place until both buttons are released once. Call on every
    // cursor recapture so the click that closed a menu/inventory doesn't bleed into the
    // world (e.g. "Back to Game" was eating a block in creative).
    void ignoreMouseUntilRelease() { swallowMouse_ = true; }

    // Right-clicking a container block (crafting table, later furnace/chest) queues a
    // request; the app consumes it and opens the matching screen.
    struct ContainerRequest {
        uint8_t kind = kContainerNone; // kContainer*
        glm::ivec3 cell{0};            // the block that was opened (chest storage later)
    };
    ContainerRequest takeContainerRequest() {
        ContainerRequest r = containerReq_;
        containerReq_ = {};
        return r;
    }

    // Selection outline of the targeted block, rebuilt every update (empty when none).
    const std::vector<AABB>& outlineBoxes() const { return outline_; }
    // Last raycast result (debug overlay target info).
    const RaycastHit& targetHit() const { return hit_; }
    const WorldSave::Level& level() const { return level_; }

    // Public on purpose (codebase style): the app's UI and render code read these, and
    // gameplay systems mutate them directly.
    World world;
    Player player;
    Inventory inv;
    Drops drops;
    Mining mining;
    GameTime gameTime;
    Weather weather;
    float spectatorHudTimer = 0.0f; // spectator hotbar reveal after scrolling

    // Minecraft's clock: 20 ticks/second, 24000 ticks/day. gametime_ is the monotonic
    // total-ticks counter (saved) that drives cloud drift (and future redstone/random
    // ticks); it never wraps, unlike the day/night time-of-day in GameTime.
    static constexpr int kTicksPerSecond = 20;
    static constexpr float kTickDuration = 1.0f / kTicksPerSecond;
    uint64_t gametime() const { return gametime_; }

private:
    BlockRegistry* reg_ = nullptr;
    std::unique_ptr<WorldSave> save_;
    WorldSave::Level level_;
    GameMode spawnMode_ = GameMode::Creative; // applied when the spawn scan lands
    bool spawnPlaced_ = false;
    double tickAccum_ = 0.0;   // fixed 20 TPS accumulator for the world simulation
    uint64_t gametime_ = 0;    // total ticks this world has run (saved; drives clouds)
    void tickWorld(GLFWwindow* window); // one fixed 50 ms world-simulation step
    float autosaveTimer_ = 0.0f;
    float breakCooldown_ = 0.0f, placeCooldown_ = 0.0f;
    float qDropCooldown_ = 0.0f;
    bool leftPrev_ = false, rightPrev_ = false, qPrev_ = false;
    bool swallowMouse_ = false; // block break/place until the mouse is released post-recapture
    RaycastHit hit_{};
    std::vector<AABB> outline_;
    ContainerRequest containerReq_{};

    void captureLevel();                       // game state -> level_ (by block names)
    void applyLevel(const Settings& settings); // level_ -> game state
    void interact(GLFWwindow* window, float dt, bool acceptInput);
};

} // namespace mc
