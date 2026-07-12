// anim_studio (T1): a standalone tool to preview authored animation clips on the player
// rig. You author clips in Blockbench (Bedrock export -> import_bedrock); this loads the
// same rig/skin/clips the game uses and plays them with an orbit camera + scrub timeline.
// It links the mc_engine library and drives its own minimal Vulkan loop (the game's
// Renderer is world-coupled, so the tool records its own model + UI pass).
//
// Controls: Space play/pause · Left/Right scrub · [ ] previous/next clip · drag orbit ·
//           wheel zoom · R reset view · Esc quit.

#include "anim/AnimClip.h"
#include "anim/AnimGraph.h"
#include "anim/Blackboard.h"
#include "anim/Molang.h"
#include "anim/Pose.h"
#include "anim/Rig.h"
#include "core/Paths.h"
#include "core/Window.h"
#include "game/PlayerModel.h"
#include "gfx/GpuBuffer.h"
#include "gfx/ModelRenderer.h"
#include "gfx/Swapchain.h"
#include "gfx/UIRenderer.h"
#include "gfx/VkContext.h"
#include "gfx/vk_common.h"
#include "ui/Font.h"

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace mc;

namespace {

constexpr int kFrames = 2;

struct CameraUBO {
    glm::mat4 view;
    glm::mat4 proj;
};

void barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
             VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
             VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
             VkImageLayout oldL, VkImageLayout newL) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = oldL;
    b.newLayout = newL;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

// Scan assets/anim/clips for *.clip.json and load each (skips graph-only/empty files).
struct Clip {
    std::string  name;
    AnimClip     clip;
};
std::vector<Clip> loadClips() {
    std::vector<Clip> out;
    fs::path dir = resolve("assets/anim/clips");
    std::error_code ec;
    if (fs::exists(dir, ec)) {
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.path().extension() == ".json") files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const fs::path& f : files) {
            Clip c;
            c.name = f.stem().string(); // "loco_walk.clip"
            if (c.clip.load(f.string()) && c.clip.duration() > 0.0f) out.push_back(std::move(c));
        }
    }
    return out;
}

// Stable color per state name (hash -> one of a small palette), for the timeline bars.
glm::vec4 stateColor(const std::string& name) {
    static const glm::vec4 kPalette[] = {
        {0.30f, 0.70f, 1.00f, 1.0f}, {0.45f, 0.85f, 0.45f, 1.0f}, {1.00f, 0.65f, 0.25f, 1.0f},
        {0.85f, 0.45f, 0.85f, 1.0f}, {0.95f, 0.85f, 0.35f, 1.0f}, {0.45f, 0.85f, 0.85f, 1.0f},
        {0.95f, 0.45f, 0.45f, 1.0f}, {0.60f, 0.60f, 0.95f, 1.0f},
    };
    if (name.empty()) return glm::vec4(0.25f, 0.25f, 0.28f, 1.0f);
    uint32_t h = 2166136261u;
    for (char c : name) { h ^= static_cast<uint8_t>(c); h *= 16777619u; }
    return kPalette[h % (sizeof(kPalette) / sizeof(kPalette[0]))];
}

// A default player skin for the preview (falls back to white inside ModelRenderer).
std::string defaultSkin() {
    for (const char* p : {"assets/textures/entity/player/wide/steve.png",
                          "assets/textures/entity/player/custom",
                          "assets/textures/entity/player/wide"}) {
        fs::path base = resolve(p);
        std::error_code ec;
        if (fs::is_regular_file(base, ec)) return base.string();
        if (fs::is_directory(base, ec))
            for (const auto& e : fs::directory_iterator(base, ec))
                if (e.path().extension() == ".png") return e.path().string();
    }
    return "";
}

} // namespace

