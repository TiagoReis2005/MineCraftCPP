#include "gfx/Swapchain.h"

#include "gfx/VkContext.h"

#include <vk_mem_alloc.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace mc {
namespace {

VkSurfaceFormatKHR chooseFormat(const std::vector<VkSurfaceFormatKHR>& formats) {
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return f;
        }
    }
    return formats.front();
}

VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& modes) {
    for (const auto& m : modes) {
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) return m; // low-latency triple buffering
    }
    return VK_PRESENT_MODE_FIFO_KHR; // always supported (vsync)
}

VkExtent2D chooseExtent(const VkSurfaceCapabilitiesKHR& caps, uint32_t width, uint32_t height) {
    if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return caps.currentExtent;
    }
    VkExtent2D extent{width, height};
    extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    return extent;
}

} // namespace

void Swapchain::init(VkContext* ctx, uint32_t width, uint32_t height) {
    ctx_ = ctx;
    create(width, height);
}

void Swapchain::recreate(uint32_t width, uint32_t height) {
    vkDeviceWaitIdle(ctx_->device);
    destroy();
    create(width, height);
}

void Swapchain::create(uint32_t width, uint32_t height) {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx_->physicalDevice, ctx_->surface, &caps);

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_->physicalDevice, ctx_->surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx_->physicalDevice, ctx_->surface, &formatCount, formats.data());

    uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx_->physicalDevice, ctx_->surface, &modeCount, nullptr);
    std::vector<VkPresentModeKHR> modes(modeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(ctx_->physicalDevice, ctx_->surface, &modeCount, modes.data());

    VkSurfaceFormatKHR surfaceFormat = chooseFormat(formats);
    VkPresentModeKHR presentMode = choosePresentMode(modes);
    extent = chooseExtent(caps, width, height);
    imageFormat = surfaceFormat.format;

    uint32_t desired = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && desired > caps.maxImageCount) {
        desired = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = ctx_->surface;
    ci.minImageCount = desired;
    ci.imageFormat = surfaceFormat.format;
    ci.imageColorSpace = surfaceFormat.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; // single graphics+present family
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = presentMode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = VK_NULL_HANDLE;

    VK_CHECK(vkCreateSwapchainKHR(ctx_->device, &ci, nullptr, &handle));

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(ctx_->device, handle, &count, nullptr);
    images.resize(count);
    vkGetSwapchainImagesKHR(ctx_->device, handle, &count, images.data());

    imageViews.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = images[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = imageFormat;
        vci.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                          VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.baseMipLevel = 0;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.baseArrayLayer = 0;
        vci.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(ctx_->device, &vci, nullptr, &imageViews[i]));
    }

    // Depth attachment shared by all frames (cleared each frame).
    VkImageCreateInfo depthInfo{};
    depthInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depthInfo.imageType = VK_IMAGE_TYPE_2D;
    depthInfo.format = depthFormat;
    depthInfo.extent = {extent.width, extent.height, 1};
    depthInfo.mipLevels = 1;
    depthInfo.arrayLayers = 1;
    depthInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    depthInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    // SAMPLED: the decal pass reads scene depth to project onto visible surfaces.
    depthInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    VmaAllocationCreateInfo depthAlloc{};
    depthAlloc.usage = VMA_MEMORY_USAGE_AUTO;
    depthAlloc.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VK_CHECK(vmaCreateImage(ctx_->allocator, &depthInfo, &depthAlloc, &depthImage,
                            &depthAllocation_, nullptr));

    VkImageViewCreateInfo depthViewInfo{};
    depthViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depthViewInfo.image = depthImage;
    depthViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depthViewInfo.format = depthFormat;
    depthViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthViewInfo.subresourceRange.baseMipLevel = 0;
    depthViewInfo.subresourceRange.levelCount = 1;
    depthViewInfo.subresourceRange.baseArrayLayer = 0;
    depthViewInfo.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(ctx_->device, &depthViewInfo, nullptr, &depthView));
}

void Swapchain::destroy() {
    if (depthView) {
        vkDestroyImageView(ctx_->device, depthView, nullptr);
        depthView = VK_NULL_HANDLE;
    }
    if (depthImage) {
        vmaDestroyImage(ctx_->allocator, depthImage, depthAllocation_);
        depthImage = VK_NULL_HANDLE;
        depthAllocation_ = nullptr;
    }
    for (VkImageView view : imageViews) {
        vkDestroyImageView(ctx_->device, view, nullptr);
    }
    imageViews.clear();
    images.clear();
    if (handle) {
        vkDestroySwapchainKHR(ctx_->device, handle, nullptr);
        handle = VK_NULL_HANDLE;
    }
}

void Swapchain::cleanup() {
    destroy();
}

} // namespace mc
