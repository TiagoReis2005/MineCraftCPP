#include "gfx/Renderer.h"

#include "core/Camera.h"
#include "core/Paths.h"
#include "core/Window.h"
#include "gfx/HeldItemRenderer.h"
#include "gfx/ModelRenderer.h"
#include "gfx/SkyRenderer.h"
#include "gfx/Swapchain.h"
#include "gfx/WeatherRenderer.h"
#include "gfx/PanoramaRenderer.h"
#include "gfx/PlayerPreview.h"
#include "gfx/TextureArray.h"
#include "gfx/UIRenderer.h"
#include "gfx/VkContext.h"
#include "world/Chunk.h"
#include "world/Mesher.h"
#include "world/World.h"

#include <vk_mem_alloc.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <vector>

namespace mc {
namespace {

struct CameraUBOData {
    glm::mat4 view;
    glm::mat4 proj;
    // Decal pass: depth -> VIEW-space position. Reconstructing in view space keeps the
    // numbers small (scene distances, not world coordinates), so per-pixel derivatives
    // stay clean; the shader rotates into world space afterwards.
    glm::mat4 invProj;
};

struct PushConstants {
    glm::vec4 chunkPos; // chunks: offset; outline: box min corner
    glm::vec4 boxSize;  // outline: box extents (unused by the chunk shader)
};

std::vector<char> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("Failed to open shader file: " + path);
    size_t size = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(size);
    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(size));
    return buffer;
}

void transitionImage(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                     VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                     VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                     VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, 0, 1, 0, 1};

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

void Renderer::init(VkContext* ctx, Swapchain* sc, Window* window, TextureArray* texture) {
    ctx_ = ctx;
    sc_ = sc;
    window_ = window;
    texture_ = texture;
    createCommands();
    createSync();
    createRenderFinished();
    createDescriptors();
    createPipeline();
    createOutline();
    createDecal();
    createDepthCopy();
    createLod();
    createOcclusion();
}

void Renderer::createDepthCopy() {
    // Plain sampled D32 image the decal pass reads scene depth from (filled by a
    // vkCmdCopyImage between passes; recreated with the swapchain on resize).
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = sc_->depthFormat;
    info.extent = {sc_->extent.width, sc_->extent.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    alloc.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VK_CHECK(vmaCreateImage(ctx_->allocator, &info, &alloc, &depthCopy_, &depthCopyAlloc_, nullptr));

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = depthCopy_;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = sc_->depthFormat;
    view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(ctx_->device, &view, nullptr, &depthCopyView_));
}

void Renderer::destroyDepthCopy() {
    if (depthCopyView_) vkDestroyImageView(ctx_->device, depthCopyView_, nullptr);
    if (depthCopy_) vmaDestroyImage(ctx_->allocator, depthCopy_, depthCopyAlloc_);
    depthCopyView_ = VK_NULL_HANDLE;
    depthCopy_ = VK_NULL_HANDLE;
    depthCopyAlloc_ = nullptr;
}