int main() {
    Window window;
    window.init(1100, 720, "anim_studio");

    VkContext ctx;
    ctx.init(window.handle());
    Swapchain sc;
    {
        int w, h;
        window.framebufferSize(w, h);
        sc.init(&ctx, static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    }

    // --- Model: the player rig + meshes + renderer (its own camera UBOs). ---
    Rig rig;
    rig.loadFromGeometry(resolve("assets/models/entity/player.json"), "geometry.npc.steve");
    if (rig.empty()) {
        std::fprintf(stderr, "[anim_studio] failed to load rig\n");
        return 1;
    }
    PlayerRigMeshes meshes = buildPlayerRigMeshes(rig, /*slim=*/false);

    AllocatedBuffer cameraUBO[kFrames]{};
    for (int i = 0; i < kFrames; ++i)
        cameraUBO[i] = createHostBuffer(ctx.allocator, sizeof(CameraUBO),
                                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

    ModelRenderer model;
    model.init(&ctx, &sc, cameraUBO, defaultSkin(), meshes);

    UIRenderer ui;
    ui.init(&ctx, &sc);
    Font font;
    font.init(ui, resolve("assets/textures/font/ascii.png"),
              resolve("assets/textures/font/accented.png"));

    std::vector<Clip> clips = loadClips();
    if (clips.empty()) std::fprintf(stderr, "[anim_studio] no clips found in assets/anim/clips\n");

    // Graph mode: drive the player state machine with mock inputs and show its timeline.
    AnimGraph graph;
    graph.load(resolve("assets/anim/graphs/player.graph.json"), rig);
    Blackboard bb;
    std::vector<std::string> layerNames;
    graph.layerNames(layerNames);

    // --- Minimal frame resources (2 in flight). ---
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = ctx.graphicsFamily;
    VK_CHECK(vkCreateCommandPool(ctx.device, &pci, nullptr, &pool));
    VkCommandBuffer cmds[kFrames]{};
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = pool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = kFrames;
    VK_CHECK(vkAllocateCommandBuffers(ctx.device, &cbi, cmds));

    VkSemaphore imageAvail[kFrames]{};
    VkFence inFlight[kFrames]{};
    std::vector<VkSemaphore> renderDone(sc.imageCount());
    auto makeSemaphores = [&]() {
        VkSemaphoreCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        renderDone.resize(sc.imageCount());
        for (VkSemaphore& s : renderDone) {
            if (s) vkDestroySemaphore(ctx.device, s, nullptr);
            VK_CHECK(vkCreateSemaphore(ctx.device, &si, nullptr, &s));
        }
    };
    {
        VkSemaphoreCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (int i = 0; i < kFrames; ++i) {
            VK_CHECK(vkCreateSemaphore(ctx.device, &si, nullptr, &imageAvail[i]));
            VK_CHECK(vkCreateFence(ctx.device, &fi, nullptr, &inFlight[i]));
        }
        for (VkSemaphore& s : renderDone) s = VK_NULL_HANDLE;
        makeSemaphores();
    }

    // --- Playback + view state. ---
    int   clipIdx = 0;
    float t = 0.0f;
    bool  playing = true;
    float yaw = 0.55f, pitch = 0.15f, dist = 3.4f; // orbit camera
    auto  resetView = [&]() { yaw = 0.55f; pitch = 0.15f; dist = 3.4f; };

    bool  graphMode = false;    // Tab toggles clip preview <-> graph debugger
    float gSpeed = 0.0f;        // mock v.speed (W ramps it up)
    float gStride = 0.0f;       // accumulated stride distance -> phase
    bool  gOnGround = true;     // mock v.is_on_ground (G toggles)

    // Edge-detect keys / mouse.
    bool spacePrev = false, upPrev = false, downPrev = false, dragging = false;
    bool tabPrev = false, gPrev = false, hPrev = false, jPrev = false;
    double lastX = 0, lastY = 0;
    double lastTime = glfwGetTime();
    uint32_t frame = 0;

    auto recreate = [&]() {
        int w = 0, h = 0;
        window.waitForValidFramebuffer(w, h);
        vkDeviceWaitIdle(ctx.device);
        sc.recreate(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        makeSemaphores();
    };

    while (!window.shouldClose()) {
        window.pollEvents();
        double now = glfwGetTime();
        float dt = static_cast<float>(now - lastTime);
        lastTime = now;
        if (dt > 0.1f) dt = 0.1f;

        GLFWwindow* w = window.handle();
        if (glfwGetKey(w, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        // Tab toggles clip preview <-> graph debugger; R resets the orbit.
        bool tab = glfwGetKey(w, GLFW_KEY_TAB) == GLFW_PRESS;
        if (tab && !tabPrev) graphMode = !graphMode;
        tabPrev = tab;
        if (glfwGetKey(w, GLFW_KEY_R) == GLFW_PRESS) resetView();

        // Orbit with left-drag, zoom with the wheel (shared by both modes).
        double mx, my;
        glfwGetCursorPos(w, &mx, &my);
        bool lbNow = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        if (lbNow && dragging) {
            yaw += static_cast<float>(mx - lastX) * 0.01f;
            pitch += static_cast<float>(my - lastY) * 0.01f;
            pitch = std::clamp(pitch, -1.4f, 1.4f);
        }
        dragging = lbNow;
        lastX = mx;
        lastY = my;
        dist = std::clamp(dist - static_cast<float>(window.consumeScroll()) * 0.3f, 1.2f, 10.0f);

        auto toMP = [&](const Pose& pose) {
            ModelPose mp{};
            glm::mat4 world[kMaxModelBones];
            pose.compose(rig, world);
            glm::mat4 place = glm::scale(glm::mat4(1.0f), glm::vec3(1.85f / 32.0f));
            mp.count = static_cast<uint32_t>(rig.boneCount());
            for (int i = 0; i < rig.boneCount(); ++i) mp.bones[i] = place * world[i];
            return mp;
        };

        const AnimClip* clip = clips.empty() ? nullptr : &clips[clipIdx].clip;
        float dur = clip ? clip->duration() : 1.0f;
        ModelPose mp{};

        if (!graphMode) {
            // Clip: play/pause, scrub, clip switch.
            bool sp = glfwGetKey(w, GLFW_KEY_SPACE) == GLFW_PRESS;
            if (sp && !spacePrev) playing = !playing;
            spacePrev = sp;
            if (glfwGetKey(w, GLFW_KEY_LEFT) == GLFW_PRESS) { playing = false; t -= dur * 0.01f; }
            if (glfwGetKey(w, GLFW_KEY_RIGHT) == GLFW_PRESS) { playing = false; t += dur * 0.01f; }
            int nClips = static_cast<int>(clips.size());
            bool upK = glfwGetKey(w, GLFW_KEY_UP) == GLFW_PRESS;
            bool downK = glfwGetKey(w, GLFW_KEY_DOWN) == GLFW_PRESS;
            if (nClips && upK && !upPrev) { clipIdx = (clipIdx + nClips - 1) % nClips; t = 0; }
            if (nClips && downK && !downPrev) { clipIdx = (clipIdx + 1) % nClips; t = 0; }
            upPrev = upK;
            downPrev = downK;
            if (playing && clip) t += dt;
            if (clip) {
                if (clip->looping()) { t = std::fmod(t, dur); if (t < 0) t += dur; }
                else t = std::clamp(t, 0.0f, dur);
            }
            if (clip) {
                Pose pose(rig);
                MolangContext mctx;
                clip->sample(t, rig, pose, mctx);
                mp = toMP(pose);
            }
        } else {
            // Graph: mock the blackboard. W ramps speed (idle->walk->run), Space jumps,
            // H waves, G toggles on-ground — then drive the state machine.
            float target = glfwGetKey(w, GLFW_KEY_W) == GLFW_PRESS ? 7.5f : 0.0f;
            gSpeed += (target - gSpeed) * std::min(1.0f, dt * 3.0f);
            bool jk = glfwGetKey(w, GLFW_KEY_SPACE) == GLFW_PRESS;
            if (jk && !jPrev && gOnGround) bb.fire("jump_pressed");
            jPrev = jk;
            bool hk = glfwGetKey(w, GLFW_KEY_H) == GLFW_PRESS;
            if (hk && !hPrev) bb.fire("wave_pressed");
            hPrev = hk;
            bool gk = glfwGetKey(w, GLFW_KEY_G) == GLFW_PRESS;
            if (gk && !gPrev) gOnGround = !gOnGround;
            gPrev = gk;
            bb.setFloat("speed", gSpeed);
            bb.setBool("is_on_ground", gOnGround);
            gStride += gSpeed * dt;
            float phase = std::fmod(gStride / 2.0f, 1.0f);
            Pose gp(rig);
            graph.update(dt, bb, phase, rig, gp);
            bb.clearTriggers();
            mp = toMP(gp);
        }

        // --- Render one frame. ---
        VK_CHECK(vkWaitForFences(ctx.device, 1, &inFlight[frame], VK_TRUE, UINT64_MAX));
        uint32_t img = 0;
        VkResult acq = vkAcquireNextImageKHR(ctx.device, sc.handle, UINT64_MAX, imageAvail[frame],
                                             VK_NULL_HANDLE, &img);
        if (acq == VK_ERROR_OUT_OF_DATE_KHR) { recreate(); continue; }
        if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) break;

        // Camera UBO (orbit).
        float aspect = static_cast<float>(sc.extent.width) / static_cast<float>(sc.extent.height);
        glm::vec3 center(0.0f, 0.95f, 0.0f);
        glm::vec3 eye = center + dist * glm::vec3(std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                                                  std::cos(pitch) * std::cos(yaw));
        CameraUBO cam;
        cam.view = glm::lookAt(eye, center, glm::vec3(0, 1, 0));
        cam.proj = glm::perspective(glm::radians(45.0f), aspect, 0.05f, 100.0f);
        cam.proj[1][1] *= -1.0f;
        std::memcpy(cameraUBO[frame].mapped, &cam, sizeof(cam));

        // --- UI: clip timeline or the graph state/event timeline. ---
        float W = static_cast<float>(sc.extent.width), H = static_cast<float>(sc.extent.height);
        float pad = 18.0f;
        ui.beginFrame(sc.extent.width, sc.extent.height);
        const glm::vec4 kTxt(0.85f, 0.85f, 0.9f, 1.0f), kWhite(1.0f);

        if (!graphMode) {
            font.drawText(ui, "[Tab] graph debugger   Space play/pause   < >  scrub   up/down clip   "
                              "drag orbit   wheel zoom   R reset",
                          10, 10, 1.5f, kTxt);
            float barH = 52.0f, barW = W - 2 * pad;
            ui.quad(0, H - barH, W, barH, glm::vec4(0.08f, 0.08f, 0.11f, 0.9f));
            float ty = H - barH + 30.0f;
            ui.quad(pad, ty, barW, 8.0f, glm::vec4(0.22f, 0.22f, 0.26f, 1.0f));
            float frac = dur > 0.0f ? t / dur : 0.0f;
            ui.quad(pad, ty, barW * frac, 8.0f, glm::vec4(0.30f, 0.70f, 1.0f, 1.0f));
            ui.quad(pad + barW * frac - 1.5f, ty - 5.0f, 3.0f, 18.0f, kWhite);
            char label[160];
            std::snprintf(label, sizeof(label), "%s    %.2f / %.2fs   %s",
                          clips.empty() ? "(no clips)" : clips[clipIdx].name.c_str(), t, dur,
                          playing ? "> playing" : "|| paused");
            font.drawText(ui, label, pad, H - barH + 8.0f, 2.0f, kWhite);
        } else {
            font.drawText(ui, "[Tab] clip preview    hold W speed   Space jump   H wave   G on-ground   "
                              "drag orbit   wheel zoom",
                          10, 10, 1.5f, kTxt);
            char hud[160];
            std::snprintf(hud, sizeof(hud), "speed %.1f   on_ground %s", gSpeed,
                          gOnGround ? "yes" : "NO");
            font.drawText(ui, hud, 10, 30, 1.5f, glm::vec4(0.7f, 0.9f, 0.7f, 1.0f));

            // Timeline: per-layer state bars scrolling right, event ticks, over ~8s.
            std::vector<AnimGraph::GraphSnapshot> hist;
            graph.history(hist);
            const float kWindow = 8.0f;
            int nLayers = static_cast<int>(layerNames.size());
            float rowH = 26.0f, evRow = 16.0f, top = 46.0f;
            float area = nLayers * rowH + evRow + 20.0f;
            float tlY = H - area, tlX = pad + 96.0f, tlW = W - tlX - pad;
            ui.quad(0, tlY, W, area, glm::vec4(0.07f, 0.07f, 0.10f, 0.92f));
            float tNow = hist.empty() ? 0.0f : hist.back().time;
            float tStart = tNow - kWindow;
            auto xOf = [&](float tt) { return tlX + (tt - tStart) / kWindow * tlW; };

            for (int r = 0; r < nLayers; ++r) {
                float ry = tlY + 8.0f + r * rowH;
                ui.quad(tlX, ry, tlW, rowH - 4.0f, glm::vec4(0.12f, 0.12f, 0.15f, 1.0f));
                font.drawText(ui, layerNames[r], 8.0f, ry + 4.0f, 1.5f, kTxt);
                for (size_t s = 0; s + 1 < hist.size(); ++s) {
                    if (r >= static_cast<int>(hist[s].layers.size())) continue;
                    if (hist[s].time < tStart) continue;
                    float x0 = xOf(hist[s].time), x1 = xOf(hist[s + 1].time);
                    glm::vec4 c = stateColor(hist[s].layers[r].state);
                    c.a = 0.35f + 0.65f * std::clamp(hist[s].layers[r].weight, 0.0f, 1.0f);
                    ui.quad(x0, ry, std::max(1.0f, x1 - x0), rowH - 4.0f, c);
                }
                if (r < static_cast<int>(graph.latestSnapshot().layers.size()))
                    font.drawText(ui, graph.currentState(r), tlX + tlW - 120.0f, ry + 4.0f, 1.5f,
                                  kWhite);
            }
            // Event ticks row.
            float ey = tlY + 8.0f + nLayers * rowH;
            font.drawText(ui, "events", 8.0f, ey + 2.0f, 1.5f, kTxt);
            for (const AnimGraph::GraphSnapshot& s : hist)
                if (s.eventCount > 0 && s.time >= tStart)
                    ui.quad(xOf(s.time) - 1.0f, ey, 2.0f, evRow, glm::vec4(1.0f, 0.8f, 0.2f, 1.0f));
            // "now" playhead.
            ui.quad(tlX + tlW - 1.0f, tlY, 2.0f, area, glm::vec4(1.0f, 1.0f, 1.0f, 0.6f));
        }

        VkCommandBuffer cmd = cmds[frame];
        VK_CHECK(vkResetFences(ctx.device, 1, &inFlight[frame]));
        VK_CHECK(vkResetCommandBuffer(cmd, 0));
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

        barrier(cmd, sc.images[img], VK_IMAGE_ASPECT_COLOR_BIT,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        barrier(cmd, sc.depthImage, VK_IMAGE_ASPECT_DEPTH_BIT,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView = sc.imageViews[img];
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0.13f, 0.14f, 0.17f, 1.0f}};
        VkRenderingAttachmentInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth.imageView = sc.depthView;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo ri{};
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent = sc.extent;
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &ri);

        VkViewport vp{};
        vp.width = W;
        vp.height = H;
        vp.maxDepth = 1.0f;
        VkRect2D scissor{{0, 0}, sc.extent};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        if (clip) model.record(cmd, frame, mp);
        ui.recordDraw(cmd, frame);

        vkCmdEndRendering(cmd);
        barrier(cmd, sc.images[img], VK_IMAGE_ASPECT_COLOR_BIT,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        VK_CHECK(vkEndCommandBuffer(cmd));

        VkCommandBufferSubmitInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        ci.commandBuffer = cmd;
        VkSemaphoreSubmitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait.semaphore = imageAvail[frame];
        wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphoreSubmitInfo signal{};
        signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal.semaphore = renderDone[img];
        signal.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.waitSemaphoreInfoCount = 1;
        submit.pWaitSemaphoreInfos = &wait;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &ci;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;
        VK_CHECK(vkQueueSubmit2(ctx.graphicsQueue, 1, &submit, inFlight[frame]));

        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &renderDone[img];
        present.swapchainCount = 1;
        present.pSwapchains = &sc.handle;
        present.pImageIndices = &img;
        VkResult pres = vkQueuePresentKHR(ctx.graphicsQueue, &present);
        if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR || window.consumeResized())
            recreate();
        frame = (frame + 1) % kFrames;
    }

    vkDeviceWaitIdle(ctx.device);
    for (int i = 0; i < kFrames; ++i) {
        vkDestroySemaphore(ctx.device, imageAvail[i], nullptr);
        vkDestroyFence(ctx.device, inFlight[i], nullptr);
        destroyBuffer(ctx.allocator, cameraUBO[i]);
    }
    for (VkSemaphore s : renderDone)
        if (s) vkDestroySemaphore(ctx.device, s, nullptr);
    vkDestroyCommandPool(ctx.device, pool, nullptr);
    ui.cleanup();
    model.cleanup();
    sc.cleanup();
    ctx.cleanup();
    window.cleanup();
    return 0;
}
