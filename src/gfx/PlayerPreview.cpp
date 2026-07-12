#include "gfx/PlayerPreview.h"

#include "gfx/Swapchain.h"
#include "gfx/VkContext.h"

#include <vk_mem_alloc.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cstring>

namespace mc {
namespace {

struct CameraUBOData {
    glm::mat4 view;
    glm::mat4 proj;
};

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                  VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

void PlayerPreview::init(VkContext* ctx, Swapchain* sc, const std::string& skinPath,
                         const PlayerRigMeshes& meshes) {
    ctx_ = ctx;
    sc_ = sc;

    // Color target (same format as the swapchain so ModelRenderer's pipeline matches).
    VkImageCreateInfo color{};
    color.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    color.imageType = VK_IMAGE_TYPE_2D;
    color.format = sc_->imageFormat;
    color.extent = {kW, kH, 1};
    color.mipLevels = 1;
    color.arrayLayers = 1;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.tiling = VK_IMAGE_TILING_OPTIMAL;
    color.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    alloc.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VK_CHECK(vmaCreateImage(ctx_->allocator, &color, &alloc, &colorImage_, &colorAlloc_, nullptr));

    VkImageViewCreateInfo cview{};
    cview.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    cview.image = colorImage_;
    cview.viewType = VK_IMAGE_VIEW_TYPE_2D;
    cview.format = sc_->imageFormat;
    cview.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(ctx_->device, &cview, nullptr, &colorView_));

    VkImageCreateInfo depth = color;
    depth.format = sc_->depthFormat;
    depth.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VK_CHECK(vmaCreateImage(ctx_->allocator, &depth, &alloc, &depthImage_, &depthAlloc_, nullptr));

    VkImageViewCreateInfo dview = cview;
    dview.image = depthImage_;
    dview.format = sc_->depthFormat;
    dview.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    VK_CHECK(vkCreateImageView(ctx_->device, &dview, nullptr, &depthView_));

    // Fixed straight-on camera: the pose places the model with its feet at the origin
    // (1.85 tall); a mild 30-degree lens at 1:2 aspect frames the full body with the
    // arms just inside the sides.
    CameraUBOData cam{};
    cam.view = glm::lookAt(glm::vec3(0.0f, 0.98f, 4.1f), glm::vec3(0.0f, 0.93f, 0.0f),
                           glm::vec3(0, 1, 0));
    cam.proj = glm::perspective(glm::radians(30.0f),
                                static_cast<float>(kW) / static_cast<float>(kH), 0.1f, 20.0f);
    cam.proj[1][1] *= -1.0f;
    for (int i = 0; i < kFramesInFlight; ++i) {
        ubos_[i] = createHostBuffer(ctx_->allocator, sizeof(CameraUBOData),
                                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        std::memcpy(ubos_[i].mapped, &cam, sizeof(cam));
    }

    model_.init(ctx_, sc_, ubos_, skinPath, meshes);
    hasModel_ = true;
}

void PlayerPreview::rebuildSkin(const std::string& skinPath, const PlayerRigMeshes& meshes) {
    if (hasModel_) model_.cleanup();
    model_.init(ctx_, sc_, ubos_, skinPath, meshes);
    hasModel_ = true;
}

void PlayerPreview::record(VkCommandBuffer cmd, uint32_t frameIndex, const ModelPose& pose) {
    if (!hasModel_) return;

    imageBarrier(cmd, colorImage_, VK_IMAGE_ASPECT_COLOR_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    imageBarrier(cmd, depthImage_, VK_IMAGE_ASPECT_DEPTH_BIT,
                 VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, 0,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo colorAtt{};
    colorAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAtt.imageView = colorView_;
    colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAtt.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}}; // transparent background

    VkRenderingAttachmentInfo depthAtt{};
    depthAtt.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAtt.imageView = depthView_;
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAtt.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.extent = {kW, kH};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &colorAtt;
    rendering.pDepthAttachment = &depthAtt;
    vkCmdBeginRendering(cmd, &rendering);

    VkViewport vp{};
    vp.width = static_cast<float>(kW);
    vp.height = static_cast<float>(kH);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D scissor{};
    scissor.extent = {kW, kH};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    model_.record(cmd, frameIndex, pose);

    vkCmdEndRendering(cmd);

    imageBarrier(cmd, colorImage_, VK_IMAGE_ASPECT_COLOR_BIT,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void PlayerPreview::cleanup() {
    if (!ctx_) return;
    if (hasModel_) model_.cleanup();
    hasModel_ = false;
    for (AllocatedBuffer& b : ubos_) destroyBuffer(ctx_->allocator, b);
    if (depthView_) vkDestroyImageView(ctx_->device, depthView_, nullptr);
    if (depthImage_) vmaDestroyImage(ctx_->allocator, depthImage_, depthAlloc_);
    if (colorView_) vkDestroyImageView(ctx_->device, colorView_, nullptr);
    if (colorImage_) vmaDestroyImage(ctx_->allocator, colorImage_, colorAlloc_);
    colorImage_ = VK_NULL_HANDLE;
    colorView_ = VK_NULL_HANDLE;
    depthImage_ = VK_NULL_HANDLE;
    depthView_ = VK_NULL_HANDLE;
}

} // namespace mc
