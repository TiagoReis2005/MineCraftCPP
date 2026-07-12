#pragma once

#include "game/PlayerModel.h"
#include "gfx/GpuBuffer.h"
#include "gfx/ModelRenderer.h"
#include "gfx/vk_common.h"

#include <string>

typedef struct VmaAllocation_T* VmaAllocation;

namespace mc {

class VkContext;
class Swapchain;

// Renders the posed player model into a small offscreen texture (transparent
// background, fixed straight-on camera) so menus can draw it as a plain UI sprite:
// the title-screen skin preview and the inventory screens' player box.
class PlayerPreview {
public:
    // Tall 1:2 target: the standing model fills the height, arms fit the width.
    // Consumers draw it letterboxed (width = height / 2) to avoid distortion.
    static constexpr uint32_t kW = 256, kH = 512;
    static constexpr int kFramesInFlight = 2;

    void init(VkContext* ctx, Swapchain* sc, const std::string& skinPath,
              const PlayerRigMeshes& meshes);
    void cleanup();

    // Swap the skin/arm-width live (caller has already vkDeviceWaitIdle'd).
    void rebuildSkin(const std::string& skinPath, const PlayerRigMeshes& meshes);

    // Records the offscreen pass; call BEFORE the main render pass begins.
    void record(VkCommandBuffer cmd, uint32_t frameIndex, const ModelPose& pose);

    VkImageView view() const { return colorView_; }

private:
    VkContext* ctx_ = nullptr;
    Swapchain* sc_ = nullptr;

    VkImage       colorImage_ = VK_NULL_HANDLE;
    VmaAllocation colorAlloc_ = nullptr;
    VkImageView   colorView_ = VK_NULL_HANDLE;
    VkImage       depthImage_ = VK_NULL_HANDLE;
    VmaAllocation depthAlloc_ = nullptr;
    VkImageView   depthView_ = VK_NULL_HANDLE;

    AllocatedBuffer ubos_[kFramesInFlight]{}; // fixed preview camera (view + proj)
    ModelRenderer model_;
    bool hasModel_ = false;
};

} // namespace mc
