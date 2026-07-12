// App shell: window, Vulkan, renderers, UI, menus and settings — everything that lives
// for the whole run. The GAME (world, player, inventory, drops, time, weather) lives in
// game/Session and only exists while a world is open: `session == nullptr` IS the title
// screen, so no gameplay state can tick, take input or render there by construction.
#include "anim/AnimClip.h"
#include "anim/AnimGraph.h"
#include "anim/Blackboard.h"
#include "anim/Blend1D.h"
#include "anim/Json.h"
#include "anim/PlayerAnimator.h"
#include "anim/Pose.h"
#include "anim/Rig.h"
#include "core/FileDialog.h"
#include "core/KeyBinds.h"
#include "core/Lang.h"
#include "core/Paths.h"
#include "core/Settings.h"
#include "core/SystemStats.h"
#include "core/Window.h"
#include "game/PlayerModel.h"
#include "game/PlayerSkin.h"
#include "game/Recipes.h"
#include "game/Session.h"
#include "gfx/HeldItemRenderer.h"
#include "gfx/ModelRenderer.h"
#include "gfx/PanoramaRenderer.h"
#include "gfx/PlayerPreview.h"
#include "gfx/Renderer.h"
#include "gfx/SkyRenderer.h"
#include "gfx/Swapchain.h"
#include "gfx/TextureArray.h"
#include "gfx/UIRenderer.h"
#include "gfx/VkContext.h"
#include "gfx/WeatherRenderer.h"
#include "ui/DebugOverlay.h"
#include "ui/Font.h"
#include "ui/Hud.h"
#include "ui/InventoryScreen.h"
#include "ui/ItemIconRenderer.h"
#include "ui/MenuScreen.h"
#include "world/BlockRegistry.h"

#include <GLFW/glfw3.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

