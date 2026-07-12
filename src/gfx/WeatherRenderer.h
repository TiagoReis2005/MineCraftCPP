#pragma once

#include "gfx/GpuBuffer.h"
#include "gfx/vk_common.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

typedef struct VmaAllocation_T* VmaAllocation;

namespace mc {

class VkContext;
class Swapchain;
class World;

// Rain curtain: one camera-facing quad per world column around the player, spanning
// from the column's first solid block up past the camera, with the rain texture
// scrolling downward. Rebuilt every frame into a host vertex buffer; alpha blended and
// depth tested (drawn after the world) so walls and roofs hide it -- the column scan
// also stops rain under overhangs.
class WeatherRenderer {
public:
    static constexpr int kFramesInFlight = 2;

    void init(VkContext* ctx, Swapchain* sc, const AllocatedBuffer* cameraUBOs,
              const std::string& envDir);
    void cleanup();

    // Also draws vanilla "fancy" 3D clouds: each clouds.png texel = a 12x12x4 box at
    // y=192, meshed from the alpha mask around the camera (sides only on cloud edges),
    // shaded per face (top 1.0 / bottom 0.7 / x 0.9 / z 0.8) and drifting slowly.
    // cloudBrightness dims them at night and under rain.
    void record(VkCommandBuffer cmd, uint32_t frameIndex, const glm::vec3& camPos,
                const World& world, float time, float intensity, float cloudBrightness);

private:
    static constexpr int kRadius = 8; // columns of rain around the player
    static constexpr uint32_t kMaxVerts = (2 * kRadius + 1) * (2 * kRadius + 1) * 6 + 6;

    VkContext* ctx_ = nullptr;
    Swapchain* sc_ = nullptr;

    VkImage       rainImage_ = VK_NULL_HANDLE;
    VmaAllocation rainAlloc_ = nullptr;
    VkImageView   rainView_ = VK_NULL_HANDLE;
    VkDescriptorSet rainSet_ = VK_NULL_HANDLE;

    VkImage       cloudImage_ = VK_NULL_HANDLE;
    VmaAllocation cloudAlloc_ = nullptr;
    VkImageView   cloudView_ = VK_NULL_HANDLE;
    VkDescriptorSet cloudSet_ = VK_NULL_HANDLE;

    struct CloudVertex { // same layout as the rain vertices (pos + uv)
        glm::vec3 pos;
        glm::vec2 uv;
    };
    static constexpr int kCloudRadius = 30;       // default cells around the camera (12 blocks each)
    static constexpr int kCloudMaxRadius = 96;    // cap on the dynamic radius (buffer sizing)
    static constexpr float kCloudCell = 12.0f;    // blocks per texel
    static constexpr float kCloudThick = 4.0f;    // box height
    static constexpr float kCloudY = 192.0f;
    // Clouds are dirt-cheap flat boxes, so no LOD: the detailed radius just grows to cover
    // the view distance. ~20 verts/cell budget (interior cells emit top+bottom only; sides
    // only on cloud edges), with an emission guard so it can never overflow.
    static constexpr uint32_t kMaxCloudVerts =
        (2 * kCloudMaxRadius + 1) * (2 * kCloudMaxRadius + 1) * 20;

    std::vector<uint8_t> cloudMask_; // alpha > 127 per texel
    int maskW_ = 0, maskH_ = 0;
    int cloudRadius_ = kCloudRadius;      // current dynamic radius (tracks view distance)
    std::vector<CloudVertex> cloudVerts_; // cached mesh, local to the anchor cell
    uint32_t cloudCounts_[4]{};           // top / bottom / x sides / z sides
    long long anchorX_ = 0, anchorZ_ = 0;
    bool anchorValid_ = false;
    AllocatedBuffer cloudVbuf_[kFramesInFlight]{};
    VkPipeline cloudPipeline_ = VK_NULL_HANDLE; // depth-write ON (self-occluding boxes)

    AllocatedBuffer vbuf_[kFramesInFlight]{};

    VkSampler             sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout uboLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout texLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      pool_ = VK_NULL_HANDLE;
    VkDescriptorSet       uboSets_[kFramesInFlight]{};

    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline       pipeline_ = VK_NULL_HANDLE;

    bool loadTexture(const std::string& path, VkImage& image, VmaAllocation& alloc,
                     VkImageView& view, VkDescriptorSet& set,
                     std::vector<uint8_t>* maskOut = nullptr);
    void buildCloudMesh(long long acx, long long acz);
    void createDescriptors(const AllocatedBuffer* cameraUBOs);
    VkPipeline createPipeline(bool depthWrite);
    VkShaderModule loadShader(const std::string& path);
};

} // namespace mc
