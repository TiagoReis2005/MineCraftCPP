#include "gfx/PanoramaRenderer.h"

#include "core/Paths.h"
#include "gfx/Swapchain.h"
#include "gfx/VkContext.h"

#include <stb_image.h>
#include <vk_mem_alloc.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace mc {
namespace {

struct PanoVertex {
    glm::vec3 pos;
    glm::vec2 uv;
};

struct PanoPush {
    glm::mat4 viewProj;
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

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkPipelineStageFlags2 srcStage,
                  VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                  VkAccessFlags2 dstAccess, VkImageLayout oldLayout, VkImageLayout newLayout) {
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

void PanoramaRenderer::init(VkContext* ctx, Swapchain* sc, const std::string& backgroundDir) {
    ctx_ = ctx;
    sc_ = sc;

    // LINEAR sampler: the panorama is a photo-like backdrop, not pixel art.
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(ctx_->device, &sampler, nullptr, &sampler_));

    VkDescriptorSetLayoutBinding texBinding{};
    texBinding.binding = 0;
    texBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    texBinding.descriptorCount = 1;
    texBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo texLayoutInfo{};
    texLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    texLayoutInfo.bindingCount = 1;
    texLayoutInfo.pBindings = &texBinding;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx_->device, &texLayoutInfo, nullptr, &texLayout_));

    VkDescriptorPoolSize size{};
    size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    size.descriptorCount = 6;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 6;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &size;
    VK_CHECK(vkCreateDescriptorPool(ctx_->device, &poolInfo, nullptr, &pool_));

    int loaded = 0;
    for (int i = 0; i < 6; ++i) {
        if (loadTexture(backgroundDir + "/panorama_" + std::to_string(i) + ".png", &faces_[i])) {
            ++loaded;
        }
    }
    valid_ = loaded == 6;
    if (!valid_) {
        std::fprintf(stderr,
                     "[Panorama] %d/6 faces found (gui/title/background/panorama_0..5.png); "
                     "title falls back to the live world\n", loaded);
        return;
    }

    createGeometry();
    createPipeline();
}