void Renderer::createDecal() {
    // Screen-space decal pass (mining cracks; scorch marks/splatters can reuse it):
    // each decal is a projection VOLUME drawn as a unit cube; the fragment shader
    // samples scene depth, reconstructs the surface position and paints the texture
    // onto whatever visible surface lies inside the volume. Faces hidden by neighbors
    // are never painted — they are not in the depth buffer.

    // Depth sampler + a descriptor set = camera UBO + block textures + scene depth.
    // Binding 2 (depth) is repointed every frame so window resizes stay valid.
    VkSamplerCreateInfo samp{};
    samp.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samp.magFilter = VK_FILTER_NEAREST;
    samp.minFilter = VK_FILTER_NEAREST;
    samp.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(ctx_->device, &samp, nullptr, &depthSampler_));

    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 3;
    li.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx_->device, &li, nullptr, &decalSetLayout_));

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = kFramesInFlight;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = kFramesInFlight * 2;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kFramesInFlight;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(ctx_->device, &poolInfo, nullptr, &decalPool_));

    std::array<VkDescriptorSetLayout, kFramesInFlight> layouts;
    layouts.fill(decalSetLayout_);
    VkDescriptorSetAllocateInfo setAlloc{};
    setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAlloc.descriptorPool = decalPool_;
    setAlloc.descriptorSetCount = kFramesInFlight;
    setAlloc.pSetLayouts = layouts.data();
    VK_CHECK(vkAllocateDescriptorSets(ctx_->device, &setAlloc, decalSets_));

    for (int i = 0; i < kFramesInFlight; ++i) {
        VkDescriptorBufferInfo bufInfo{};
        bufInfo.buffer = cameraUBO_[i].buffer;
        bufInfo.offset = 0;
        bufInfo.range = sizeof(CameraUBOData);

        VkDescriptorImageInfo texInfo{};
        texInfo.sampler = texture_->sampler();
        texInfo.imageView = texture_->view();
        texInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = decalSets_[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &bufInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = decalSets_[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &texInfo;
        vkUpdateDescriptorSets(ctx_->device, 2, writes, 0, nullptr);
    }

    // Same 2-vec4 push constants, but visible to BOTH stages (frag needs box + layer).
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &decalSetLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(ctx_->device, &layoutInfo, nullptr, &decalPipelineLayout_));

    VkShaderModule vert = loadShaderModule(resolve("shaders/crack.vert.spv"));
    VkShaderModule frag = loadShaderModule(resolve("shaders/crack.frag.spv"));

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    // The volume is the shared outline unit cube, scaled by push constants.
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(glm::vec3);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attr{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 1;
    vertexInput.pVertexAttributeDescriptions = &attr;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_FRONT_BIT; // back faces only: works from inside the box
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // No depth attachment in the decal segment: the shader does its own visibility by
    // reconstructing from the sampled depth (the volume test discards everything else).
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    // Minecraft's crumbling blend, "modulate 2x": out = 2 * crack * block. Mid-gray
    // crack texels leave the face unchanged, darker texels carve it darker, lighter
    // texels highlight it — the crack reads as damage IN the block texture.
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blend;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &sc_->imageFormat;
    renderingInfo.depthAttachmentFormat = VK_FORMAT_UNDEFINED; // color-only segment

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingInfo;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewport;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = decalPipelineLayout_;
    VK_CHECK(vkCreateGraphicsPipelines(ctx_->device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &decalPipeline_));

    vkDestroyShaderModule(ctx_->device, vert, nullptr);
    vkDestroyShaderModule(ctx_->device, frag, nullptr);
}

void Renderer::createOutline() {
    // One solid unit cube; each outline EDGE arrives as a thin bar-shaped box (min+size
    // via push constants), so compound shapes (stairs, fences) outline without interior
    // seams — the edge list itself decides what's drawn.
    static const glm::vec3 boxCorner[8] = {
        {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0},
        {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1},
    };
    static const int boxTri[36] = {
        0, 2, 3, 0, 3, 1, 4, 5, 7, 4, 7, 6, // -Z, +Z
        0, 4, 6, 0, 6, 2, 1, 3, 7, 1, 7, 5, // -X, +X
        0, 1, 5, 0, 5, 4, 2, 6, 7, 2, 7, 3, // -Y, +Y
    };

    std::vector<glm::vec3> verts;
    verts.reserve(36);
    for (int k = 0; k < 36; ++k) verts.push_back(boxCorner[boxTri[k]]);

    outlineVertexCount_ = static_cast<uint32_t>(verts.size());
    outlineVbuf_ = createDeviceBufferWithData(*ctx_, verts.data(), verts.size() * sizeof(glm::vec3),
                                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

    VkShaderModule vert = loadShaderModule(resolve("shaders/outline.vert.spv"));
    VkShaderModule frag = loadShaderModule(resolve("shaders/outline.frag.spv"));

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(glm::vec3);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attr{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 1;
    vertexInput.pVertexAttributeDescriptions = &attr;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    // The bars are opaque and write depth: passes drawn later that depth-test (clouds,
    // rain) can no longer paint over the selection outline.
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blend;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &sc_->imageFormat;
    renderingInfo.depthAttachmentFormat = sc_->depthFormat;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingInfo;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewport;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = pipelineLayout_; // reuse: same set layout (UBO) + vec4 push constant
    VK_CHECK(vkCreateGraphicsPipelines(ctx_->device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &outlinePipeline_));

    vkDestroyShaderModule(ctx_->device, vert, nullptr);
    vkDestroyShaderModule(ctx_->device, frag, nullptr);
}

void Renderer::createCommands() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = ctx_->graphicsFamily;
    VK_CHECK(vkCreateCommandPool(ctx_->device, &poolInfo, nullptr, &commandPool_));

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = kFramesInFlight;
    VK_CHECK(vkAllocateCommandBuffers(ctx_->device, &allocInfo, commandBuffers_));
}

void Renderer::createSync() {
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i = 0; i < kFramesInFlight; ++i) {
        VK_CHECK(vkCreateSemaphore(ctx_->device, &semInfo, nullptr, &imageAvailable_[i]));
        VK_CHECK(vkCreateFence(ctx_->device, &fenceInfo, nullptr, &inFlight_[i]));
    }
}

void Renderer::createRenderFinished() {
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    renderFinished_.resize(sc_->imageCount());
    for (auto& sem : renderFinished_) {
        VK_CHECK(vkCreateSemaphore(ctx_->device, &semInfo, nullptr, &sem));
    }
}

void Renderer::destroyRenderFinished() {
    for (VkSemaphore sem : renderFinished_) vkDestroySemaphore(ctx_->device, sem, nullptr);
    renderFinished_.clear();
}

void Renderer::createDescriptors() {
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx_->device, &layoutInfo, nullptr, &descriptorSetLayout_));

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = kFramesInFlight;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = kFramesInFlight;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kFramesInFlight;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(ctx_->device, &poolInfo, nullptr, &descriptorPool_));

    std::array<VkDescriptorSetLayout, kFramesInFlight> layouts;
    layouts.fill(descriptorSetLayout_);
    VkDescriptorSetAllocateInfo setAlloc{};
    setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAlloc.descriptorPool = descriptorPool_;
    setAlloc.descriptorSetCount = kFramesInFlight;
    setAlloc.pSetLayouts = layouts.data();
    VK_CHECK(vkAllocateDescriptorSets(ctx_->device, &setAlloc, descriptorSets_));

    for (int i = 0; i < kFramesInFlight; ++i) {
        cameraUBO_[i] = createHostBuffer(ctx_->allocator, sizeof(CameraUBOData),
                                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

        VkDescriptorBufferInfo bufInfo{};
        bufInfo.buffer = cameraUBO_[i].buffer;
        bufInfo.offset = 0;
        bufInfo.range = sizeof(CameraUBOData);

        VkDescriptorImageInfo imgInfo{};
        imgInfo.sampler = texture_->sampler();
        imgInfo.imageView = texture_->view();
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descriptorSets_[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &bufInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = descriptorSets_[i];
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &imgInfo;
        vkUpdateDescriptorSets(ctx_->device, 2, writes, 0, nullptr);
    }
}

VkShaderModule Renderer::loadShaderModule(const std::string& path) {
    std::vector<char> code = readFile(path);
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(ctx_->device, &ci, nullptr, &module));
    return module;
}

