#pragma once

#include "game/Drops.h" // Drops::Draw (dropped-item instances)
#include "gfx/GpuBuffer.h"
#include "gfx/ModelRenderer.h" // ModelPose (player-preview pass)
#include "gfx/vk_common.h"
#include "world/Block.h" // AABB for outline boxes

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace mc {

class VkContext;
class Swapchain;
class Window;
class Camera;
class TextureArray;
class World;
class UIRenderer;
class ModelRenderer;
struct ModelPose;
class HeldItemRenderer;
class SkyRenderer;
class WeatherRenderer;
class PanoramaRenderer;
class PlayerPreview;

// Records and submits frames using Vulkan 1.3 dynamic rendering + synchronization2.
// M3: renders all loaded chunks of a streaming world with a depth buffer and fly camera.
class Renderer {
public:
    void init(VkContext* ctx, Swapchain* sc, Window* window, TextureArray* texture);
    void cleanup();
    // world may be null (title screen: no session exists) — the world pass, sky and
    // weather are skipped and the panorama/UI carry the frame.
    void drawFrame(const Camera& camera, const World* world, UIRenderer* ui,
                   const std::vector<AABB>& outlineBoxes, // world-space selection boxes
                   ModelRenderer* model, const ModelPose& modelPose, bool modelVisible,
                   HeldItemRenderer* held, const glm::mat4& heldMatrix, uint16_t heldBlock,
                   bool heldVisible);

    // Sky clear color (driven by the day/night cycle).
    void setSkyColor(const glm::vec3& c) { skyColor_ = c; }

    // Celestial pass: sun/moon/stars drawn first in the world pass (set once at init).
    void setSkyRenderer(SkyRenderer* sky) { sky_ = sky; }
    void setCelestial(float sunAngleRad, int moonPhase, float starAlpha, float brightness) {
        sunAngle_ = sunAngleRad;
        moonPhase_ = moonPhase;
        starAlpha_ = starAlpha;
        celestialBrightness_ = brightness;
    }

    // Dropped items this frame, rendered as mini blocks via the held-item renderer.
    void setDropDraws(std::vector<Drops::Draw> draws) { dropDraws_ = std::move(draws); }

    // Mining crack overlays, rendered as SCREEN-SPACE DECALS: each box is a projection
    // volume; the decal pass samples scene depth, reconstructs the surface inside the
    // volume and paints destroy_stage_<stage> onto it (tri-planar, world-anchored).
    // Hidden faces never receive paint — they are not in the depth buffer.
    struct Crack {
        AABB box;      // world-space decal volume (from the block's crackBoxes)
        int stage = 0; // 0..9 -> destroy_stage_N
    };
    void setCracks(std::vector<Crack> cracks) { cracks_ = std::move(cracks); }

    // Title-screen panorama backdrop: while active (and the art exists) it replaces the
    // sky/world/weather entirely — only the player-model preview and UI draw over it.
    void setPanoramaRenderer(PanoramaRenderer* p) { panorama_ = p; }
    void setTitlePanorama(bool active, float time) {
        panoramaActive_ = active;
        panoramaTime_ = time;
    }

    // Offscreen player preview (title screen + inventory player box), rendered before
    // the main pass whenever active so the UI can sample its texture.
    void setPlayerPreviewRenderer(PlayerPreview* p) { playerPreview_ = p; }
    void setPlayerPreview(bool active, const ModelPose& pose) {
        previewActive_ = active;
        previewPose_ = pose;
    }

    // Rain curtain, drawn after the world (set once at init; per-frame state below).
    void setWeatherRenderer(WeatherRenderer* weather) { weather_ = weather; }
    void setWeather(float intensity, float time, float cloudBrightness) {
        weatherIntensity_ = intensity;
        weatherTime_ = time;
        cloudBrightness_ = cloudBrightness;
    }

    // Per-frame camera UBOs (view+proj), shared with the model renderer.
    const AllocatedBuffer* cameraUBOs() const { return cameraUBO_; }

    // Chunks that survived frustum culling and were actually drawn last frame (F3).
    int lastRenderedChunks() const { return renderedChunks_; }

    // Profiling (ms): time blocked waiting on the GPU (high => GPU-bound) and time spent
    // recording the command buffer on the CPU (high => too many draw calls / CPU draw-bound).
    double gpuWaitMs() const { return gpuWaitMs_; }
    double recordMs() const { return recordMs_; }

private:
    static constexpr int kFramesInFlight = 2;

    VkContext*    ctx_     = nullptr;
    Swapchain*    sc_      = nullptr;
    Window*       window_  = nullptr;
    TextureArray* texture_ = nullptr;

    VkCommandPool   commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffers_[kFramesInFlight]{};
    VkSemaphore     imageAvailable_[kFramesInFlight]{};
    VkFence         inFlight_[kFramesInFlight]{};
    std::vector<VkSemaphore> renderFinished_;

    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      descriptorPool_      = VK_NULL_HANDLE;
    VkDescriptorSet       descriptorSets_[kFramesInFlight]{};
    AllocatedBuffer       cameraUBO_[kFramesInFlight]{};

    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline       pipeline_       = VK_NULL_HANDLE;

    // Selection outline: floating wireframe bars (GEOMETRY, not a decal — the outline
    // is the selection VOLUME and legitimately floats in air where a fence's box has
    // no mesh behind it; decals can only paint existing surfaces). The unit cube is
    // shared with decal volumes and occlusion query boxes.
    VkPipeline      outlinePipeline_ = VK_NULL_HANDLE;
    AllocatedBuffer outlineVbuf_{};
    uint32_t        outlineVertexCount_ = 0;

    // Screen-space decal pass (mining cracks now; scorch marks/splatters later): its own
    // descriptor set adds sampled scene DEPTH to the camera UBO + block textures. Depth
    // is COPIED into a plain sampled image between passes — sampling the live attachment
    // in a read-only layout gave unreliable (compressed) reads on AMD.
    VkDescriptorSetLayout decalSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      decalPool_ = VK_NULL_HANDLE;
    VkDescriptorSet       decalSets_[kFramesInFlight]{};
    VkSampler             depthSampler_ = VK_NULL_HANDLE;
    VkPipelineLayout      decalPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline            decalPipeline_ = VK_NULL_HANDLE;
    VkImage               depthCopy_ = VK_NULL_HANDLE; // swapchain-sized, D32, sampled
    VmaAllocation         depthCopyAlloc_ = nullptr;
    VkImageView           depthCopyView_ = VK_NULL_HANDLE;
    std::vector<Crack> cracks_;

    VkPipeline       lodPipeline_       = VK_NULL_HANDLE; // far distant-horizon terrain
    VkPipelineLayout lodPipelineLayout_ = VK_NULL_HANDLE; // camera set (no push)

    // GPU occlusion culling: each drawn chunk's box is queried against the depth buffer;
    // chunks with zero visible samples are skipped ~2 frames later. One query pool per
    // frame-in-flight so results are read without a stall (the slot's fence already waited).
    static constexpr uint32_t kMaxOcclusionQueries = 16384;
    VkQueryPool occlusionPool_[kFramesInFlight]{};
    VkPipeline  occlusionPipeline_ = VK_NULL_HANDLE; // depth-tested box, no color/depth write
    std::vector<glm::ivec3> querySlotCoord_[kFramesInFlight]; // slot -> chunk coord recorded
    void createOcclusion();
    void applyOcclusionResults(const World* world);

    uint32_t currentFrame_ = 0;
    glm::vec3 skyColor_{0.49f, 0.67f, 0.96f};

    // View frustum (6 planes, ax+by+cz+d, pointing inward) rebuilt each frame from
    // proj*view; chunks/LOD tiles whose AABB falls fully outside are skipped.
    glm::vec4 frustumPlanes_[6]{};
    int renderedChunks_ = 0;
    double gpuWaitMs_ = 0.0, recordMs_ = 0.0; // per-frame profiling
    bool aabbInFrustum(const glm::vec3& mn, const glm::vec3& mx) const;

    SkyRenderer* sky_ = nullptr;
    float     sunAngle_ = 0.0f;
    int       moonPhase_ = 0;
    float     starAlpha_ = 0.0f;
    float     celestialBrightness_ = 1.0f;
    glm::vec3 camEye_{0.0f}; // view position this frame (sky centers on it)
    glm::vec3 camFront_{0.0f, 0.0f, -1.0f}; // view direction this frame
    // Occlusion queries are re-recorded only when the camera moved/turned past a threshold
    // (tiny nudges aren't worth the GPU work); visibility is frozen until then.
    glm::vec3 lastQueryEye_{1e9f};
    glm::vec3 lastQueryFront_{0.0f};

    std::vector<Drops::Draw> dropDraws_;

    WeatherRenderer* weather_ = nullptr;
    float weatherIntensity_ = 0.0f;
    float weatherTime_ = 0.0f;
    float cloudBrightness_ = 1.0f;

    PanoramaRenderer* panorama_ = nullptr;
    bool panoramaActive_ = false;
    float panoramaTime_ = 0.0f;

    PlayerPreview* playerPreview_ = nullptr;
    bool previewActive_ = false;
    ModelPose previewPose_{};

    void createCommands();
    void createSync();
    void createRenderFinished();
    void destroyRenderFinished();
    void createDescriptors();
    void createPipeline();
    void createOutline();
    void createDecal();
    void createDepthCopy();
    void destroyDepthCopy();
    void createLod();
    void updateCameraUBO(const Camera& camera);
    void recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex, const World* world,
                             UIRenderer* ui, const std::vector<AABB>& outlineBoxes,
                             ModelRenderer* model, const ModelPose& modelPose, bool modelVisible,
                             HeldItemRenderer* held, const glm::mat4& heldMatrix,
                             uint16_t heldBlock, bool heldVisible);
    void recreateSwapchain();
    VkShaderModule loadShaderModule(const std::string& path);
};

} // namespace mc
