#pragma once

#include "gfx/GpuBuffer.h"
#include "gfx/vk_common.h"

#include <glm/glm.hpp>

#include <string>

typedef struct VmaAllocation_T* VmaAllocation;

namespace mc {

class VkContext;
class Swapchain;

// Sun, moon (8 phase textures) and a procedural star field, drawn first in the world
// pass with no depth so terrain covers them. Everything rides one rotation around the
// world Z axis driven by GameTime::sunAngle (0 = sun on the east horizon, pi/2 = noon);
// the moon sits on the opposite side. Additive blending like Minecraft.
class SkyRenderer {
public:
    static constexpr int kFramesInFlight = 2;

    void init(VkContext* ctx, Swapchain* sc, const AllocatedBuffer* cameraUBOs,
              const std::string& envDir);
    void cleanup();

    // brightness < 1 dims the sun/moon (rain clouds); stars fade via starAlpha.
    void record(VkCommandBuffer cmd, uint32_t frameIndex, const glm::vec3& camPos,
                float sunAngle, int moonPhase, float starAlpha, float brightness);

private:
    struct Texture {
        VkImage         image = VK_NULL_HANDLE;
        VmaAllocation   alloc = nullptr;
        VkImageView     view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE; // set 1: just the sampler binding
    };

    VkContext* ctx_ = nullptr;
    Swapchain* sc_ = nullptr;

    Texture sun_{};
    Texture moon_[8]{};
    Texture white_{}; // 1x1 white for the stars

    AllocatedBuffer quadVbuf_{};             // unit quad, normal +X (sun/moon billboard)
    AllocatedBuffer starVbuf_{};             // star quads baked on the sky sphere
    uint32_t        starVertexCount_ = 0;

    VkSampler             sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout uboLayout_ = VK_NULL_HANDLE; // set 0: camera UBO
    VkDescriptorSetLayout texLayout_ = VK_NULL_HANDLE; // set 1: texture
    VkDescriptorPool      pool_ = VK_NULL_HANDLE;
    VkDescriptorSet       uboSets_[kFramesInFlight]{};

    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline       pipeline_ = VK_NULL_HANDLE;

    bool loadTexture(const std::string& path, Texture* out); // false + magenta-free skip
    void createWhite(Texture* out);
    VkDescriptorSet allocTexSet(VkImageView view);
    void createGeometry();
    void createDescriptors(const AllocatedBuffer* cameraUBOs);
    void createPipeline();
    void destroyTexture(Texture& t);
    VkShaderModule loadShader(const std::string& path);
};

} // namespace mc