void PanoramaRenderer::createGeometry() {
    // Inward-facing unit cube around the camera. Face order matches vanilla:
    // 0 front (-Z), 1 right (+X), 2 back (+Z), 3 left (-X), 4 top (+Y), 5 bottom (-Y).
    auto face = [](std::vector<PanoVertex>& v, glm::vec3 bl, glm::vec3 br, glm::vec3 tr,
                   glm::vec3 tl) {
        v.push_back({bl, {0, 1}});
        v.push_back({br, {1, 1}});
        v.push_back({tr, {1, 0}});
        v.push_back({bl, {0, 1}});
        v.push_back({tr, {1, 0}});
        v.push_back({tl, {0, 0}});
    };
    std::vector<PanoVertex> verts;
    verts.reserve(36);
    face(verts, {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1});  // 0 front  (-Z)
    face(verts, {1, -1, -1}, {1, -1, 1}, {1, 1, 1}, {1, 1, -1});      // 1 right  (+X)
    face(verts, {1, -1, 1}, {-1, -1, 1}, {-1, 1, 1}, {1, 1, 1});      // 2 back   (+Z)
    face(verts, {-1, -1, 1}, {-1, -1, -1}, {-1, 1, -1}, {-1, 1, 1});  // 3 left   (-X)
    face(verts, {-1, 1, -1}, {1, 1, -1}, {1, 1, 1}, {-1, 1, 1});      // 4 top    (+Y)
    face(verts, {-1, -1, 1}, {1, -1, 1}, {1, -1, -1}, {-1, -1, -1});  // 5 bottom (-Y)
    cubeVbuf_ = createDeviceBufferWithData(*ctx_, verts.data(), verts.size() * sizeof(PanoVertex),
                                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
}

bool PanoramaRenderer::loadTexture(const std::string& path, Texture* out) {
    int w = 0, h = 0, ch = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!pixels) return false;

    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_SRGB;
    info.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    alloc.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VK_CHECK(vmaCreateImage(ctx_->allocator, &info, &alloc, &out->image, &out->alloc, nullptr));

    VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4;
    AllocatedBuffer staging = createHostBuffer(ctx_->allocator, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(staging.mapped, pixels, static_cast<size_t>(bytes));
    stbi_image_free(pixels);

    ctx_->immediateSubmit([&](VkCommandBuffer cmd) {
        imageBarrier(cmd, out->image, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, out->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        imageBarrier(cmd, out->image, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    destroyBuffer(ctx_->allocator, staging);

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = out->image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R8G8B8A8_SRGB;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(ctx_->device, &view, nullptr, &out->view));

    out->set = allocTexSet(out->view);
    return true;
}

VkDescriptorSet PanoramaRenderer::allocTexSet(VkImageView view) {
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &texLayout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateDescriptorSets(ctx_->device, &alloc, &set));

    VkDescriptorImageInfo img{};
    img.sampler = sampler_;
    img.imageView = view;
    img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &img;
    vkUpdateDescriptorSets(ctx_->device, 1, &write, 0, nullptr);
    return set;
}

VkShaderModule PanoramaRenderer::loadShader(const std::string& path) {
    std::vector<char> code = readFile(path);
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size();
    info.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(ctx_->device, &info, nullptr, &module));
    return module;
}

void PanoramaRenderer::createPipeline() {
    VkShaderModule vert = loadShader(resolve("shaders/panorama.vert.spv"));
    VkShaderModule frag = loadShader(resolve("shaders/panorama.frag.spv"));

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
    binding.stride = sizeof(PanoVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(PanoVertex, pos)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(PanoVertex, uv)};

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
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE; // backdrop: everything draws over it
    depthStencil.depthWriteEnable = VK_FALSE;

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

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push.size = sizeof(PanoPush);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &texLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
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
    VK_CHECK(vkCreateGraphicsPipelines(ctx_->device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_));

    vkDestroyShaderModule(ctx_->device, vert, nullptr);
    vkDestroyShaderModule(ctx_->device, frag, nullptr);
}

void PanoramaRenderer::record(VkCommandBuffer cmd, float time, float aspect) {
    if (!valid_) return;

    // Vanilla-style spin: slow yaw drift with a gentle pitch sway, looking slightly up.
    float yaw = glm::radians(time * 1.8f);
    float pitch = glm::radians(-12.0f + 3.0f * std::sin(time * 0.05f));
    glm::mat4 view = glm::rotate(glm::mat4(1.0f), pitch, glm::vec3(1, 0, 0)) *
                     glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0, 1, 0));
    glm::mat4 proj = glm::perspective(glm::radians(80.0f), aspect, 0.05f, 10.0f);
    proj[1][1] *= -1.0f;

    PanoPush push{proj * view};

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &cubeVbuf_.buffer, &off);
    for (int i = 0; i < 6; ++i) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                                &faces_[i].set, 0, nullptr);
        vkCmdDraw(cmd, 6, 1, i * 6, 0);
    }
}

void PanoramaRenderer::cleanup() {
    if (!ctx_) return;
    if (pipeline_) vkDestroyPipeline(ctx_->device, pipeline_, nullptr);
    if (pipelineLayout_) vkDestroyPipelineLayout(ctx_->device, pipelineLayout_, nullptr);
    destroyBuffer(ctx_->allocator, cubeVbuf_);
    for (Texture& t : faces_) {
        if (t.view) vkDestroyImageView(ctx_->device, t.view, nullptr);
        if (t.image) vmaDestroyImage(ctx_->allocator, t.image, t.alloc);
        t = {};
    }
    if (pool_) vkDestroyDescriptorPool(ctx_->device, pool_, nullptr);
    if (texLayout_) vkDestroyDescriptorSetLayout(ctx_->device, texLayout_, nullptr);
    if (sampler_) vkDestroySampler(ctx_->device, sampler_, nullptr);
}

} // namespace mc
