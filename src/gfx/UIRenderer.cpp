#include "gfx/UIRenderer.h"

#include "core/Paths.h"
#include "gfx/Swapchain.h"
#include "gfx/VkContext.h"

#include <vk_mem_alloc.h>
#include <stb_image.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace mc {
namespace {

std::vector<char> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("Failed to open shader file: " + path);
    size_t size = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(size);
    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(size));
    return buffer;
}

void imageBarrier(VkCommandBuffer cmd, VkImage image,
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
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

void UIRenderer::init(VkContext* ctx, Swapchain* sc) {
    ctx_ = ctx;
    sc_ = sc;
    createSamplerAndLayout();
    pipeline_ = buildPipeline(false);
    invertPipeline_ = buildPipeline(true);
    createBuffers();
    verts_.reserve(kMaxVerts);

    const uint8_t white[4] = {255, 255, 255, 255};
    createTexture(white, 1, 1); // texture id 0
}

void UIRenderer::createSamplerAndLayout() {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(ctx_->device, &samplerInfo, nullptr, &sampler_));

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx_->device, &layoutInfo, nullptr, &setLayout_));

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = kMaxTextures;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kMaxTextures;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    VK_CHECK(vkCreateDescriptorPool(ctx_->device, &poolInfo, nullptr, &pool_));

    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &setLayout_;
    VK_CHECK(vkCreatePipelineLayout(ctx_->device, &pl, nullptr, &pipelineLayout_));
}

int UIRenderer::createTexture(const uint8_t* rgba, int w, int h) {
    UITexture tex;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    imageInfo.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VK_CHECK(vmaCreateImage(ctx_->allocator, &imageInfo, &allocInfo, &tex.image, &tex.alloc, nullptr));

    VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4;
    AllocatedBuffer staging = createHostBuffer(ctx_->allocator, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(staging.mapped, rgba, static_cast<size_t>(bytes));

    ctx_->immediateSubmit([&](VkCommandBuffer cmd) {
        imageBarrier(cmd, tex.image, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, tex.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        imageBarrier(cmd, tex.image, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    destroyBuffer(ctx_->allocator, staging);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = tex.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(ctx_->device, &viewInfo, nullptr, &tex.view));

    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &setLayout_;
    VK_CHECK(vkAllocateDescriptorSets(ctx_->device, &alloc, &tex.set));

    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler = sampler_;
    imgInfo.imageView = tex.view;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = tex.set;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &imgInfo;
    vkUpdateDescriptorSets(ctx_->device, 1, &write, 0, nullptr);

    int id = static_cast<int>(textures_.size());
    textures_.push_back(tex);
    return id;
}

int UIRenderer::registerImageView(VkImageView view) {
    UITexture tex;
    tex.owned = false;
    tex.view = view;

    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &setLayout_;
    VK_CHECK(vkAllocateDescriptorSets(ctx_->device, &alloc, &tex.set));

    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler = sampler_;
    imgInfo.imageView = view;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = tex.set;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &imgInfo;
    vkUpdateDescriptorSets(ctx_->device, 1, &write, 0, nullptr);

    int id = static_cast<int>(textures_.size());
    textures_.push_back(tex);
    return id;
}

int UIRenderer::registerTexture(const std::string& pngPath) {
    int w = 0, h = 0, ch = 0;
    stbi_uc* data = stbi_load(pngPath.c_str(), &w, &h, &ch, 4);
    if (!data) {
        std::fprintf(stderr, "[UI] failed to load %s; using white fallback\n", pngPath.c_str());
        return 0;
    }
    int id = createTexture(data, w, h);
    stbi_image_free(data);
    return id;
}

VkShaderModule UIRenderer::loadShader(const std::string& path) {
    std::vector<char> code = readFile(path);
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(ctx_->device, &ci, nullptr, &module));
    return module;
}

VkPipeline UIRenderer::buildPipeline(bool invert) {
    VkShaderModule vert = loadShader(resolve("shaders/ui.vert.spv"));
    VkShaderModule frag = loadShader(resolve("shaders/ui.frag.spv"));

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
    binding.stride = sizeof(UIVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UIVertex, pos)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UIVertex, uv)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(UIVertex, color)};

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 3;
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
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    if (invert) {
        // result = src*(1-dst) + dst*(1-src); with src=white this yields 1-dst (inverted).
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
    } else {
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
    }
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
    pipelineInfo.layout = pipelineLayout_;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(ctx_->device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));
    vkDestroyShaderModule(ctx_->device, vert, nullptr);
    vkDestroyShaderModule(ctx_->device, frag, nullptr);
    return pipeline;
}

