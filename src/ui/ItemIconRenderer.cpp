#include "ui/ItemIconRenderer.h"

#include "core/Paths.h"
#include "gfx/TextureArray.h"
#include "gfx/UIRenderer.h"
#include "gfx/VkContext.h"
#include "world/BlockRegistry.h"

#include <vk_mem_alloc.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstddef>
#include <cstring>
#include <fstream>
#include <vector>

namespace mc {
namespace {

struct IconUBO {
    glm::mat4 view;
    glm::mat4 proj;
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

void ItemIconRenderer::init(VkContext* ctx, TextureArray* blockTex, const BlockRegistry* reg,
                            UIRenderer* ui) {
    ctx_ = ctx;
    createDepth();
    createDescriptor(blockTex);
    createPipeline();
    renderIcons(reg, ui);
}

void ItemIconRenderer::createDepth() {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_D32_SFLOAT;
    info.extent = {kIconSize, kIconSize, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    alloc.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VK_CHECK(vmaCreateImage(ctx_->allocator, &info, &alloc, &depthImage_, &depthAlloc_, nullptr));

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = depthImage_;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_D32_SFLOAT;
    view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(ctx_->device, &view, nullptr, &depthView_));
}

void ItemIconRenderer::createDescriptor(TextureArray* blockTex) {
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
    VK_CHECK(vkCreateDescriptorSetLayout(ctx_->device, &layoutInfo, nullptr, &setLayout_));

    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[0].descriptorCount = 1;
    sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[1].descriptorCount = 1;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(ctx_->device, &poolInfo, nullptr, &pool_));

    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &setLayout_;
    VK_CHECK(vkAllocateDescriptorSets(ctx_->device, &alloc, &set_));

    // The per-block gui display transform (pushed as the model matrix) does all the
    // orienting; the ortho window is one block unit across, matching the vanilla 16px
    // slot so the standard 0.625 gui scale fills the icon like Minecraft's.
    IconUBO data{};
    data.view = glm::mat4(1.0f);
    data.proj = glm::ortho(-0.5f, 0.5f, -0.5f, 0.5f, -2.0f, 2.0f);
    data.proj[1][1] *= -1.0f; // Vulkan clip-space Y