void Renderer::createPipeline() {
    VkShaderModule vert = loadShaderModule(resolve("shaders/chunk.vert.spv"));
    VkShaderModule frag = loadShaderModule(resolve("shaders/chunk.frag.spv"));

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(ChunkVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[4]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(ChunkVertex, pos)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ChunkVertex, uv)};
    attrs[2] = {2, 0, VK_FORMAT_R32_SFLOAT, offsetof(ChunkVertex, layer)};
    attrs[3] = {3, 0, VK_FORMAT_R32_SFLOAT, offsetof(ChunkVertex, light)};

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 4;
    vertexInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    // Cull inward-facing (back) faces; outward faces are counter-clockwise in framebuffer space.
    raster.cullMode = VK_CULL_MODE_BACK_BIT;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = VK_FALSE;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorSetLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(ctx_->device, &layoutInfo, nullptr, &pipelineLayout_));

    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &sc_->imageFormat;
    renderingInfo.depthAttachmentFormat = sc_->depthFormat;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingInfo;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewport;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = pipelineLayout_;

    VK_CHECK(vkCreateGraphicsPipelines(ctx_->device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                       nullptr, &pipeline_));

    vkDestroyShaderModule(ctx_->device, vert, nullptr);
    vkDestroyShaderModule(ctx_->device, frag, nullptr);
}

void Renderer::createLod() {
    VkShaderModule vert = loadShaderModule(resolve("shaders/lod.vert.spv"));
    VkShaderModule frag = loadShaderModule(resolve("shaders/lod.frag.spv"));

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(LodVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(LodVertex, pos)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(LodVertex, color)};

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 2;
    vertexInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    // Double-sided: the downsampled tiles aren't strictly wound, and there's no benefit
    // to culling this cheap far geometry.
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = VK_FALSE;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    // Own layout: same camera set (binding 0 UBO), no push constants (no fog).
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorSetLayout_;
    layoutInfo.pushConstantRangeCount = 0;
    VK_CHECK(vkCreatePipelineLayout(ctx_->device, &layoutInfo, nullptr, &lodPipelineLayout_));

    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &sc_->imageFormat;
    renderingInfo.depthAttachmentFormat = sc_->depthFormat;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingInfo;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewport;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = lodPipelineLayout_;
    VK_CHECK(vkCreateGraphicsPipelines(ctx_->device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &lodPipeline_));

    vkDestroyShaderModule(ctx_->device, vert, nullptr);
    vkDestroyShaderModule(ctx_->device, frag, nullptr);
}

