#pragma once

#include "gfx/GpuBuffer.h"
#include "gfx/vk_common.h"

#include <string>

typedef struct VmaAllocation_T* VmaAllocation;

namespace mc {

class VkContext;
class Swapchain;

// Vanilla title-screen background: the six gui/title/background/panorama_0..5.png
// faces drawn as a cube around the camera, slowly spinning. Drawn first in the frame
// with no depth, covering everything behind it. valid() is false when the textures
// are missing (the caller falls back to the live world panorama).
class PanoramaRenderer {
public:
    void init(VkContext* ctx, Swapchain* sc, const std::string& backgroundDir);
    void cleanup();

    bool valid() const { return valid_; }

    // time drives the spin; aspect = swapchain width/height.
    void record(VkCommandBuffer cmd, float time, float aspect);

private:
    struct Texture {
        VkImage         image = VK_NULL_HANDLE;
        VmaAllocation   alloc = nullptr;
        VkImageView     view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };

    VkContext* ctx_ = nullptr;
    Swapchain* sc_ = nullptr;
    bool valid_ = false;

    Texture faces_[6]{};
    AllocatedBuffer cubeVbuf_{}; // 6 faces x 6 verts, inward-facing

    VkSampler             sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout texLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      pool_ = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline            pipeline_ = VK_NULL_HANDLE;

    bool loadTexture(const std::string& path, Texture* out);
    VkDescriptorSet allocTexSet(VkImageView view);
    void createGeometry();
    void createPipeline();
    VkShaderModule loadShader(const std::string& path);
};

} // namespace mc