int main() {
    try {
        // Early options peek so the window opens at the persisted size/mode (the full
        // settings load with defaults happens after the camera presets, as before).
        mc::Settings boot;
        boot.load(mc::resolve("options.txt"));
        mc::Window window;
        window.init(boot.windowW, boot.windowH, "MineCraftCPP");
        if (boot.fullscreen) window.setFullscreen(true);

        mc::VkContext ctx;
        ctx.init(window.handle());

        int width = 0, height = 0;
        window.framebufferSize(width, height);
        mc::Swapchain swapchain;
        swapchain.init(&ctx, static_cast<uint32_t>(width), static_cast<uint32_t>(height));

        mc::BlockRegistry registry;
        registry.registerDefaults();
        mc::TextureArray textures;
        std::vector<std::string> texNames = registry.textureNames();
        for (int i = 0; i < 10; ++i) { // mining crack overlay stages
            texNames.push_back("destroy_stage_" + std::to_string(i));
        }
        textures.build(ctx, texNames, mc::resolve("assets/textures/blocks"));
        registry.resolveTextures(textures);

        // Per-block average colors used to tint the far low-detail (LOD) terrain: top
        // face for the surface, side face for cliff skirts. Built once (registry +
        // textures are fixed) and applied to each world's LOD field on enter.
        std::vector<glm::vec3> lodTopColors(registry.count()), lodSideColors(registry.count());
        for (uint16_t id = 0; id < registry.count(); ++id) {
            const auto& fl = registry.block(static_cast<mc::BlockId>(id)).faceLayers();
            lodTopColors[id] = textures.averageColor(fl[mc::FACE_PY]);
            lodSideColors[id] = textures.averageColor(fl[mc::FACE_PX]);
        }
        // Gui/hand orientation per block from the model jsons' "display" tables.
        registry.loadDisplayTransforms(mc::resolve("assets/models/block"));

        // Crafting recipes (2x2 inventory grid + 3x3 crafting table).
        mc::RecipeBook recipes;
        recipes.registerDefaults(registry);

        // Camera tuning from the Bedrock camera presets in assets/cameras/ (FOV, orbit
        // radius, wall-avoidance minimum, front-view input inversion). Missing files
        // just keep the built-in defaults; each new session's player gets these.
        mc::Session::PlayerTuning tuning;
        float presetFov = 70.0f;
        {
            auto component = [](const mc::JsonValue& root, const char* name) -> const mc::JsonValue* {
                const mc::JsonValue* ent = root.find("minecraft:camera_entity");
                const mc::JsonValue* comps = ent ? ent->find("components") : nullptr;
                return comps ? comps->find(name) : nullptr;
            };
            try {
                mc::JsonValue j = mc::parseJsonFile(mc::resolve("assets/cameras/first_person.json"));
                if (const mc::JsonValue* cam = component(j, "minecraft:camera")) {
                    if (const mc::JsonValue* fov = cam->find("field_of_view"); fov && fov->isNumber()) {
                        presetFov = static_cast<float>(fov->number);
                    }
                }
            } catch (const std::exception&) {}
            try {
                mc::JsonValue j = mc::parseJsonFile(mc::resolve("assets/cameras/third_person.json"));
                if (const mc::JsonValue* orbit = component(j, "minecraft:camera_orbit")) {
                    if (const mc::JsonValue* r = orbit->find("radius"); r && r->isNumber()) {
                        tuning.thirdPersonRadius = static_cast<float>(r->number);
                    }
                }
                if (const mc::JsonValue* avoid = component(j, "minecraft:camera_avoidance")) {
                    if (const mc::JsonValue* m = avoid->find("distance_constraint_min"); m && m->isNumber()) {
                        tuning.thirdPersonMinDist = static_cast<float>(m->number);
                    }
                }
            } catch (const std::exception&) {}
            try {
                mc::JsonValue j = mc::parseJsonFile(mc::resolve("assets/cameras/third_person_front.json"));
                if (const mc::JsonValue* orbit = component(j, "minecraft:camera_orbit")) {
                    if (const mc::JsonValue* inv = orbit->find("invert_x_input")) {
                        tuning.invertFrontLookX =
                            inv->type == mc::JsonValue::Type::Bool && inv->boolean;
                    }
                }
            } catch (const std::exception&) {}
            std::fprintf(stderr, "[Camera] fov %.0f, orbit radius %.1f, min dist %.2f, front invert-x %s\n",
                         presetFov, tuning.thirdPersonRadius, tuning.thirdPersonMinDist,
                         tuning.invertFrontLookX ? "on" : "off");
        }

        // User options: defaults come from the camera preset / engine, then options.txt
        // (written by the options menu) overrides; applied live while the menu is open.
        const std::string optionsPath = mc::resolve("options.txt");
        mc::Settings settings;
        settings.fovDeg = presetFov;
        settings.load(optionsPath);

        // Localization: settings.language holds the code (en_us); drop vanilla-format
        // lang JSONs in assets/lang/ and tr() picks them up (English fallbacks built in).
        mc::Lang lang;
        lang.load(mc::resolve("assets/lang"), settings.language);

        // Rebindable key bindings (Key Binds screen); overlay saved codes onto defaults.
        const std::string keyBindsPath = mc::resolve("keybinds.txt");
        mc::KeyBinds keyBinds;
        keyBinds.load(keyBindsPath);

        // Pick the player's skin + body type; the remembered skin (options.txt) wins,
        // and whatever got picked is remembered for next launch.
        mc::PlayerSkin playerSkin =
            mc::pickPlayerSkin(mc::resolve("assets/textures/entity/player"), settings.skin);
        settings.skin = playerSkin.name;

        mc::Renderer renderer;
        renderer.init(&ctx, &swapchain, &window, &textures);

        // Player body model (rendered in 3rd person), using the picked skin. The
        // geometry comes from assets/models/entity/player.json; the Skin Customization
        // toggles pick which overlay layers are meshed (cycle options store the index:
        // 0 = ON, 1 = OFF).
        auto skinLayerMask = [&settings]() -> uint32_t {
            uint32_t m = 0;
            if (settings.optGet("skinHat", 0.0f) < 0.5f) m |= mc::SL_Hat;
            if (settings.optGet("skinJacket", 0.0f) < 0.5f) m |= mc::SL_Jacket;
            if (settings.optGet("skinLSleeve", 0.0f) < 0.5f) m |= mc::SL_LeftSleeve;
            if (settings.optGet("skinRSleeve", 0.0f) < 0.5f) m |= mc::SL_RightSleeve;
            if (settings.optGet("skinLPants", 0.0f) < 0.5f) m |= mc::SL_LeftPants;
            if (settings.optGet("skinRPants", 0.0f) < 0.5f) m |= mc::SL_RightPants;
            return m;
        };
        uint32_t modelLayerMask = skinLayerMask();
        // The player skeleton (bone hierarchy + pivots + locators) drives both meshing
        // and posing; the meshes are one entry per bone so limbs bend at their joints.
        mc::Rig playerRig;
        // Bone indices used by the pose assembly / held item / first-person mask below.
        int biHead = -1, biHat = -1, biRightArm = -1, biRightItem = -1;
        int fpChain[5]{};  // right-arm chain (+ item anchor) drawn rigidly in first person
        int biBend[4]{};   // FUTURE(ik): F6 debug elbow/knee flex
        // (Re)load the rig for the current model (steve/alex) and refresh those indices.
        // Called at startup and whenever the skin's arm width flips.
        auto reloadRig = [&](bool slim) {
            playerRig.loadFromGeometry(mc::resolve("assets/models/entity/player.json"),
                                       slim ? "geometry.npc.alex" : "geometry.npc.steve");
            biHead = playerRig.findBone("head");
            biHat = playerRig.findBone("hat");
            biRightArm = playerRig.findBone("rightArm");
            biRightItem = playerRig.findBone("rightItem"); // held-item anchor (child of forearm)
            // First person poses the whole right-arm chain rigidly, incl. the item anchor.
            const char* chain[] = {"rightArm", "rightForearm", "rightSleeve", "rightForeSleeve",
                                   "rightItem"};
            for (int i = 0; i < 5; ++i) fpChain[i] = playerRig.findBone(chain[i]);
            const char* bend[] = {"leftForearm", "rightForearm", "leftShin", "rightShin"};
            for (int i = 0; i < 4; ++i) biBend[i] = playerRig.findBone(bend[i]);
        };
        reloadRig(playerSkin.slim);

        mc::ModelRenderer modelRenderer;
        mc::PlayerRigMeshes rigMeshes = mc::buildPlayerRigMeshes(playerRig, playerSkin.slim, modelLayerMask);
        modelRenderer.init(&ctx, &swapchain, renderer.cameraUBOs(), playerSkin.texturePath, rigMeshes);

        // Offscreen player preview (title screen + inventory player box).
        mc::PlayerPreview playerPreview;
        playerPreview.init(&ctx, &swapchain, playerSkin.texturePath, rigMeshes);
        renderer.setPlayerPreviewRenderer(&playerPreview);

        mc::PlayerAnimator animator;
        animator.load(mc::resolve("assets/animations/player.animation.json"));
        animator.load(mc::resolve("assets/animations/player_firstperson.animation.json"));

        // A4/A5: the player animation graph — a base locomotion layer (idle/walk/run blend
        // + jump/fall/land) plus a masked upper-body action layer (right-arm wave over the
        // running legs), Molang-guarded and cross-faded, authored in a graph JSON. Gameplay
        // writes params to the blackboard (P1). F7 toggles it against the vanilla stack.
        mc::AnimGraph playerGraph;
        playerGraph.load(mc::resolve("assets/anim/graphs/player.graph.json"), playerRig);
        mc::Blackboard blackboard;
        bool jumpAnimPrev = false;    // rising-edge detect the jump key for jump_pressed
        bool waveAnimPrev = false;    // rising-edge detect the wave key (H) for wave_pressed
        std::string prevAnimState;    // print base-layer state changes (state readout)

        mc::HeldItemRenderer heldRenderer;
        heldRenderer.init(&ctx, &swapchain, renderer.cameraUBOs(), &textures, &registry);

        // Sun, moon phases and stars riding the day/night cycle.
        mc::SkyRenderer skyRenderer;
        skyRenderer.init(&ctx, &swapchain, renderer.cameraUBOs(),
                         mc::resolve("assets/textures/environment"));
        renderer.setSkyRenderer(&skyRenderer);

        // Rain curtain for the weather cycle.
        mc::WeatherRenderer weatherRenderer;
        weatherRenderer.init(&ctx, &swapchain, renderer.cameraUBOs(),
                             mc::resolve("assets/textures/environment"));
        renderer.setWeatherRenderer(&weatherRenderer);

        // Title-screen panorama backdrop (vanilla gui/title/background/panorama_0..5).
        mc::PanoramaRenderer panoramaRenderer;
        panoramaRenderer.init(&ctx, &swapchain, mc::resolve("assets/textures/gui/title/background"));
        renderer.setPanoramaRenderer(&panoramaRenderer);

        mc::UIRenderer ui;
        ui.init(&ctx, &swapchain);
        mc::Hud hud;
        mc::HudTextures hudTex;
        hudTex.hotbar = ui.registerTexture(mc::resolve("assets/textures/gui/sprites/hud/hotbar.png"));
        hudTex.selection = ui.registerTexture(mc::resolve("assets/textures/gui/sprites/hud/hotbar_selection.png"));

        mc::Font font;
        font.init(ui, mc::resolve("assets/textures/font/ascii.png"),
                  mc::resolve("assets/textures/font/accented.png"));
        mc::DebugOverlay debug;

        // 3D block icons for the hotbar slots.
        mc::ItemIconRenderer iconRenderer;
        iconRenderer.init(&ctx, &textures, &registry, &ui);

        // The offscreen player preview, drawn by menus/inventories as a normal sprite.
        int playerPreviewTex = ui.registerImageView(playerPreview.view());

        // Creative inventory screen (E), drawn with the vanilla creative-tab texture.
        mc::InventoryScreen invScreen;
        {
            mc::InventoryScreen::Textures itex;
            itex.creativePanel = ui.registerTexture(
                mc::resolve("assets/textures/gui/container/creative_inventory/tab_items.png"));
            itex.creativeInventoryPanel = ui.registerTexture(
                mc::resolve("assets/textures/gui/container/creative_inventory/tab_inventory.png"));
            itex.survivalPanel =
                ui.registerTexture(mc::resolve("assets/textures/gui/container/inventory.png"));
            itex.craftingPanel =
                ui.registerTexture(mc::resolve("assets/textures/gui/container/crafting_table.png"));
            const std::string tabDir =
                mc::resolve("assets/textures/gui/sprites/container/creative_inventory/");
            for (int t = 0; t < mc::kTabCount; ++t) {
                std::string n = std::to_string(t + 1);
                itex.tabSelected[t] = ui.registerTexture(tabDir + "tab_top_selected_" + n + ".png");
                itex.tabUnselected[t] =
                    ui.registerTexture(tabDir + "tab_top_unselected_" + n + ".png");
            }
            itex.scroller = ui.registerTexture(tabDir + "scroller.png");
            itex.scrollerDisabled = ui.registerTexture(tabDir + "scroller_disabled.png");
            itex.playerPreview = playerPreviewTex;
            itex.tooltipBg =
                ui.registerTexture(mc::resolve("assets/textures/gui/sprites/tooltip/background.png"));
            itex.tooltipFrame =
                ui.registerTexture(mc::resolve("assets/textures/gui/sprites/tooltip/frame.png"));
            invScreen.init(itex, &recipes, &lang);
        }
        mc::InventoryScreen::Mode invMode = mc::InventoryScreen::Mode::Survival;

        // Title/pause/options menus, skinned with the vanilla widget sprites (missing
        // files fall back to flat quads).
        mc::MenuScreen menu;
        {
            const std::string wdir = mc::resolve("assets/textures/gui/sprites/widget/");
            const std::string idir = mc::resolve("assets/textures/gui/sprites/icon/");
            mc::MenuScreen::Textures mtex;
            mtex.button = ui.registerTexture(wdir + "button.png");
            mtex.buttonHighlighted = ui.registerTexture(wdir + "button_highlighted.png");
            mtex.buttonDisabled = ui.registerTexture(wdir + "button_disabled.png");
            mtex.slider = ui.registerTexture(wdir + "slider.png");
            mtex.sliderHandle = ui.registerTexture(wdir + "slider_handle.png");
            mtex.sliderHandleHighlighted =
                ui.registerTexture(wdir + "slider_handle_highlighted.png");
            mtex.textField = ui.registerTexture(wdir + "text_field.png");
            mtex.textFieldHighlighted = ui.registerTexture(wdir + "text_field_highlighted.png");
            mtex.iconFriends = ui.registerTexture(
                mc::resolve("assets/textures/gui/sprites/friends/friends.png"));
            mtex.iconLanguage = ui.registerTexture(idir + "language.png");
            mtex.iconAccessibility = ui.registerTexture(idir + "accessibility.png");
            const std::string pdir = mc::resolve("assets/textures/gui/sprites/pause_menu/");
            mtex.iconBug = ui.registerTexture(pdir + "bug.png");
            mtex.iconFeedback = ui.registerTexture(pdir + "player_reporting.png");
            mtex.iconSocial = ui.registerTexture(pdir + "social_interactions.png");
            mtex.playerPreview = playerPreviewTex;
            menu.init(mtex, &lang, &keyBinds);
        }

        // Spectator hotbar icons from assets/textures/gui/sprites/spectator/. Known names
        // get their vanilla slots (teleports left, close on the right); the scroll_* page
        // arrows are not menu items; anything else fills the free middle slots.
        std::vector<int> spectatorSlots(9, -1);
        {
            std::vector<std::filesystem::path> files;
            std::error_code ec;
            std::filesystem::directory_iterator it(
                mc::resolve("assets/textures/gui/sprites/spectator"), ec);
            if (!ec) {
                for (const auto& entry : it) {
                    if (entry.path().extension() == ".png") files.push_back(entry.path());
                }
            }
            std::sort(files.begin(), files.end());
            int nextFree = 2;
            for (const auto& file : files) {
                std::string stem = file.stem().string();
                int slot = -1;
                if (stem == "teleport_to_player") slot = 0;
                else if (stem == "teleport_to_team") slot = 1;
                else if (stem == "close") slot = 8;
                else if (stem == "scroll_left" || stem == "scroll_right") continue;
                else if (nextFree < 8) slot = nextFree++;
                if (slot >= 0) spectatorSlots[slot] = ui.registerTexture(file.string());
            }
            if (files.empty()) {
                std::fprintf(stderr, "[Hud] no spectator icons found (drop PNGs in "
                                     "assets/textures/gui/sprites/spectator)\n");
            }
        }

        std::fprintf(stderr,
            "[Controls] WASD move, mouse look, Space/Shift up-down (or jump), Ctrl boost/sprint\n"
            "           Left-click break, Right-click place, 1-9 / scroll select block\n"
            "           E inventory (click blocks into the hotbar; right-click a slot clears it)\n"
            "           G cycle mode, F3 debug, F5 perspective (1st/3rd), R rain (debug), Esc pause\n");

        // THE split: the session exists only while a world is open. Boot = title screen.
        std::unique_ptr<mc::Session> session;

        // Captured = gameplay (hidden cursor, raw motion); released = menus/inventory.
        // Recapturing always resyncs the player's look so a cursor that moved while free
        // (pause menu, inventory) doesn't snap the camera on the first frame back.
        auto setCursorCaptured = [&](bool captured) {
            if (captured) {
                glfwSetInputMode(window.handle(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                if (glfwRawMouseMotionSupported())
                    glfwSetInputMode(window.handle(), GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
                if (session) {
                    session->player.syncLook();
                    session->ignoreMouseUntilRelease(); // don't let the closing click break a block
                }
            } else {
                if (glfwRawMouseMotionSupported())
                    glfwSetInputMode(window.handle(), GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
                glfwSetInputMode(window.handle(), GLFW_CURSOR, GLFW_CURSOR_NORMAL);
                int ww = 0, wh = 0;
                glfwGetWindowSize(window.handle(), &ww, &wh);
                glfwSetCursorPos(window.handle(), ww * 0.5, wh * 0.5);
            }
        };

        const std::string savesRoot = mc::resolve("saves");
        mc::Camera menuCam; // static camera behind the title (panorama fills the frame)
        setCursorCaptured(false);
        bool debugOn = false;

        auto leaveSession = [&]() { // save + drop the whole game
            if (!session) return;
            invScreen.setOpen(false, &session->inv); // carried stack back into the bag
            // Overflow that didn't fit spawns into the world before it's saved, so it
            // persists as ground drops instead of vanishing on quit.
            for (const mc::ItemStack& d : invScreen.takeDrops()) session->throwStack(d);
            session->destroy(ctx);
            session.reset();
            renderer.setDropDraws({});
            renderer.setCracks({});
            debugOn = false; // F3 dies with the world
        };
        auto enterWorld = [&](const std::string& folder) {
            leaveSession();
            session = std::make_unique<mc::Session>();
            session->start(registry, savesRoot + "/" + folder, settings, tuning);
            session->world.setLodColors(lodTopColors, lodSideColors);
            session->player.setKeyBinds(&keyBinds);
            menu.setPage(mc::MenuScreen::Page::None);
            setCursorCaptured(true);
        };
        auto exitToTitle = [&]() {
            leaveSession();
            menu.setPage(mc::MenuScreen::Page::Title);
            setCursorCaptured(false);
        };
        auto openWorldsPage = [&]() {
            std::vector<mc::MenuScreen::WorldEntry> entries;
            for (const mc::WorldSave::Info& w : mc::WorldSave::list(savesRoot)) {
                std::string modeName =
                    w.mode == 1   ? lang.tr("gameMode.survival", "Survival")
                    : w.mode == 2 ? lang.tr("gameMode.spectator", "Spectator")
                                  : lang.tr("gameMode.creative", "Creative");
                entries.push_back({w.folder, w.name,
                                   modeName + " - " + lang.tr("selectWorld.day", "Day") + " " +
                                       std::to_string(w.day) + " - " + w.folder});
            }
            menu.setWorlds(std::move(entries));
            menu.setPage(mc::MenuScreen::Page::Worlds);
        };

        // App-side input state (UI edges, menu text fields, preview drag).
        bool escPrev = false, ePrev = false, f3Prev = false, f6Prev = false, f7Prev = false;
        // FUTURE(ik): F6 cycles a debug elbow/knee bend so the split joints are visibly
        // independent — a temporary A1 proof, removed once the AnimGraph drives joints.
        float debugBendDeg = 0.0f;
        bool clipTestOn = true; // A3: locomotion blend drives the body (F7 toggles vanilla)
        bool leftPrev = false, rightPrev = false, midPrev = false, qPrev = false;
        bool bsPrev = false;
        float bsCooldown = 0.0f;
        float previewYaw = 180.0f; // title model preview faces the camera; drag spins it
        float lastPreviewMx = -1.0f;
        float fps = 0.0f;
        uint64_t prevGpuMem = 0;
        const std::vector<mc::AABB> kNoOutline;

        double lastTime = glfwGetTime();
        // Rolling profiler: averages over ~2s appended to perf.log so the bottleneck
        // (CPU sim vs GPU vs draw-record) is visible after a play session.
        double perfTimer = 0.0, perfSimAccum = 0.0, perfWaitAccum = 0.0, perfRecAccum = 0.0;
        int perfFrames = 0;
        const std::string perfPath = mc::resolve("perf.log");
        std::remove(perfPath.c_str());
        while (!window.shouldClose()) {
            window.pollEvents();
            double now = glfwGetTime();
            float dt = static_cast<float>(now - lastTime);
            lastTime = now;
            if (dt > 0.1f) dt = 0.1f; // clamp on hitches so physics doesn't explode
            float instFps = dt > 0.0f ? 1.0f / dt : 0.0f;
            fps += (instFps - fps) * 0.1f;

            // Esc: closes the inventory first, steps menus back (options -> parent,
            // pause -> game), or pauses the game. On the title screen it does nothing.
            bool escNow = glfwGetKey(window.handle(), GLFW_KEY_ESCAPE) == GLFW_PRESS;
            if (escNow && !escPrev) {
                if (invScreen.open() && session) {
                    invScreen.setOpen(false, &session->inv);
                    setCursorCaptured(true);
                } else if (menu.open()) {
                    menu.back();
                    if (!menu.open()) setCursorCaptured(true);
                } else if (session) {
                    menu.setPage(mc::MenuScreen::Page::Pause);
                    setCursorCaptured(false);
                }
            }
            escPrev = escNow;

            // E toggles the inventory (in a world, no menu, not in spectator).
            bool eNow = glfwGetKey(window.handle(), keyBinds.key("inventory")) == GLFW_PRESS;
            if (eNow && !ePrev && session && !menu.open() &&
                session->player.mode != mc::GameMode::Spectator) {
                if (!invScreen.open()) {
                    invMode = session->player.mode == mc::GameMode::Creative
                                  ? mc::InventoryScreen::Mode::Creative
                                  : mc::InventoryScreen::Mode::Survival;
                    invScreen.setHeaderTitle(settings.playerName);
                }
                invScreen.setOpen(!invScreen.open(), &session->inv);
                setCursorCaptured(!invScreen.open());
            }
            ePrev = eNow;

            // F3 debug overlay: in-world only.
            bool f3Now = glfwGetKey(window.handle(), keyBinds.key("debug")) == GLFW_PRESS;
            if (f3Now && !f3Prev && session) debugOn = !debugOn;
            f3Prev = f3Now;

            // F6: cycle the debug joint bend (0 -> 45 -> 90 -> 0) to see elbows/knees flex.
            bool f6Now = glfwGetKey(window.handle(), GLFW_KEY_F6) == GLFW_PRESS;
            if (f6Now && !f6Prev) debugBendDeg = debugBendDeg >= 90.0f ? 0.0f : debugBendDeg + 45.0f;
            f6Prev = f6Now;

            // F7: toggle the A3 locomotion blend (idle/walk/run by real speed) on the body.
            bool f7Now = glfwGetKey(window.handle(), GLFW_KEY_F7) == GLFW_PRESS;
            if (f7Now && !f7Prev) {
                clipTestOn = !clipTestOn;
                std::fprintf(stderr, "[Anim] locomotion blend %s\n", clipTestOn ? "ON" : "OFF");
            }
            f7Prev = f7Now;

            bool invOpen = invScreen.open();
            bool menuOpen = menu.open();

            // Shared per-frame input the UI screens consume.
            double scroll = window.consumeScroll();
            std::string typedText = window.consumeText();
            bool leftNow = glfwGetMouseButton(window.handle(), GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
            bool rightNow = glfwGetMouseButton(window.handle(), GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
            bool midNow = glfwGetMouseButton(window.handle(), GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
            bool qNow = glfwGetKey(window.handle(), keyBinds.key("drop")) == GLFW_PRESS;
            bool leftEdge = leftNow && !leftPrev;
            bool rightEdge = rightNow && !rightPrev;
            bool midEdge = midNow && !midPrev;
            bool qEdge = qNow && !qPrev;
            leftPrev = leftNow;
            rightPrev = rightNow;
            midPrev = midNow;
            qPrev = qNow;
            bool bsNow = glfwGetKey(window.handle(), GLFW_KEY_BACKSPACE) == GLFW_PRESS;
            bsCooldown -= dt;
            bool backspaceFire = false;
            if (bsNow && (!bsPrev || bsCooldown <= 0.0f)) {
                backspaceFire = true;
                bsCooldown = bsPrev ? 0.05f : 0.35f; // held backspace repeats
            }
            if (!bsNow) bsCooldown = 0.0f;
            bsPrev = bsNow;

            // ---- Gameplay step. Skipped entirely while a menu is open (hard pause) —
            // and at the title there is no session at all.
            if (session && !menuOpen) {
                mc::Session::FrameInput fin;
                fin.inventoryOpen = invOpen;
                fin.scroll = invOpen ? 0.0 : scroll;
                fin.now = now;
                auto tSim = std::chrono::high_resolution_clock::now();
                session->update(ctx, window.handle(), renderer, dt, fin);
                perfSimAccum += std::chrono::duration<double, std::milli>(
                                    std::chrono::high_resolution_clock::now() - tSim)
                                    .count();

                // A container block was right-clicked: open its screen.
                mc::Session::ContainerRequest creq = session->takeContainerRequest();
                if (creq.kind == mc::kContainerCrafting && !invOpen) {
                    invMode = mc::InventoryScreen::Mode::Crafting;
                    invScreen.setOpen(true, &session->inv);
                    setCursorCaptured(false);
                    invOpen = true;
                }
                // Furnace/chest requests arrive here once those screens exist.
            }
            renderer.setTitlePanorama(session == nullptr, static_cast<float>(now));

            // ---- UI pass.
            uint32_t sw = swapchain.extent.width, sh = swapchain.extent.height;
            ui.beginFrame(sw, sh);

            // HUD (hotbar/crosshair); spectator variant shows briefly after scrolling.
            if (session && session->player.mode == mc::GameMode::Spectator) {
                hud.build(ui, sw, sh, session->inv.selected, hudTex, spectatorSlots.data(),
                          static_cast<int>(spectatorSlots.size()),
                          /*showHotbar=*/session->spectatorHudTimer > 0.0f);
            } else if (session) {
                int slotIcons[mc::Inventory::kSlots];
                int slotCounts[mc::Inventory::kSlots];
                for (int i = 0; i < mc::Inventory::kSlots; ++i) {
                    const mc::ItemStack& s = session->inv.slots[static_cast<size_t>(i)];
                    slotIcons[i] = s.empty() ? -1 : iconRenderer.iconTexId(s.id);
                    slotCounts[i] = s.count;
                }
                hud.build(ui, sw, sh, session->inv.selected, hudTex, slotIcons,
                          mc::Inventory::kSlots, /*showHotbar=*/true,
                          /*showCrosshair=*/!invOpen && !menuOpen, &font, slotCounts);
            }

            if (debugOn && session) {
                mc::DebugInfo info;
                info.fps = fps;
                info.pos = session->player.camera.position;
                info.yaw = session->player.camera.yaw;
                info.pitch = session->player.camera.pitch;
                info.chunksLoaded = session->world.loadedChunkCount();
                info.chunksRendered = renderer.lastRenderedChunks();
                info.lodTiles = session->world.lodTileCount();
                info.gpuMemBytes = ctx.gpuMemoryUsedBytes();
                info.ramBytes = mc::processMemoryBytes();
                info.cpuPercent = mc::cpuUsagePercent();
                info.displayW = sw;
                info.displayH = sh;
                info.gpuName = ctx.deviceName;
                info.allocRateMiBs = dt > 0.0f
                    ? static_cast<float>((static_cast<double>(info.gpuMemBytes) -
                                          static_cast<double>(prevGpuMem)) / (1024.0 * 1024.0) / dt)
                    : 0.0f;
                prevGpuMem = info.gpuMemBytes;
                char clock[8];
                session->gameTime.clockText(clock, sizeof(clock));
                info.timeText = std::string("Time: ") + clock + " (day " +
                                std::to_string(session->gameTime.dayCount()) + ")";
                const mc::RaycastHit& hit = session->targetHit();
                if (hit.hit) {
                    info.hasTarget = true;
                    info.targetBlock = hit.block;
                    info.targetNormal = hit.normal;
                    mc::BlockState tstate =
                        session->world.getState(hit.block.x, hit.block.y, hit.block.z);
                    const mc::Block& tb = registry.block(tstate);
                    info.targetName = tb.name();
                    char tags[96];
                    std::snprintf(tags, sizeof(tags), "strength %.1f%s%s, data %u",
                                  tb.props().strength,
                                  tb.props().opaque ? ", opaque" : ", transparent",
                                  tb.props().requiresTool ? ", needs tool" : "",
                                  tstate.data());
                    info.targetTags = tags;
                }
                debug.build(ui, font, sw, sh, info);
            }

            // Mouse in framebuffer pixels (window coords rescaled for HiDPI), shared by
            // the inventory screen and the menus.
            float uiMouseX = 0.0f, uiMouseY = 0.0f;
            {
                double mx = 0.0, my = 0.0;
                glfwGetCursorPos(window.handle(), &mx, &my);
                int winW = 0, winH = 0;
                glfwGetWindowSize(window.handle(), &winW, &winH);
                uiMouseX = static_cast<float>(mx) *
                           (winW > 0 ? static_cast<float>(sw) / static_cast<float>(winW) : 1.0f);
                uiMouseY = static_cast<float>(my) *
                           (winH > 0 ? static_cast<float>(sh) / static_cast<float>(winH) : 1.0f);
            }

            // Inventory screen, drawn over the HUD/debug text.
            if (invOpen && session) {
                mc::InventoryScreen::Input sin;
                sin.mouseX = uiMouseX;
                sin.mouseY = uiMouseY;
                sin.leftPressed = leftEdge;
                sin.leftDown = leftNow;
                sin.rightPressed = rightEdge;
                sin.rightDown = rightNow;
                sin.middlePressed = midEdge;
                sin.dropPressed = qEdge;
                sin.shiftDown = glfwGetKey(window.handle(), GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                                glfwGetKey(window.handle(), GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
                sin.scroll = scroll;
                sin.time = now;
                invScreen.build(ui, font, registry, iconRenderer, session->inv, sw, sh, sin,
                                invMode);

                // Hovering a block and pressing 1-9 drops it straight into that slot.
                if (invScreen.hoveredBlock() != mc::BLOCK_AIR) {
                    for (int i = 0; i < mc::Inventory::kSlots; ++i) {
                        if (glfwGetKey(window.handle(), GLFW_KEY_1 + i) == GLFW_PRESS) {
                            session->inv.slots[static_cast<size_t>(i)] =
                                {invScreen.hoveredBlock(), 64};
                        }
                    }
                }
            }

            // Drops queued by the screen (Q, off-panel clicks, overflow when it closes)
            // get thrown into the world — drained every frame so a stack dropped as the
            // screen closes still spawns even though the screen no longer builds.
            if (session) {
                for (const mc::ItemStack& d : invScreen.takeDrops()) session->throwStack(d);
            }

            // Title / pause / options menus, drawn over everything. Slider changes apply
            // live (FOV, sensitivity, render distance, fullscreen); Done persists them.
            if (menuOpen) {
                mc::MenuScreen::Input menuIn;
                menuIn.mouseX = uiMouseX;
                menuIn.mouseY = uiMouseY;
                menuIn.leftPressed = leftEdge;
                menuIn.leftDown = leftNow;
                menuIn.typed = typedText;
                menuIn.backspace = backspaceFire;
                menuIn.keyPressed = window.consumeKeyPress(); // key-bind rebinding
                menuIn.scroll = scroll;
                menuIn.time = now;
                // Title screen: dragging over the right half spins the model preview
                // (skipped while the pointer sits on a widget, e.g. the name box).
                if (menu.page() == mc::MenuScreen::Page::Title) {
                    if (menuIn.leftDown && lastPreviewMx >= 0.0f && !menu.pointerOverWidget() &&
                        menuIn.mouseX > sw * 0.55f) {
                        previewYaw += (menuIn.mouseX - lastPreviewMx) * 0.4f;
                    }
                    lastPreviewMx = menuIn.mouseX;
                } else {
                    lastPreviewMx = -1.0f;
                }
                mc::MenuScreen::Action act = menu.build(ui, font, sw, sh, menuIn, settings);
                if (menu.takeKeyBindsDirty()) keyBinds.save(keyBindsPath);
                if (session) session->applySettings(settings);
                if (window.isFullscreen() != settings.fullscreen) {
                    window.setFullscreen(settings.fullscreen);
                }
                switch (act) {
                    case mc::MenuScreen::Action::OpenWorlds:
                        openWorldsPage();
                        break;
                    case mc::MenuScreen::Action::PlayWorld:
                        enterWorld(menu.actionFolder());
                        break;
                    case mc::MenuScreen::Action::CreateWorld: {
                        std::string name = menu.createName();
                        std::string folder = mc::WorldSave::makeFolder(savesRoot, name);
                        mc::WorldSave fresh(savesRoot + "/" + folder);
                        mc::WorldSave::Level l;
                        l.name = name;
                        const std::string& seedText = menu.createSeedText();
                        if (seedText.empty()) {
                            // Random seed, salted with the clock so every created world
                            // is guaranteed distinct.
                            l.seed = static_cast<uint32_t>(std::random_device{}()) ^
                                     static_cast<uint32_t>(
                                         std::chrono::steady_clock::now().time_since_epoch().count());
                        } else {
                            try {
                                l.seed = static_cast<uint32_t>(std::stoul(seedText));
                            } catch (const std::exception&) { // words hash like vanilla
                                l.seed = static_cast<uint32_t>(std::hash<std::string>{}(seedText));
                            }
                        }
                        l.mode = menu.createSurvival() ? 1 : 0;
                        fresh.saveLevel(l);
                        enterWorld(folder);
                        break;
                    }
                    case mc::MenuScreen::Action::DeleteWorld:
                        mc::WorldSave::remove(savesRoot, menu.actionFolder());
                        openWorldsPage();
                        break;
                    case mc::MenuScreen::Action::Resume:
                        menu.setPage(mc::MenuScreen::Page::None);
                        setCursorCaptured(true);
                        break;
                    case mc::MenuScreen::Action::SaveQuit:
                        exitToTitle();
                        break;
                    case mc::MenuScreen::Action::Done:
                        settings.save(optionsPath);
                        break;
                    case mc::MenuScreen::Action::OpenLanguages: {
                        std::vector<mc::MenuScreen::LangEntry> entries;
                        for (const mc::Lang::Info& l : mc::Lang::list(mc::resolve("assets/lang"))) {
                            entries.push_back({l.code, l.name});
                        }
                        menu.setLanguages(std::move(entries));
                        menu.setPage(mc::MenuScreen::Page::Language);
                        break;
                    }
                    case mc::MenuScreen::Action::SetLanguage:
                        settings.language = menu.actionLanguage();
                        lang.load(mc::resolve("assets/lang"), settings.language);
                        settings.save(optionsPath); // menus retranslate live next frame
                        break;
                    case mc::MenuScreen::Action::UploadSkin: {
                        std::string picked = mc::openPngFileDialog();
                        if (!picked.empty()) {
                            std::error_code ec;
                            std::filesystem::path src(picked);
                            std::filesystem::path dstDir(
                                mc::resolve("assets/textures/entity/player/custom"));
                            std::filesystem::create_directories(dstDir, ec);
                            std::filesystem::path dst = dstDir / src.filename();
                            std::filesystem::copy_file(
                                src, dst, std::filesystem::copy_options::overwrite_existing, ec);
                            if (!ec || std::filesystem::exists(dst)) {
                                std::string stem = dst.stem().string();
                                std::string low = stem;
                                std::transform(low.begin(), low.end(), low.begin(),
                                               [](unsigned char c) {
                                                   return static_cast<char>(std::tolower(c));
                                               });
                                playerSkin.texturePath = dst.string();
                                playerSkin.name = stem;
                                playerSkin.slim = low.find("slim") != std::string::npos ||
                                                  low.find("alex") != std::string::npos;
                                settings.skin = stem;
                                settings.save(optionsPath);
                                // Live-swap the body model (skin texture + arm width).
                                vkDeviceWaitIdle(ctx.device);
                                modelRenderer.cleanup();
                                reloadRig(playerSkin.slim); // steve<->alex changes the rig
                                rigMeshes = mc::buildPlayerRigMeshes(playerRig, playerSkin.slim,
                                                                     modelLayerMask);
                                modelRenderer.init(&ctx, &swapchain, renderer.cameraUBOs(),
                                                   playerSkin.texturePath, rigMeshes);
                                playerPreview.rebuildSkin(playerSkin.texturePath, rigMeshes);
                                std::fprintf(stderr, "[Skin] installed '%s' (%s)\n",
                                             stem.c_str(),
                                             playerSkin.slim ? "slim/Alex" : "wide/Steve");
                            } else {
                                std::fprintf(stderr, "[Skin] could not copy %s\n",
                                             picked.c_str());
                            }
                        }
                        break;
                    }
                    case mc::MenuScreen::Action::Quit:
                        glfwSetWindowShouldClose(window.handle(), GLFW_TRUE);
                        break;
                    case mc::MenuScreen::Action::None:
                        break;
                }
            }

            // ---- Poses: the in-world body model and the offscreen preview.
            auto rotX = [](float a) { return glm::rotate(glm::mat4(1.0f), a, glm::vec3(1, 0, 0)); };
            auto rotY = [](float a) { return glm::rotate(glm::mat4(1.0f), a, glm::vec3(0, 1, 0)); };
            auto rotZ = [](float a) { return glm::rotate(glm::mat4(1.0f), a, glm::vec3(0, 0, 1)); };
            auto trans = [](glm::vec3 t) { return glm::translate(glm::mat4(1.0f), t); };
            // Rotate about a bone's pivot (pixel space); used for the hand-tuned
            // first-person arm placement.
            auto jointAt = [&](glm::vec3 pv, const glm::mat4& rot) {
                return trans(pv) * rot * trans(-pv);
            };
            // Full third-person/preview body pose: animator local transforms -> camera
            // head look -> optional debug joint bend -> compose the rig -> place in world.
            // Finish a sampled local Pose into a world-placed ModelPose: camera head look,
            // optional F6 debug joint bend, compose the rig, then place in the world.
            auto finishPose = [&](mc::Pose& p, const glm::mat4& place, const glm::quat& headLook) {
                if (biHead >= 0) p.rot[biHead] = p.rot[biHead] * headLook;
                if (debugBendDeg != 0.0f) { // FUTURE(ik): visible elbow/knee flex proof
                    glm::quat bendArm = glm::angleAxis(glm::radians(-debugBendDeg), glm::vec3(1, 0, 0));
                    glm::quat bendLeg = glm::angleAxis(glm::radians(debugBendDeg), glm::vec3(1, 0, 0));
                    const glm::quat bends[] = {bendArm, bendArm, bendLeg, bendLeg};
                    for (int k = 0; k < 4; ++k)
                        if (biBend[k] >= 0) p.rot[biBend[k]] = bends[k];
                }
                glm::mat4 world[mc::kMaxModelBones];
                p.compose(playerRig, world);
                mc::ModelPose mp{};
                mp.count = static_cast<uint32_t>(playerRig.boneCount());
                for (int i = 0; i < playerRig.boneCount(); ++i) mp.bones[i] = place * world[i];
                return mp;
            };
            // Vanilla animator body pose (used by the preview and when the A3 demo is off).
            auto buildBodyPose = [&](const mc::PlayerAnimator::Input& in, const glm::mat4& place,
                                     const glm::quat& headLook) {
                mc::Pose p(playerRig);
                animator.evaluatePose(in, playerRig, p);
                return finishPose(p, place, headLook);
            };

            // Animator input: idle defaults without a session (title preview), the live
            // player state with one.
            mc::PlayerAnimator::Input ain;
            ain.attackTime = 1.0f;
            ain.slim = playerSkin.slim;
            ain.lifeTime = static_cast<float>(now);
            if (session) {
                ain.distanceMoved = session->player.limbSwing();
                ain.moveAmount = session->player.limbSwingAmount();
                ain.lifeTime = session->player.animTime();
                ain.attackTime = session->player.swingProgress();
                ain.sneakAmount =
                    glm::clamp(session->player.camera.crouchOffset / 0.3f, 0.0f, 1.0f);
            }

            // Live-apply the Skin Customization toggles: rebuild the model (body +
            // preview) whenever a layer flips. Rare event, so a device idle is fine.
            if (uint32_t m = skinLayerMask(); m != modelLayerMask) {
                modelLayerMask = m;
                vkDeviceWaitIdle(ctx.device);
                modelRenderer.cleanup();
                rigMeshes = mc::buildPlayerRigMeshes(playerRig, playerSkin.slim, modelLayerMask);
                modelRenderer.init(&ctx, &swapchain, renderer.cameraUBOs(),
                                   playerSkin.texturePath, rigMeshes);
                playerPreview.rebuildSkin(playerSkin.texturePath, rigMeshes);
            }

            mc::ModelPose pose{};
            bool modelVisible = false;
            glm::mat4 itemDelta(1.0f); // first-person held-item motion (identity in 3rd person)
            if (session) {
                mc::Player& player = session->player;
                modelVisible = true;
                if (player.camera.perspective == mc::Perspective::First) {
                    // First person: draw only the right-arm chain, anchored to the camera.
                    // Base pose is hand-tuned; the first_person.* animations layer on as
                    // deltas. The chain (arm+forearm+sleeves) shares one rigid matrix — same
                    // look as the old single baked arm, but now per-bone.
                    modelVisible = player.mode != mc::GameMode::Spectator;
                    pose.count = static_cast<uint32_t>(playerRig.boneCount());
                    pose.mask = 0;
                    glm::mat4 armDelta(1.0f);
                    // View Bobbing off: steady the first-person hand (no walk bob/sway).
                    mc::PlayerAnimator::Input fpIn = ain;
                    if (settings.optGet("viewBobbing", 0.0f) > 0.5f) { // index 1 = OFF
                        fpIn.moveAmount = 0.0f;
                        fpIn.distanceMoved = 0.0f;
                    }
                    animator.evaluateFirstPerson(fpIn, armDelta, itemDelta);
                    glm::mat4 fpPlace =
                        glm::translate(glm::mat4(1.0f), player.camera.renderEye()) *
                        rotY(glm::radians(-player.camera.yaw - 90.0f)) *
                        rotX(glm::radians(player.camera.pitch)) *
                        glm::scale(glm::mat4(1.0f), glm::vec3(1.85f / 32.0f));
                    glm::vec3 armPivot = biRightArm >= 0 ? playerRig.bone(biRightArm).pivot
                                                         : glm::vec3(5.0f, 22.0f, 0.0f);
                    // Shoulder sits below/right of the screen edge; the arm points
                    // up-forward so it enters from the bottom-right like MC.
                    glm::mat4 armM = fpPlace * trans({1.5f, -28.0f, 3.5f}) * armDelta *
                                     jointAt(armPivot, rotX(glm::radians(100.0f)) *
                                                       rotZ(glm::radians(-10.0f)) *
                                                       rotY(glm::radians(-8.0f)));
                    for (int c : fpChain)
                        if (c >= 0) { pose.bones[c] = armM; pose.mask |= 1u << c; }
                } else {
                    // Data-driven pose (bob + walk + attack + sneak), then the
                    // camera-driven head look layered on top.
                    glm::vec3 feet = player.feetPosition();
                    float bodyAngle = glm::radians(-player.bodyYaw() - 90.0f);
                    glm::mat4 place = trans(feet) *
                                      glm::rotate(glm::mat4(1.0f), bodyAngle, glm::vec3(0, 1, 0)) *
                                      glm::scale(glm::mat4(1.0f), glm::vec3(1.85f / 32.0f));
                    float headYawExtra = glm::radians(-(player.headYaw() - player.bodyYaw()));
                    float headPitch = glm::radians(player.camera.pitch);
                    glm::quat headLook = glm::angleAxis(headYawExtra, glm::vec3(0, 1, 0)) *
                                         glm::angleAxis(headPitch, glm::vec3(1, 0, 0));
                    if (clipTestOn && playerGraph.loaded()) {
                        // A4: the state machine drives the body. Gameplay writes world state
                        // into the blackboard (P1); the graph reads it (v.speed / v.is_on_ground
                        // / v.jump_pressed) to pick + cross-fade states. Stride phase is
                        // distance-driven so the locomotion feet track ground travel.
                        glm::vec3 v = player.velocity();
                        float speed = glm::length(glm::vec2(v.x, v.z));
                        blackboard.setFloat("speed", speed);
                        blackboard.setBool("is_on_ground", player.onGround());
                        bool jumpKey = glfwGetKey(window.handle(), GLFW_KEY_SPACE) == GLFW_PRESS;
                        if (jumpKey && !jumpAnimPrev && player.onGround() && !player.flying)
                            blackboard.fire("jump_pressed"); // stand-jump vs run-jump: guarded by speed
                        jumpAnimPrev = jumpKey;
                        // H waves the right arm — the A5 upper-body layer plays over the legs.
                        bool waveKey = glfwGetKey(window.handle(), GLFW_KEY_H) == GLFW_PRESS;
                        if (waveKey && !waveAnimPrev) blackboard.fire("wave_pressed");
                        waveAnimPrev = waveKey;
                        constexpr float kStrideMeters = 2.0f; // ground travel per stride cycle
                        float stridePhase = std::fmod(player.limbSwing() / kStrideMeters, 1.0f);
                        mc::Pose p(playerRig);
                        playerGraph.update(dt, blackboard, stridePhase, playerRig, p);
                        blackboard.clearTriggers();
                        if (playerGraph.currentState(0) != prevAnimState) {
                            prevAnimState = playerGraph.currentState(0);
                            std::fprintf(stderr, "[Graph] state -> %s\n", prevAnimState.c_str());
                        }
                        // A6: drain animation events. This is the fan-out point (P3) — sound
                        // by block material, FUTURE(decals) footprints, FUTURE(snow) craters,
                        // FUTURE(boat) oar thrust. For now, log footsteps to prove one-per-step.
                        for (const mc::AnimFiredEvent& ev : playerGraph.events())
                            std::fprintf(stderr, "[anim] %s (%s)\n", ev.name.c_str(), ev.bone.c_str());
                        // The authored clips drive the whole body (knee bends, any hip motion) —
                        // no procedural foot-lock IK. Skating stays at vanilla level; the clean
                        // walk (timed knee bends + subtle bob) is authored, not computed.
                        // FUTURE(ik): anim/FootIK is kept for mounts/creatures if wanted later.
                        pose = finishPose(p, place, headLook);
                    } else {
                        pose = buildBodyPose(ain, place, headLook);
                    }
                }
                // Spectator in 3rd person: just a translucent floating head (+ hat), like MC.
                if (player.mode == mc::GameMode::Spectator &&
                    player.camera.perspective != mc::Perspective::First) {
                    pose.mask = 0;
                    if (biHead >= 0) pose.mask |= 1u << biHead;
                    if (biHat >= 0) pose.mask |= 1u << biHat;
                    pose.opacity = 0.5f;
                }
            }

            // Offscreen player preview (title screen right column + the inventory
            // panels' player box). The title drag yaw spins it; in the inventory it
            // watches the cursor like vanilla. It idles on real time so it keeps
            // breathing while gameplay is paused.
            bool previewOn = !session || invOpen;
            if (previewOn) {
                mc::PlayerAnimator::Input pain = ain;
                pain.lifeTime = static_cast<float>(now);
                float baseYaw = previewYaw;
                float headYawE = 0.0f, headPitchE = 0.0f;
                if (session && invOpen && invScreen.previewLookValid()) {
                    baseYaw = 180.0f + invScreen.previewLookYawDeg() * 0.4f;
                    headYawE = invScreen.previewLookYawDeg() * 0.75f;
                    headPitchE = -invScreen.previewLookPitchDeg() * 0.6f;
                }
                glm::mat4 pplace = rotY(glm::radians(baseYaw)) *
                                   glm::scale(glm::mat4(1.0f), glm::vec3(1.85f / 32.0f));
                mc::ModelPose previewPose = buildBodyPose(
                    pain, pplace,
                    glm::angleAxis(glm::radians(headYawE), glm::vec3(0, 1, 0)) *
                        glm::angleAxis(glm::radians(headPitchE), glm::vec3(1, 0, 0)));
                renderer.setPlayerPreview(true, previewPose);
            } else {
                renderer.setPlayerPreview(false, {});
            }

            // Held block riding the right hand; orientation/size from the block model's
            // display table (translation in 16ths, scale in blocks -> model px).
            mc::BlockId selectedBlock = session ? session->inv.selectedBlock() : mc::BLOCK_AIR;
            bool firstPerson =
                session && session->player.camera.perspective == mc::Perspective::First;
            const mc::ItemTransform& disp = firstPerson
                                                ? registry.block(selectedBlock).display().firstPerson
                                                : registry.block(selectedBlock).display().thirdPerson;
            constexpr float kPxPerBlock = 32.0f / 1.85f; // model px per world block
            constexpr float kHeldShrink = 0.56f;         // hand-held blocks read a bit smaller
            // Extra 90-deg right yaw at the end: engine meshes face 90 deg off vanilla's
            // model space (same correction as the inventory icons).
            glm::mat4 dispM = trans(disp.translation * (kPxPerBlock / 16.0f)) *
                              rotX(glm::radians(disp.rotationDeg.x)) *
                              rotY(glm::radians(disp.rotationDeg.y)) *
                              rotZ(glm::radians(disp.rotationDeg.z)) *
                              glm::scale(glm::mat4(1.0f), disp.scale * kPxPerBlock * kHeldShrink) *
                              rotY(glm::radians(-90.0f));
            // Anchor the held block to the rightItem bone — parented to the forearm in
            // player.json with the vanilla hand pivot, so it follows the lower arm (and
            // its slim/wide offset) with no hardcoded numbers. The bone's world is its
            // parent's at rest; the pivot is the actual attach point, applied here.
            int biHand = biRightItem >= 0 ? biRightItem : biRightArm;
            glm::mat4 anchor = biHand >= 0 ? pose.bones[biHand] : glm::mat4(1.0f);
            if (biRightItem >= 0) anchor = anchor * trans(playerRig.bone(biRightItem).pivot);
            glm::mat4 heldM = anchor * itemDelta * dispM * trans({-0.5f, -0.5f, -0.5f});
            bool heldVisible = session && selectedBlock != mc::BLOCK_AIR &&
                               session->player.canEdit() && modelVisible;

            const mc::Camera& cam = session ? session->player.camera : menuCam;
            const std::vector<mc::AABB>& outline =
                session ? session->outlineBoxes() : kNoOutline;
            renderer.drawFrame(cam, session ? &session->world : nullptr, &ui, outline,
                               &modelRenderer, pose, modelVisible, &heldRenderer, heldM,
                               selectedBlock, heldVisible);

            // Profiler: accumulate this frame, flush an averaged line to perf.log every ~2s.
            perfWaitAccum += renderer.gpuWaitMs();
            perfRecAccum += renderer.recordMs();
            ++perfFrames;
            perfTimer += dt;
            if (perfTimer >= 2.0 && perfFrames > 0 && session) {
                double inv = 1.0 / perfFrames;
                std::FILE* pf = std::fopen(perfPath.c_str(), "a");
                if (pf) {
                    std::fprintf(pf,
                                 "fps %.1f | sim %.1fms gpuWait %.1fms record %.1fms | "
                                 "chunks %d/%d rendered %d | lod %d | RAM %lluMiB VRAM %lluMiB\n",
                                 perfFrames / perfTimer, perfSimAccum * inv, perfWaitAccum * inv,
                                 perfRecAccum * inv, session->world.loadedChunkCount(),
                                 session->world.readyChunkCount(), renderer.lastRenderedChunks(),
                                 session->world.lodTileCount(),
                                 static_cast<unsigned long long>(mc::processMemoryBytes() >> 20),
                                 static_cast<unsigned long long>(ctx.gpuMemoryUsedBytes() >> 20));
                    std::fclose(pf);
                }
                perfTimer = perfSimAccum = perfWaitAccum = perfRecAccum = 0.0;
                perfFrames = 0;
            }
        }

        leaveSession(); // closing the window mid-game still saves
        settings.fullscreen = window.isFullscreen();
        window.windowedSize(settings.windowW, settings.windowH);
        settings.save(optionsPath); // persist options edited this session

        vkDeviceWaitIdle(ctx.device);
        panoramaRenderer.cleanup();
        weatherRenderer.cleanup();
        skyRenderer.cleanup();
        heldRenderer.cleanup();
        playerPreview.cleanup();
        modelRenderer.cleanup();
        renderer.cleanup();
        ui.cleanup();
        iconRenderer.cleanup();
        textures.destroy(ctx);
        swapchain.cleanup();
        ctx.cleanup();
        window.cleanup();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fatal error: %s\n", e.what());
        return 1;
    }
    return 0;
}