void Renderer::createOcclusion() {
    // One occlusion query pool per frame-in-flight.
    VkQueryPoolCreateInfo qp{};
    qp.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qp.queryType = VK_QUERY_TYPE_OCCLUSION;
    qp.queryCount = kMaxOcclusionQueries;
    for (int i = 0; i < kFramesInFlight; ++i) {
        VK_CHECK(vkCreateQueryPool(ctx_->device, &qp, nullptr, &occlusionPool_[i]));
    }

    // Pipeline that draws a chunk's bounding box (outline unit cube), depth-TESTED but
    // writing neither color nor depth — used only to count visible samples for a query.
    VkShaderModule vert = loadShaderModule(resolve("shaders/outline.vert.spv"));
    VkShaderModule frag = loadShaderModule(resolve("shaders/outline.frag.spv"));

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(glm::vec3);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attr{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 1;
    vertexInput.pVertexAttributeDescriptions = &attr;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE; // camera may be inside the box
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = VK_FALSE;
    blendAttachment.colorWriteMask = 0; // write nothing; the query counts passing samples

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &sc_->imageFormat;
    renderingInfo.depthAttachmentFormat = sc_->depthFormat;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingInfo;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewport;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = pipelineLayout_; // reuse: same camera set + box push constant
    VK_CHECK(vkCreateGraphicsPipelines(ctx_->device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                       &occlusionPipeline_));

    vkDestroyShaderModule(ctx_->device, vert, nullptr);
    vkDestroyShaderModule(ctx_->device, frag, nullptr);
}

// Read the previous results from this frame slot's query pool (its fence already waited,
// so they're ready without a stall) and mark each queried chunk visible/hidden. Runs in
// drawFrame before the command buffer is re-recorded (which resets the pool).
void Renderer::applyOcclusionResults(const World* world) {
    std::vector<glm::ivec3>& slots = querySlotCoord_[currentFrame_];
    if (!world || slots.empty()) return;

    std::vector<uint64_t> results(slots.size(), 0);
    VkResult r = vkGetQueryPoolResults(
        ctx_->device, occlusionPool_[currentFrame_], 0, static_cast<uint32_t>(slots.size()),
        results.size() * sizeof(uint64_t), results.data(), sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT);
    if (r != VK_SUCCESS) return; // VK_NOT_READY (shouldn't happen post-fence): leave as-is

    const ChunkMap& chunks = world->chunks();
    for (size_t i = 0; i < slots.size(); ++i) {
        auto it = chunks.find({slots[i].x, slots[i].y, slots[i].z});
        if (it != chunks.end()) it->second->occVisible = (results[i] > 0);
    }
}

void Renderer::updateCameraUBO(const Camera& camera) {
    float aspect = static_cast<float>(sc_->extent.width) /
                   static_cast<float>(sc_->extent.height);
    CameraUBOData data{};
    data.view = camera.view();
    data.proj = camera.projection(aspect);
    data.invProj = glm::inverse(data.proj);
    std::memcpy(cameraUBO_[currentFrame_].mapped, &data, sizeof(data));

    // Frustum planes (Gribb-Hartmann) from proj*view. glm is column-major, so row i of
    // the combined matrix m is (m[0][i], m[1][i], m[2][i], m[3][i]). Depth is 0..1
    // (GLM_FORCE_DEPTH_ZERO_TO_ONE) so the near plane is row2, not row3+row2.
    glm::mat4 m = data.proj * data.view;
    auto row = [&](int i) { return glm::vec4(m[0][i], m[1][i], m[2][i], m[3][i]); };
    glm::vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    frustumPlanes_[0] = r3 + r0; // left
    frustumPlanes_[1] = r3 - r0; // right
    frustumPlanes_[2] = r3 + r1; // bottom
    frustumPlanes_[3] = r3 - r1; // top
    frustumPlanes_[4] = r2;      // near
    frustumPlanes_[5] = r3 - r2; // far
}

// AABB [mn,mx] vs the cached frustum: reject only when the box's positive vertex (the
// corner farthest along a plane normal) is still behind that plane -> fully outside.
bool Renderer::aabbInFrustum(const glm::vec3& mn, const glm::vec3& mx) const {
    for (const glm::vec4& p : frustumPlanes_) {
        glm::vec3 pv(p.x >= 0.0f ? mx.x : mn.x, p.y >= 0.0f ? mx.y : mn.y,
                     p.z >= 0.0f ? mx.z : mn.z);
        if (p.x * pv.x + p.y * pv.y + p.z * pv.z + p.w < 0.0f) return false;
    }
    return true;
}

void Renderer::recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex, const World* world,
                                   UIRenderer* ui, const std::vector<AABB>& outlineBoxes,
                                   ModelRenderer* model, const ModelPose& modelPose, bool modelVisible,
                                   HeldItemRenderer* held, const glm::mat4& heldMatrix,
                                   uint16_t heldBlock, bool heldVisible) {
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &begin));

    // Player-preview offscreen pass (title screen / inventory player box): renders the
    // posed model to its own texture, sampled later by the UI pass.
    if (playerPreview_ && previewActive_) {
        playerPreview_->record(cmd, currentFrame_, previewPose_);
    }

    transitionImage(cmd, sc_->images[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    transitionImage(cmd, sc_->depthImage, VK_IMAGE_ASPECT_DEPTH_BIT,
                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = sc_->imageViews[imageIndex];
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue.color = {{skyColor_.r, skyColor_.g, skyColor_.b, 1.0f}};

    // Decal segment runs while a block is cracked. (The selection outline stays
    // GEOMETRY: it is the selection volume and floats in air where a fence's box has
    // no mesh — a decal can only paint surfaces that exist in the depth buffer.)
    bool doDecals = !cracks_.empty();

    VkRenderingAttachmentInfo depthAttachment{};
    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttachment.imageView = sc_->depthView;
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    // When the decal segment runs it samples this depth after the pass — keep it.
    depthAttachment.storeOp =
        doDecals ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.offset = {0, 0};
    rendering.renderArea.extent = sc_->extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &colorAttachment;
    rendering.pDepthAttachment = &depthAttachment;

    // Occlusion queries for this frame slot are (re)written below; reset the pool now,
    // before the render pass begins (resets aren't allowed inside dynamic rendering).
    vkCmdResetQueryPool(cmd, occlusionPool_[currentFrame_], 0, kMaxOcclusionQueries);
    querySlotCoord_[currentFrame_].clear();

    vkCmdBeginRendering(cmd, &rendering);

    VkViewport vp{};
    vp.width = static_cast<float>(sc_->extent.width);
    vp.height = static_cast<float>(sc_->extent.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = sc_->extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Title screen with panorama art: the spinning backdrop replaces the sky and the
    // world entirely; only the player-model preview and the UI draw on top.
    bool panoramaOn = panoramaActive_ && panorama_ && panorama_->valid();
    if (panoramaOn) {
        float aspect = static_cast<float>(sc_->extent.width) /
                       static_cast<float>(std::max(1u, sc_->extent.height));
        panorama_->record(cmd, panoramaTime_, aspect);
    }

    // Sun/moon/stars first, no depth: everything else draws over them.
    if (sky_ && !panoramaOn && world) {
        sky_->record(cmd, currentFrame_, camEye_, sunAngle_, moonPhase_, starAlpha_,
                     celestialBrightness_);
    }

    if (!panoramaOn && world) {
        const int S = Chunk::kSize;
        const float Sf = static_cast<float>(S);

        // Frustum-cull, then sort front-to-back so nearer chunks fill the depth buffer
        // before farther ones are occlusion-tested against it.
        struct VisChunk { float dist2; Chunk* c; };
        std::vector<VisChunk> vis;
        vis.reserve(world->chunks().size());
        // Created/READY cubes per column: a column only hides its LOD tile when every cube
        // it has is ready AND it has at least its non-sky cubes (the topmost cube is
        // usually skipped as guaranteed air). Partial columns — loading up or unloading
        // down — keep the LOD visible underneath so there is never a hole with neither.
        auto columnKey = [](int cx, int cz) {
            return (static_cast<int64_t>(cx) << 32) ^ static_cast<uint32_t>(cz);
        };
        std::unordered_map<int64_t, glm::ivec2> colState; // x = created cubes, y = READY
        colState.reserve(world->chunks().size() / World::kVerticalChunks);
        for (const auto& [coord, chunk] : world->chunks()) {
            glm::ivec2& cs = colState[columnKey(coord.x, coord.z)];
            ++cs.x;
            if (chunk->state.load(std::memory_order_acquire) != CHUNK_READY) continue;
            ++cs.y; // counts empty (all-air) cubes too
            if (chunk->mesh.indexCount == 0) continue;
            // Tight mesh bounds (not the full 32^3 cube): terrain cubes usually hold a
            // thin slice, so culling and the occlusion proxy box hug the real geometry.
            glm::vec3 org(chunk->cx() * Sf, chunk->cy() * Sf, chunk->cz() * Sf);
            glm::vec3 mn = org + chunk->mesh.boundsMin;
            glm::vec3 mx = org + chunk->mesh.boundsMax;
            if (!aabbInFrustum(mn, mx)) continue;
            glm::vec3 d = (mn + mx) * 0.5f - camEye_;
            vis.push_back({glm::dot(d, d), chunk.get()});
        }
        std::sort(vis.begin(), vis.end(),
                  [](const VisChunk& a, const VisChunk& b) { return a.dist2 < b.dist2; });

        std::vector<glm::ivec3>& slots = querySlotCoord_[currentFrame_];
        VkQueryPool pool = occlusionPool_[currentFrame_];
        renderedChunks_ = 0;

        // Only re-test occlusion when the camera actually moved/turned enough to matter; a
        // tiny nudge isn't worth the query + bounding-box GPU work, so visibility is frozen.
        constexpr float kOccMoveThresh = 1.5f; // blocks
        constexpr float kOccTurnCos = 0.9990f; // ~2.6 degrees
        glm::vec3 dEye = camEye_ - lastQueryEye_;
        bool doQueries = glm::dot(dEye, dEye) > kOccMoveThresh * kOccMoveThresh ||
                         glm::dot(camFront_, lastQueryFront_) < kOccTurnCos;
        if (doQueries) {
            lastQueryEye_ = camEye_;
            lastQueryFront_ = camFront_;
        }

        // Pass A: real geometry for chunks visible at the last test (and, when out of query
        // slots, drawn conservatively). Each real draw is wrapped in an occlusion query.
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                                &descriptorSets_[currentFrame_], 0, nullptr);
        for (const VisChunk& v : vis) {
            Chunk* c = v.c;
            // Hidden chunks aren't drawn: bounding-box tested in pass B when querying, or
            // simply left hidden when frozen. Only when querying AND out of slots do we
            // fall through and draw a hidden chunk conservatively (it can't be tested).
            if (!c->occVisible && (!doQueries || slots.size() < kMaxOcclusionQueries)) continue;
            bool query = doQueries && slots.size() < kMaxOcclusionQueries;
            uint32_t slot = static_cast<uint32_t>(slots.size());
            if (query) vkCmdBeginQuery(cmd, pool, slot, 0);
            PushConstants pc{};
            pc.chunkPos = glm::vec4(c->cx() * Sf, c->cy() * Sf, c->cz() * Sf, 0.0f);
            vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &c->mesh.vertexBuffer.buffer, &offset);
            vkCmdBindIndexBuffer(cmd, c->mesh.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, c->mesh.indexCount, 1, 0, 0, 0);
            if (query) {
                vkCmdEndQuery(cmd, pool, slot);
                slots.push_back({c->cx(), c->cy(), c->cz()});
            }
            ++renderedChunks_;
        }

        // Pass B: bounding-box occlusion test for chunks hidden at the last test, so they
        // are detected when they come back into view. Depth-tested, writes no color/depth.
        bool boxBound = false;
        for (const VisChunk& v : vis) {
            if (!doQueries) break;
            Chunk* c = v.c;
            if (c->occVisible) continue;
            if (slots.size() >= kMaxOcclusionQueries) break;
            if (!boxBound) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, occlusionPipeline_);
                VkDeviceSize off = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &outlineVbuf_.buffer, &off);
                boxBound = true;
            }
            uint32_t slot = static_cast<uint32_t>(slots.size());
            vkCmdBeginQuery(cmd, pool, slot, 0);
            PushConstants pc{};
            // Proxy box = the mesh's tight bounds, padded a hair so depth precision
            // against exactly coplanar neighbor faces can't swallow it (visible bias
            // is the safe direction for occlusion).
            constexpr float kOccPad = 0.05f;
            glm::vec3 org(c->cx() * Sf, c->cy() * Sf, c->cz() * Sf);
            pc.chunkPos = glm::vec4(org + c->mesh.boundsMin - kOccPad, 0.0f);
            pc.boxSize =
                glm::vec4(c->mesh.boundsMax - c->mesh.boundsMin + 2.0f * kOccPad, 0.0f);
            vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
            vkCmdDraw(cmd, outlineVertexCount_, 1, 0, 0);
            vkCmdEndQuery(cmd, pool, slot);
            slots.push_back({c->cx(), c->cy(), c->cz()});
        }

        // Far LOD terrain: colored downsampled tiles filling the ring beyond the real
        // chunks (no fog — the full world stays visible). Frustum-culled like the chunks.
        if (!world->lodTiles().empty()) {
            const float Sf = static_cast<float>(S);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lodPipeline_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, lodPipelineLayout_, 0, 1,
                                    &descriptorSets_[currentFrame_], 0, nullptr);
            for (const auto& [coord, tile] : world->lodTiles()) {
                if (tile.mesh.indexCount == 0) continue;
                // Hide the tile only when its column is FULLY loaded: every created cube is
                // ready and at least the non-sky cubes exist. Partial columns keep their
                // LOD so transitions never leave holes.
                auto hd = colState.find(columnKey(coord.x, coord.z));
                if (hd != colState.end() && hd->second.x >= World::kVerticalChunks - 1 &&
                    hd->second.y == hd->second.x) {
                    continue;
                }
                glm::vec3 mn(static_cast<float>(coord.x) * Sf, tile.mesh.minY,
                             static_cast<float>(coord.z) * Sf);
                glm::vec3 mx(static_cast<float>(coord.x + 1) * Sf, tile.mesh.maxY + 1.0f,
                             static_cast<float>(coord.z + 1) * Sf);
                if (!aabbInFrustum(mn, mx)) continue;
                VkDeviceSize off = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &tile.mesh.vertexBuffer.buffer, &off);
                vkCmdBindIndexBuffer(cmd, tile.mesh.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, tile.mesh.indexCount, 1, 0, 0, 0);
            }
        }
    }

    // ---- Screen-space decal segment (mining cracks + selection outline) ----------
    // Terrain is fully drawn; end the pass, move DEPTH to a sampled layout, and paint
    // each decal volume onto whatever terrain surface its shader reconstructs from
    // depth. Then restore depth and resume the pass (color+depth LOAD) for entities,
    // overlays and UI.
    if (doDecals && !panoramaOn) {
        vkCmdEndRendering(cmd);

        transitionImage(cmd, sc_->images[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        // Copy scene depth into the plain sampled image the decal shader reads.
        // (A copy with transfer barriers is the reliable path — sampling the live
        // attachment in a read-only layout gave compressed/garbage reads on AMD.)
        transitionImage(cmd, sc_->depthImage, VK_IMAGE_ASPECT_DEPTH_BIT,
                        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transitionImage(cmd, depthCopy_, VK_IMAGE_ASPECT_DEPTH_BIT,
                        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        region.extent = {sc_->extent.width, sc_->extent.height, 1};
        vkCmdCopyImage(cmd, sc_->depthImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depthCopy_,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        transitionImage(cmd, depthCopy_, VK_IMAGE_ASPECT_DEPTH_BIT,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        transitionImage(cmd, sc_->depthImage, VK_IMAGE_ASPECT_DEPTH_BIT,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

        VkRenderingAttachmentInfo colorLoad = colorAttachment;
        colorLoad.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        VkRenderingInfo decalPass = rendering;
        decalPass.pColorAttachments = &colorLoad;
        decalPass.pDepthAttachment = nullptr; // color-only: depth is a texture here
        vkCmdBeginRendering(cmd, &decalPass);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, decalPipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, decalPipelineLayout_, 0, 1,
                                &decalSets_[currentFrame_], 0, nullptr);
        VkDeviceSize decalOff = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &outlineVbuf_.buffer, &decalOff);
        for (const Crack& c : cracks_) {
            float layer = static_cast<float>(
                texture_->layer("destroy_stage_" + std::to_string(c.stage)));
            PushConstants pc{};
            pc.chunkPos = glm::vec4(c.box.min, 0.0f);
            pc.boxSize = glm::vec4(c.box.max - c.box.min, layer);
            vkCmdPushConstants(cmd, decalPipelineLayout_,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(pc), &pc);
            vkCmdDraw(cmd, outlineVertexCount_, 1, 0, 0);
        }

        vkCmdEndRendering(cmd);

        transitionImage(cmd, sc_->images[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        // Depth is already back in attachment layout (restored right after the copy).
        VkRenderingAttachmentInfo depthLoad = depthAttachment;
        depthLoad.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depthLoad.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        VkRenderingInfo resume = rendering;
        resume.pColorAttachments = &colorLoad;
        resume.pDepthAttachment = &depthLoad;
        vkCmdBeginRendering(cmd, &resume);
    }

    // Player model (3rd-person), depth-tested with the world.
    if (model && modelVisible) {
        model->record(cmd, currentFrame_, modelPose);
    }

    // Held block in the player's hand (1st and 3rd person).
    if (held && heldVisible) {
        held->record(cmd, currentFrame_, heldMatrix, heldBlock);
    }

    // Dropped items (mini blocks bobbing on the ground), same pipeline as the held block.
    if (held) {
        for (const Drops::Draw& d : dropDraws_) held->record(cmd, currentFrame_, d.m, d.block);
    }

    // Wireframe outline around the targeted block's selection boxes (depth-tested,
    // sits just off the faces). One draw per box (fences/stairs have several).
    if (!outlineBoxes.empty()) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, outlinePipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                                &descriptorSets_[currentFrame_], 0, nullptr);
        VkDeviceSize outlineOff = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &outlineVbuf_.buffer, &outlineOff);
        for (const AABB& box : outlineBoxes) {
            PushConstants pc{};
            pc.chunkPos = glm::vec4(box.min, 0.0f);
            pc.boxSize = glm::vec4(box.max - box.min, 0.0f);
            vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
            vkCmdDraw(cmd, outlineVertexCount_, 1, 0, 0);
        }
    }

    // Rain curtain around the player (alpha blended, depth-tested against the world).
    if (weather_ && !panoramaOn && world) {
        weather_->record(cmd, currentFrame_, camEye_, *world, weatherTime_, weatherIntensity_,
                         cloudBrightness_);
    }

    // 2D HUD, drawn over the world (no depth test, alpha blended).
    if (ui) ui->recordDraw(cmd, currentFrame_);

    vkCmdEndRendering(cmd);

    transitionImage(cmd, sc_->images[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    VK_CHECK(vkEndCommandBuffer(cmd));
}

void Renderer::drawFrame(const Camera& camera, const World* world, UIRenderer* ui,
                         const std::vector<AABB>& outlineBoxes,
                         ModelRenderer* model, const ModelPose& modelPose, bool modelVisible,
                         HeldItemRenderer* held, const glm::mat4& heldMatrix, uint16_t heldBlock,
                         bool heldVisible) {
    auto tWait = std::chrono::high_resolution_clock::now();
    VK_CHECK(vkWaitForFences(ctx_->device, 1, &inFlight_[currentFrame_], VK_TRUE, UINT64_MAX));
    gpuWaitMs_ = std::chrono::duration<double, std::milli>(
                     std::chrono::high_resolution_clock::now() - tWait)
                     .count();

    uint32_t imageIndex = 0;
    VkResult acquire = vkAcquireNextImageKHR(ctx_->device, sc_->handle, UINT64_MAX,
                                             imageAvailable_[currentFrame_], VK_NULL_HANDLE,
                                             &imageIndex);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return;
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
        throw std::runtime_error("vkAcquireNextImageKHR failed");
    }

    updateCameraUBO(camera);
    camEye_ = camera.renderEye();
    camFront_ = camera.front();

    // This frame slot's fence is signaled, so its occlusion queries (recorded 2 frames
    // ago) are ready to read without stalling. Apply them before the buffer is re-recorded.
    applyOcclusionResults(world);

    // (Re)point the decal set's depth binding at the current depth-copy view — it
    // changes on window resize, and the fence wait guarantees this set isn't in flight.
    {
        VkDescriptorImageInfo depthInfo{};
        depthInfo.sampler = depthSampler_;
        depthInfo.imageView = depthCopyView_;
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = decalSets_[currentFrame_];
        w.dstBinding = 2;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.descriptorCount = 1;
        w.pImageInfo = &depthInfo;
        vkUpdateDescriptorSets(ctx_->device, 1, &w, 0, nullptr);
    }

    VK_CHECK(vkResetFences(ctx_->device, 1, &inFlight_[currentFrame_]));

    VkCommandBuffer cmd = commandBuffers_[currentFrame_];
    VK_CHECK(vkResetCommandBuffer(cmd, 0));
    auto tRec = std::chrono::high_resolution_clock::now();
    recordCommandBuffer(cmd, imageIndex, world, ui, outlineBoxes, model, modelPose,
                        modelVisible, held, heldMatrix, heldBlock, heldVisible);
    recordMs_ = std::chrono::duration<double, std::milli>(
                    std::chrono::high_resolution_clock::now() - tRec)
                    .count();

    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = cmd;

    VkSemaphoreSubmitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitInfo.semaphore = imageAvailable_[currentFrame_];
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signalInfo{};
    signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfo.semaphore = renderFinished_[imageIndex];
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &waitInfo;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signalInfo;
    VK_CHECK(vkQueueSubmit2(ctx_->graphicsQueue, 1, &submit, inFlight_[currentFrame_]));

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderFinished_[imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &sc_->handle;
    present.pImageIndices = &imageIndex;

    VkResult presentResult = vkQueuePresentKHR(ctx_->graphicsQueue, &present);
    bool resized = window_->consumeResized();
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR || resized) {
        recreateSwapchain();
    } else if (presentResult != VK_SUCCESS) {
        throw std::runtime_error("vkQueuePresentKHR failed");
    }

    currentFrame_ = (currentFrame_ + 1) % kFramesInFlight;
}

void Renderer::recreateSwapchain() {
    int width = 0, height = 0;
    window_->waitForValidFramebuffer(width, height);
    vkDeviceWaitIdle(ctx_->device);
    sc_->recreate(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    destroyDepthCopy();
    createDepthCopy();
    destroyRenderFinished();
    createRenderFinished();
}

void Renderer::cleanup() {
    vkDeviceWaitIdle(ctx_->device);
    if (occlusionPipeline_) vkDestroyPipeline(ctx_->device, occlusionPipeline_, nullptr);
    for (int i = 0; i < kFramesInFlight; ++i) {
        if (occlusionPool_[i]) vkDestroyQueryPool(ctx_->device, occlusionPool_[i], nullptr);
    }
    if (lodPipeline_) vkDestroyPipeline(ctx_->device, lodPipeline_, nullptr);
    if (lodPipelineLayout_) vkDestroyPipelineLayout(ctx_->device, lodPipelineLayout_, nullptr);
    if (decalPipeline_) vkDestroyPipeline(ctx_->device, decalPipeline_, nullptr);
    if (decalPipelineLayout_) vkDestroyPipelineLayout(ctx_->device, decalPipelineLayout_, nullptr);
    if (decalPool_) vkDestroyDescriptorPool(ctx_->device, decalPool_, nullptr);
    if (decalSetLayout_) vkDestroyDescriptorSetLayout(ctx_->device, decalSetLayout_, nullptr);
    if (depthSampler_) vkDestroySampler(ctx_->device, depthSampler_, nullptr);
    destroyDepthCopy();
    if (outlinePipeline_) vkDestroyPipeline(ctx_->device, outlinePipeline_, nullptr);
    destroyBuffer(ctx_->allocator, outlineVbuf_);
    if (pipeline_) vkDestroyPipeline(ctx_->device, pipeline_, nullptr);
    if (pipelineLayout_) vkDestroyPipelineLayout(ctx_->device, pipelineLayout_, nullptr);
    for (int i = 0; i < kFramesInFlight; ++i) destroyBuffer(ctx_->allocator, cameraUBO_[i]);
    if (descriptorPool_) vkDestroyDescriptorPool(ctx_->device, descriptorPool_, nullptr);
    if (descriptorSetLayout_) vkDestroyDescriptorSetLayout(ctx_->device, descriptorSetLayout_, nullptr);
    destroyRenderFinished();
    for (int i = 0; i < kFramesInFlight; ++i) {
        if (imageAvailable_[i]) vkDestroySemaphore(ctx_->device, imageAvailable_[i], nullptr);
        if (inFlight_[i]) vkDestroyFence(ctx_->device, inFlight_[i], nullptr);
    }
    if (commandPool_) vkDestroyCommandPool(ctx_->device, commandPool_, nullptr);
}

} // namespace mc
