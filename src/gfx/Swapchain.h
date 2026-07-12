#pragma once

#include "gfx/vk_common.h"

#include <vector>

// Forward-declare opaque VMA allocation handle (matches VK_DEFINE_HANDLE).
typedef struct VmaAllocation_T* VmaAllocation;

namespace mc {

class VkContext;

// Owns the swapchain, its per-image color views, and a shared depth attachment.
class Swapchain {
public:
    void init(VkContext* ctx, uint32_t width, uint32_t height);
    void recreate(uint32_t width, uint32_t height);
    void cleanup();

    VkSwapchainKHR handle      = VK_NULL_HANDLE;
    VkFormat       imageFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D     extent{};

    std::vector<VkImage>     images;
    std::vector<VkImageView> imageViews;

    VkImage     depthImage  = VK_NULL_HANDLE;
    VkImageView depthView   = VK_NULL_HANDLE;
    VkFormat    depthFormat = VK_FORMAT_D32_SFLOAT;

    uint32_t imageCount() const { return static_cast<uint32_t>(images.size()); }

private:
    VkContext*    ctx_ = nullptr;
    VmaAllocation depthAllocation_ = nullptr;

    void create(uint32_t width, uint32_t height);
    void destroy();
};

} // namespace mc