    ubo_ = createHostBuffer(ctx_->allocator, sizeof(IconUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    std::memcpy(ubo_.mapped, &data, sizeof(data));

    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = ubo_.buffer;
    bufInfo.range = sizeof(IconUBO);
    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler = blockTex->sampler();
    imgInfo.imageView = blockTex->view();
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set_;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].descriptorCount = 1;
    writes[0].pBufferInfo = &bufInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set_;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &imgInfo;
    vkUpdateDescriptorSets(ctx_->device, 2, writes, 0, nullptr);
}

VkShaderModule ItemIconRenderer::loadShader(const std::string& path) {
    std::vector<char> code = readFile(path);
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(ctx_->device, &ci, nullptr, &module));
    return module;
}

void ItemIconRenderer::createPipeline() {
    // Own vertex shader (per-icon model-matrix push constant); the chunk fragment shader
    // is reused for the texture-array sampling + face shading.
    VkShaderModule vert = loadShader(resolve("shaders/icon.vert.spv"));
    VkShaderModule frag = loadShader(resolve("shaders/chunk.frag.spv"));

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

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_FALSE;
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

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(glm::mat4);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VK_CHECK(vkCreatePipelineLayout(ctx_->device, &layoutInfo, nullptr, &pipelineLayout_));

    VkFormat colorFormat = VK_FORMAT_R8G8B8A8_SRGB;
    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &colorFormat;
    renderingInfo.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

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
    VK_CHECK(vkCreateGraphicsPipelines(ctx_->device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_));

    vkDestroyShaderModule(ctx_->device, vert, nullptr);
    vkDestroyShaderModule(ctx_->device, frag, nullptr);
}

ChunkMeshData ItemIconRenderer::buildCube(const BlockRegistry* reg, BlockId id) {
    // Build the block's actual shape (cube/slab/cross) and center it on the origin.
    ChunkMeshData mesh = buildSingleBlock(*reg, id);
    for (ChunkVertex& v : mesh.vertices) v.pos -= glm::vec3(0.5f);
    return mesh;
}

void ItemIconRenderer::renderIcons(const BlockRegistry* reg, UIRenderer* ui) {
    icons_.resize(reg->count());
    // Build a cube mesh and an offscreen color image for every non-air, non-hidden
    // block (hidden variants like double slabs never appear in any UI, and each icon
    // costs a UI texture slot).
    for (uint16_t i = 1; i < reg->count(); ++i) {
        BlockId id = static_cast<BlockId>(i);
        if (reg->block(id).props().hiddenFromHotbar) continue;
        Icon& icon = icons_[i];
        icon.mesh = uploadMesh(*ctx_, buildCube(reg, id));

        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_SRGB;
        info.extent = {kIconSize, kIconSize, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        VmaAllocationCreateInfo alloc{};
        alloc.usage = VMA_MEMORY_USAGE_AUTO;
        alloc.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        VK_CHECK(vmaCreateImage(ctx_->allocator, &info, &alloc, &icon.image, &icon.alloc, nullptr));

        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = icon.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = VK_FORMAT_R8G8B8A8_SRGB;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(ctx_->device, &view, nullptr, &icon.view));
    }

    ctx_->immediateSubmit([&](VkCommandBuffer cmd) {
        imageBarrier(cmd, depthImage_, VK_IMAGE_ASPECT_DEPTH_BIT,
                     VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                         VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

        VkViewport vp{0, 0, static_cast<float>(kIconSize), static_cast<float>(kIconSize), 0, 1};
        VkRect2D scissor{{0, 0}, {kIconSize, kIconSize}};

        for (uint16_t i = 1; i < reg->count(); ++i) {
            Icon& icon = icons_[i];
            if (icon.image == VK_NULL_HANDLE) continue; // hidden block: no icon

            imageBarrier(cmd, icon.image, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

            // Order this icon's depth clear/writes after the previous icon's (shared depth).
            imageBarrier(cmd, depthImage_, VK_IMAGE_ASPECT_DEPTH_BIT,
                         VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

            VkRenderingAttachmentInfo color{};
            color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            color.imageView = icon.view;
            color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            color.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

            VkRenderingAttachmentInfo depth{};
            depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depth.imageView = depthView_;
            depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depth.clearValue.depthStencil = {1.0f, 0};

            VkRenderingInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering.renderArea = scissor;
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachments = &color;
            rendering.pDepthAttachment = &depth;

            vkCmdBeginRendering(cmd, &rendering);
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                                    &set_, 0, nullptr);
            // The block's gui display transform (vanilla: translate/16, rotate XYZ, scale)
            // plus a 90-deg right yaw: the engine's meshes face 90 deg off vanilla's
            // model space, so every icon turns to match.
            const ItemTransform& gui = reg->block(static_cast<BlockId>(i)).display().gui;
            glm::mat4 model =
                glm::translate(glm::mat4(1.0f), gui.translation / 16.0f) *
                glm::rotate(glm::mat4(1.0f), glm::radians(gui.rotationDeg.x), glm::vec3(1, 0, 0)) *
                glm::rotate(glm::mat4(1.0f), glm::radians(gui.rotationDeg.y), glm::vec3(0, 1, 0)) *
                glm::rotate(glm::mat4(1.0f), glm::radians(gui.rotationDeg.z), glm::vec3(0, 0, 1)) *
                glm::scale(glm::mat4(1.0f), gui.scale) *
                glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(0, 1, 0));
            vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(model), &model);
            VkDeviceSize off = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &icon.mesh.vertexBuffer.buffer, &off);
            vkCmdBindIndexBuffer(cmd, icon.mesh.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, icon.mesh.indexCount, 1, 0, 0, 0);
            vkCmdEndRendering(cmd);

            imageBarrier(cmd, icon.image, VK_IMAGE_ASPECT_COLOR_BIT,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    });

    for (uint16_t i = 1; i < reg->count(); ++i) {
        if (icons_[i].view == VK_NULL_HANDLE) continue;
        icons_[i].uiTexId = ui->registerImageView(icons_[i].view);
    }
}

int ItemIconRenderer::iconTexId(BlockId id) const {
    return id < icons_.size() ? icons_[id].uiTexId : -1;
}

void ItemIconRenderer::cleanup() {
    if (pipeline_) vkDestroyPipeline(ctx_->device, pipeline_, nullptr);
    if (pipelineLayout_) vkDestroyPipelineLayout(ctx_->device, pipelineLayout_, nullptr);
    if (pool_) vkDestroyDescriptorPool(ctx_->device, pool_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(ctx_->device, setLayout_, nullptr);
    destroyBuffer(ctx_->allocator, ubo_);
    if (depthView_) vkDestroyImageView(ctx_->device, depthView_, nullptr);
    if (depthImage_) vmaDestroyImage(ctx_->allocator, depthImage_, depthAlloc_);
    for (Icon& icon : icons_) {
        icon.mesh.destroy(ctx_->allocator);
        if (icon.view) vkDestroyImageView(ctx_->device, icon.view, nullptr);
        if (icon.image) vmaDestroyImage(ctx_->allocator, icon.image, icon.alloc);
    }
}

} // namespace mc