void UIRenderer::createBuffers() {
    for (int i = 0; i < kFramesInFlight; ++i) {
        vbuf_[i] = createHostBuffer(ctx_->allocator, kMaxVerts * sizeof(UIVertex),
                                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    }
}

void UIRenderer::beginFrame(uint32_t screenW, uint32_t screenH) {
    verts_.clear();
    cmds_.clear();
    screenW_ = static_cast<float>(screenW ? screenW : 1);
    screenH_ = static_cast<float>(screenH ? screenH : 1);
}

void UIRenderer::pushVertex(float px, float py, float u, float v, const glm::vec4& color) {
    UIVertex vtx;
    vtx.pos = {px / screenW_ * 2.0f - 1.0f, py / screenH_ * 2.0f - 1.0f};
    vtx.uv = {u, v};
    vtx.color = color;
    verts_.push_back(vtx);
}

void UIRenderer::addQuad(int texId, bool invert, float x, float y, float w, float h,
                         float u0, float v0, float u1, float v1, const glm::vec4& color) {
    uint32_t first = static_cast<uint32_t>(verts_.size());
    pushVertex(x, y, u0, v0, color);
    pushVertex(x, y + h, u0, v1, color);
    pushVertex(x + w, y + h, u1, v1, color);
    pushVertex(x, y, u0, v0, color);
    pushVertex(x + w, y + h, u1, v1, color);
    pushVertex(x + w, y, u1, v0, color);

    if (!cmds_.empty() && cmds_.back().texId == texId && cmds_.back().invert == invert) {
        cmds_.back().count += 6;
    } else {
        cmds_.push_back({texId, invert, first, 6});
    }
}

void UIRenderer::quad(float x, float y, float w, float h, const glm::vec4& color) {
    addQuad(0, false, x, y, w, h, 0, 0, 1, 1, color);
}

void UIRenderer::sprite(int texId, float x, float y, float w, float h, const glm::vec4& color) {
    addQuad(texId, false, x, y, w, h, 0, 0, 1, 1, color);
}

void UIRenderer::texQuad(int texId, float x, float y, float w, float h,
                         float u0, float v0, float u1, float v1, const glm::vec4& color) {
    addQuad(texId, false, x, y, w, h, u0, v0, u1, v1, color);
}

void UIRenderer::invertQuad(float x, float y, float w, float h) {
    addQuad(0, true, x, y, w, h, 0, 0, 1, 1, glm::vec4(1.0f));
}

void UIRenderer::recordDraw(VkCommandBuffer cmd, uint32_t frameIndex) {
    if (verts_.empty()) return;
    size_t count = verts_.size() < kMaxVerts ? verts_.size() : kMaxVerts;
    std::memcpy(vbuf_[frameIndex].mapped, verts_.data(), count * sizeof(UIVertex));

    VkViewport vp{};
    vp.width = static_cast<float>(sc_->extent.width);
    vp.height = static_cast<float>(sc_->extent.height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D scissor{{0, 0}, sc_->extent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf_[frameIndex].buffer, &offset);

    for (const DrawCmd& c : cmds_) {
        if (c.first + c.count > count) continue;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c.invert ? invertPipeline_ : pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                                &textures_[c.texId].set, 0, nullptr);
        vkCmdDraw(cmd, c.count, 1, c.first, 0);
    }
}

void UIRenderer::cleanup() {
    if (pipeline_) vkDestroyPipeline(ctx_->device, pipeline_, nullptr);
    if (invertPipeline_) vkDestroyPipeline(ctx_->device, invertPipeline_, nullptr);
    if (pipelineLayout_) vkDestroyPipelineLayout(ctx_->device, pipelineLayout_, nullptr);
    if (pool_) vkDestroyDescriptorPool(ctx_->device, pool_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(ctx_->device, setLayout_, nullptr);
    for (int i = 0; i < kFramesInFlight; ++i) destroyBuffer(ctx_->allocator, vbuf_[i]);
    if (sampler_) vkDestroySampler(ctx_->device, sampler_, nullptr);
    for (UITexture& t : textures_) {
        if (!t.owned) continue; // external images (icons) are freed by their owner
        if (t.view) vkDestroyImageView(ctx_->device, t.view, nullptr);
        if (t.image) vmaDestroyImage(ctx_->allocator, t.image, t.alloc);
    }
    textures_.clear();
}

} // namespace mc
