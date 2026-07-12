#include "game/Session.h"

#include "core/Settings.h"
#include "gfx/Renderer.h"
#include "gfx/VkContext.h"
#include "world/BlockRegistry.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdio>

namespace mc {
namespace {

// Delay between repeated break/place actions while a mouse button is held.
constexpr float kActionRepeat = 0.2f;

} // namespace

void Session::start(BlockRegistry& registry, const std::string& saveDir,
                    const Settings& settings, const PlayerTuning& tuning) {
    reg_ = &registry;
    save_ = std::make_unique<WorldSave>(saveDir);
    level_ = WorldSave::Level{};
    save_->loadLevel(level_);

    world.init(&registry, level_.seed);
    world.setSave(save_.get());
    // Blocks the world pops (support removed, door halves) drop as items.
    world.onBlockPopped = [this](const glm::ivec3& cell, BlockState s) {
        drops.spawnFromBlock(*reg_, reg_->block(s), s, cell);
    };

    player.thirdPersonRadius = tuning.thirdPersonRadius;
    player.thirdPersonMinDist = tuning.thirdPersonMinDist;
    player.invertFrontLookX = tuning.invertFrontLookX;
    applySettings(settings);
    player.camera.fovDeg = settings.fovDeg;

    weather.seed(level_.seed); // same seed -> same weather sequence (applyLevel restores state)
    applyLevel(settings);
    std::fprintf(stderr, "[World] entered '%s' (%s), seed %u\n", level_.name.c_str(),
                 saveDir.c_str(), level_.seed);
}

void Session::tickWorld(GLFWwindow* window) {
    player.tick(window, world); // one 20 TPS physics step (interpolated for rendering)

    // Ground item entities tick right after the player (uses the authoritative eye, not the
    // interpolated one, for the magnet target).
    glm::vec3 feet = player.camera.position - glm::vec3(0.0f, 1.62f, 0.0f);
    drops.tick(world, feet, inv, /*collect=*/player.mode != GameMode::Spectator);

    ++gametime_; // monotonic total-ticks counter (drives clouds; saved)
    gameTime.advance(kTickDuration);
    weather.advance(kTickDuration);
    // Future fixed-tick systems hang here: mob AI/physics and block + random ticks (crop
    // growth, redstone, fluids, furnaces).
}

void Session::applySettings(const Settings& s) {
    player.baseFov = s.fovDeg;
    player.camera.lookSensitivity = s.sensitivity;
    world.renderDistance = s.renderDistance;
    world.lodDistance = s.lodDistance;
    // Far clip must reach past the farthest LOD tile (or real chunks) plus a margin for
    // altitude, so distant terrain/clouds aren't sliced off by the far plane.
    int viewChunks = std::max(s.renderDistance, s.lodDistance);
    player.camera.farPlane = std::max(1000.0f, (viewChunks + 3) * 32.0f);
    // Input options (opt cycles: index 0 = the first listed value).
    player.setInputOptions(/*invertMouse=*/s.optGet("invertMouse", 0.0f) > 0.5f,
                           /*sneakToggle=*/s.optGet("sneakMode", 0.0f) > 0.5f,
                           /*sprintToggle=*/s.optGet("sprintMode", 0.0f) > 0.5f,
                           /*autoJump=*/s.optGet("autoJump", 0.0f) > 0.5f);
}

void Session::applyLevel(const Settings&) {
    gameTime.setSeconds(level_.timeSeconds >= 0.0 ? level_.timeSeconds
                                                  : GameTime::freshWorldSeconds());
    gametime_ = level_.gametime;
    tickAccum_ = 0.0;
    weather.setState(level_.raining, level_.rainTimer, level_.rainIntensity);
    inv = Inventory{};
    inv.selected = std::clamp(level_.selected, 0, 8);
    auto stack = [&](const WorldSave::ItemLine& l) -> ItemStack {
        if (l.name.empty() || l.count <= 0) return {};
        BlockId id = reg_->byName(l.name);
        if (id == BLOCK_AIR) return {}; // block gone from the registry
        return {id, std::min(l.count, Inventory::kMaxStack)};
    };
    for (int i = 0; i < 9; ++i) inv.slots[static_cast<size_t>(i)] = stack(level_.hotbar[i]);
    for (int i = 0; i < 36; ++i) inv.main[static_cast<size_t>(i)] = stack(level_.main[i]);

    std::vector<Mining::Crack> cracks;
    for (const WorldSave::CrackLine& c : level_.cracks) {
        BlockId id = reg_->byName(c.block);
        if (id != BLOCK_AIR) {
            // Saves don't store the dig aim; default to the cell center.
            cracks.push_back({c.cell, BlockState(id, c.data), c.progress,
                              glm::vec3(c.cell) + 0.5f, glm::ivec3(0)});
        }
    }
    mining.setCracks(std::move(cracks));

    std::vector<Drops::Item> groundItems;
    for (const WorldSave::DropLine& d : level_.drops) {
        BlockId id = reg_->byName(d.block);
        if (id == BLOCK_AIR) continue;
        Drops::Item it;
        it.id = id;
        it.count = std::min(d.count, Inventory::kMaxStack);
        it.pos = d.pos;
        it.vel = d.vel;
        it.age = d.age;
        groundItems.push_back(it);
    }
    drops.setItems(std::move(groundItems));

    spawnMode_ = static_cast<GameMode>(std::clamp(level_.mode, 0, 2));
    if (level_.hasPlayer) {
        player.mode = spawnMode_;
        player.flying = level_.flying;
        player.camera.position = level_.playerPos;
        player.camera.yaw = level_.yaw;
        player.camera.pitch = level_.pitch;
        spawnPlaced_ = true;
    } else {
        // Fresh world: hover (creative) until the spawn column streams in, then the
        // spawn scan sets the feet down and applies spawnMode_.
        player.mode = GameMode::Creative;
        player.flying = true;
        player.camera.position = glm::vec3(0.5f, 100.0f, 0.5f);
        player.camera.yaw = -90.0f;
        player.camera.pitch = -20.0f;
        spawnPlaced_ = false;
    }
    player.resetInterp(); // snap the render eye to the spawn/load position (no interp slide)
}

void Session::captureLevel() {
    level_.timeSeconds = gameTime.seconds();
    level_.gametime = gametime_;
    level_.raining = weather.raining();
    level_.rainTimer = weather.timer();
    level_.rainIntensity = weather.intensity();
    level_.mode = static_cast<int>(player.mode);
    level_.flying = player.flying;
    level_.hasPlayer = true;
    level_.playerPos = player.camera.position;
    level_.yaw = player.camera.yaw;
    level_.pitch = player.camera.pitch;
    level_.selected = inv.selected;
    auto item = [&](const ItemStack& s) {
        return s.empty() ? WorldSave::ItemLine{}
                         : WorldSave::ItemLine{reg_->block(BlockState(s.id)).name(), s.count};
    };
    for (int i = 0; i < 9; ++i) level_.hotbar[i] = item(inv.slots[static_cast<size_t>(i)]);
    for (int i = 0; i < 36; ++i) level_.main[i] = item(inv.main[static_cast<size_t>(i)]);
    level_.cracks.clear();
    for (const Mining::Crack& c : mining.cracks()) {
        level_.cracks.push_back({c.cell, reg_->block(c.state).name(), c.state.data(), c.progress});
    }
    level_.drops.clear();
    for (const Drops::Item& d : drops.items()) {
        level_.drops.push_back({d.pos, d.vel, reg_->block(BlockState(d.id)).name(), d.count,
                                d.age});
    }
}

void Session::save() {
    if (!save_) return;
    captureLevel();
    save_->saveLevel(level_);
    world.flushEdited();
}

void Session::destroy(VkContext& ctx) {
    save();
    vkDeviceWaitIdle(ctx.device); // frames in flight still reference chunk meshes
    world.destroy(ctx);           // joins the worker threads; save_ unused afterwards
    save_.reset();
}

void Session::throwStack(const ItemStack& s) {
    drops.spawnThrown(s.id, s.count, player.camera.renderEye(), player.camera.front(),
                      player.velocity());
}

void Session::update(VkContext& ctx, GLFWwindow* window, Renderer& renderer, float dt,
                     const FrameInput& in) {
    bool acceptInput = !in.inventoryOpen;
    player.handleInput(window, acceptInput);

    // Fixed 20 TPS tick: player physics + world simulation, deterministic and pausable.
    // Runs before targeting/interaction so the raycast uses this frame's motion. Clamp a
    // big frame gap to avoid a spiral of death. partialTick smooths rendering between ticks.
    tickAccum_ += dt;
    if (tickAccum_ > 0.25) tickAccum_ = 0.25;
    while (tickAccum_ >= kTickDuration) {
        tickWorld(window);
        tickAccum_ -= kTickDuration;
    }
    float partialTick = static_cast<float>(tickAccum_ / kTickDuration);
    player.updateVisual(window, world, dt, partialTick);

    // Hotbar: number keys pick a slot; the wheel cycles. The inventory screen owns
    // these while it is open (1-9 assign the hovered block there instead).
    int prevSelected = inv.selected;
    if (acceptInput) {
        for (int i = 0; i < Inventory::kSlots; ++i) {
            if (glfwGetKey(window, GLFW_KEY_1 + i) == GLFW_PRESS) inv.selected = i;
        }
        if (in.scroll != 0.0) {
            int dir = in.scroll > 0.0 ? -1 : 1; // scroll up -> previous slot
            inv.selected = (inv.selected + dir + Inventory::kSlots) % Inventory::kSlots;
        }
    }

    // Q throws from the selected stack (shift: the whole stack); holding Q keeps
    // spewing. While the inventory is open the screen handles Q itself.
    bool qNow = glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS;
    bool qEdge = qNow && !qPrev_;
    qPrev_ = qNow;
    bool shiftNow = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                    glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
    qDropCooldown_ -= dt;
    if (qNow && acceptInput && player.canEdit() && (qEdge || qDropCooldown_ <= 0.0f)) {
        ItemStack& sel = inv.slots[static_cast<size_t>(inv.selected)];
        if (!sel.empty()) {
            int n = shiftNow ? sel.count : 1;
            drops.spawnThrown(sel.id, n, player.camera.renderEye(), player.camera.front(),
                              player.velocity());
            sel.count -= n;
            if (sel.count <= 0) sel = {};
            qDropCooldown_ = 0.12f;
        }
    }
    if (!qNow) qDropCooldown_ = 0.0f;

    bool spectator = player.mode == GameMode::Spectator;
    if (spectator && in.scroll != 0.0) spectatorHudTimer = 3.0f; // reveal spectator bar
    spectatorHudTimer = spectatorHudTimer > dt ? spectatorHudTimer - dt : 0.0f;
    if (inv.selected != prevSelected) {
        std::fprintf(stderr, "[Hotbar] %s\n", reg_->block(inv.selectedBlock()).name().c_str());
    }

    // Block targeting: raycast + the selection outline (edges thickened into thin bars).
    hit_ = raycastVoxel(world, player.camera.renderEye(), player.camera.front(), 6.0f);
    outline_.clear();
    if (hit_.hit && !spectator && acceptInput) {
        BlockState st = world.getState(hit_.block.x, hit_.block.y, hit_.block.z);
        glm::vec3 cell(hit_.block);
        constexpr float kBarHalf = 1.0f / 128.0f;
        for (const Segment& e :
             reg_->block(st).aimedOutlineEdges(st, &world, hit_.block, hit_.point, hit_.normal)) {
            AABB bar{glm::min(e.a, e.b) + cell, glm::max(e.a, e.b) + cell};
            for (int axis = 0; axis < 3; ++axis) {
                if (bar.max[axis] - bar.min[axis] < 1e-5f) {
                    bar.min[axis] -= kBarHalf;
                    bar.max[axis] += kBarHalf;
                }
            }
            outline_.push_back(bar);
        }
    }

    interact(window, dt, acceptInput);

    // Heal cracks that aren't being dug and hand every partially-broken block to the
    // renderer at its own stage. Each aim-aware crackBoxes box (visual silhouette;
    // a double slab's aimed half) becomes a screen-space decal volume.
    mining.update(world, dt);
    std::vector<Renderer::Crack> crackDraws;
    for (const Mining::Crack& c : mining.cracks()) {
        const Block& b = reg_->block(c.state);
        float need = b.breakSeconds(ToolType::None);
        if (need <= 0.0f) continue;
        int stage = std::min(9, static_cast<int>(c.progress / need * 10.0f));
        std::vector<AABB> boxes = b.crackBoxes(c.state, &world, c.cell, c.hitPoint, c.normal);
        if (boxes.empty()) boxes.push_back(AABB{}); // full cell fallback
        glm::vec3 cell(c.cell);
        for (const AABB& bb : boxes) {
            crackDraws.push_back({AABB{bb.min + cell, bb.max + cell}, stage});
        }
    }
    renderer.setCracks(std::move(crackDraws));

    // Day/night + weather (already advanced by the tick above) drive the sky color, the
    // celestial pass and the rain/clouds.
    float wet = weather.intensity();
    renderer.setSkyColor(gameTime.skyColor() * (1.0f - 0.55f * wet));
    float starAlpha = glm::clamp(-gameTime.sunHeight() * 3.0f, 0.0f, 1.0f) * (1.0f - wet);
    renderer.setCelestial(gameTime.sunAngle(), (gameTime.dayCount() - 1) % 8, starAlpha,
                          1.0f - 0.75f * wet);
    float cloudB = glm::clamp(0.18f + 0.82f * glm::max(0.0f, gameTime.sunHeight() * 2.0f),
                              0.18f, 1.0f) * (1.0f - 0.35f * wet);
    // Clouds drift on GAME time (total ticks + partial), not wall-clock, so they are seed-
    // consistent, persist across reload, and pause with the game.
    float cloudTime =
        static_cast<float>((static_cast<double>(gametime_) + partialTick) / kTicksPerSecond);
    renderer.setWeather(wet, cloudTime, cloudB);

    // Autosave every 60s of play (level.txt + edited chunks). Paused frames never get
    // here, so the timer only counts real play time.
    autosaveTimer_ += dt;
    if (autosaveTimer_ >= 60.0f) {
        save();
        autosaveTimer_ = 0.0f;
    }

    // Stream/generate/mesh chunks around the player.
    world.update(ctx, player.camera.position);

    // Dropped items: physics/magnet/pickup ran on the tick above; here just build this
    // frame's draws, interpolated between ticks so they move and bob smoothly.
    {
        std::vector<Drops::Draw> dropDraws;
        drops.buildDraws(dropDraws, partialTick);
        renderer.setDropDraws(std::move(dropDraws));
    }

    // Spawn on the ground: fresh worlds hover while the spawn column streams in; once
    // it has terrain, set the feet on the surface and apply the world's game mode.
    if (!spawnPlaced_) {
        for (int y = 255; y >= 0; --y) {
            if (world.getBlock(0, y, 0) != BLOCK_AIR) {
                player.camera.position = glm::vec3(0.5f, y + 1.0f + 1.62f, 0.5f);
                player.flying = false;
                player.mode = spawnMode_;
                player.resetInterp(); // land the render eye on the surface without an interp slide
                spawnPlaced_ = true;
                break;
            }
        }
    }
}

void Session::interact(GLFWwindow* window, float dt, bool acceptInput) {
    bool leftNow = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    bool rightNow = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;

    // A click that came from a UI screen (menu/inventory) must not bleed into the world:
    // after a recapture, do nothing until both buttons are released once.
    if (swallowMouse_) {
        if (leftNow || rightNow) {
            leftPrev_ = leftNow;
            rightPrev_ = rightNow;
            return;
        }
        swallowMouse_ = false;
    }

    bool leftEdge = leftNow && !leftPrev_;
    bool rightEdge = rightNow && !rightPrev_;
    breakCooldown_ -= dt;
    placeCooldown_ -= dt;

    BlockId selectedBlock = inv.selectedBlock();
    if (acceptInput && player.canEdit() && hit_.hit) {
        BlockState hitState = world.getState(hit_.block.x, hit_.block.y, hit_.block.z);
        const Block& hitBlock = reg_->block(hitState);

        // ---- Breaking: instant in creative; timed by strength/tool in survival.
        if (leftNow) {
            bool doBreak = false;
            if (player.mode == GameMode::Creative) {
                if (breakCooldown_ <= 0.0f) {
                    doBreak = true;
                    breakCooldown_ = kActionRepeat;
                }
            } else {
                float need = hitBlock.breakSeconds(ToolType::None); // bare hands
                if (need > 0.0f) {
                    doBreak = mining.dig(hit_.block, hitState, need, dt, hit_.point, hit_.normal,
                                         hitBlock.aimedPart(hitState, hit_.block, hit_.point,
                                                            hit_.normal));
                } else if (need == 0.0f) {
                    doBreak = true; // instant-break blocks (plants, torches...)
                }
                // Keep swinging while mining.
                if (player.swingProgress() >= 1.0f) player.triggerSwing();
            }
            if (doBreak) {
                BlockState after = hitBlock.breakResult(hitState, hit_.point, hit_.block,
                                                        hit_.normal);
                world.setState(hit_.block.x, hit_.block.y, hit_.block.z, after);
                hitBlock.onBroken(world, hit_.block, hitState);
                if (player.mode == GameMode::Survival) {
                    drops.spawnFromBlock(*reg_, hitBlock, hitState, hit_.block);
                }
                player.triggerSwing();
            }
        }

        // ---- Using / placing.
        if (rightNow && placeCooldown_ <= 0.0f) {
            bool consumed = false;
            // Container blocks open their screen (crafting table; furnace/chest once
            // those exist) unless sneaking — sneaking places against them instead.
            if (!player.crouching() && hitBlock.props().container != kContainerNone) {
                containerReq_ = {hitBlock.props().container, hit_.block};
                consumed = true;
                placeCooldown_ = kActionRepeat;
                player.triggerSwing();
            }
            // Interactive blocks next (doors, gates), same sneak rule.
            if (!consumed && !player.crouching() &&
                hitBlock.onUse(world, hit_.block, hitState)) {
                consumed = true;
                placeCooldown_ = kActionRepeat;
                player.triggerSwing();
            }
            if (!consumed && selectedBlock != BLOCK_AIR) {
                const Block& sel = reg_->block(selectedBlock);
                PlacementCtx pctx;
                pctx.cell = hit_.block + hit_.normal;
                pctx.clickedCell = hit_.block;
                pctx.normal = hit_.normal;
                pctx.hitPoint = hit_.point;
                pctx.playerYawDeg = player.camera.yaw;
                pctx.sneaking = player.crouching();
                pctx.world = &world;

                BlockState merged;
                if (sel.tryMergeInto(*reg_, hitState, pctx, &merged)) {
                    // Merge into the clicked cell (double slabs, snow layers).
                    if (!player.collidesWithBlock(world, *reg_, merged, hit_.block)) {
                        world.setState(hit_.block.x, hit_.block.y, hit_.block.z, merged);
                        if (player.mode == GameMode::Survival) inv.consumeSelected();
                        placeCooldown_ = kActionRepeat;
                        player.triggerSwing();
                    }
                    consumed = true;
                }
                if (!consumed) {
                    glm::ivec3 p = pctx.cell;
                    BlockState st = sel.placementState(pctx);
                    BlockState existing = world.getState(p.x, p.y, p.z);
                    bool free = reg_->block(existing).props().replaceable;
                    BlockState combined;
                    if (!free && sel.tryCombineAt(*reg_, existing, pctx, &combined)) {
                        // Target cell already holds a compatible partial block (aimed
                        // through a slab's empty half): fill it instead.
                        if (!player.collidesWithBlock(world, *reg_, combined, p)) {
                            world.setState(p.x, p.y, p.z, combined);
                            if (player.mode == GameMode::Survival) inv.consumeSelected();
                            placeCooldown_ = kActionRepeat;
                            player.triggerSwing();
                        }
                    } else if (free && sel.canPlaceAt(world, p, st) &&
                               !player.collidesWithBlock(world, *reg_, st, p)) {
                        if (world.setState(p.x, p.y, p.z, st)) {
                            sel.onPlaced(world, p, st);
                            if (player.mode == GameMode::Survival) inv.consumeSelected();
                            placeCooldown_ = kActionRepeat;
                            player.triggerSwing();
                        }
                    }
                }
            }
        }
    }
    // Swing on the initial click even when hitting nothing (like MC's whiff swing).
    if (acceptInput && (leftEdge || rightEdge)) player.triggerSwing();
    leftPrev_ = leftNow;
    rightPrev_ = rightNow;
    if (!leftNow) breakCooldown_ = 0.0f;
    if (!rightNow) placeCooldown_ = 0.0f;
}

} // namespace mc
