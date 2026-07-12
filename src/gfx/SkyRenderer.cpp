#include "gfx/SkyRenderer.h"

#include "core/Paths.h"
#include "gfx/Swapchain.h"
#include "gfx/VkContext.h"

#include <stb_image.h>
#include <vk_mem_alloc.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <vector>

namespace mc {
namespace {

// Vanilla proportions: the sun quad's half-size is 0.30x its distance, the moon 0.20x.
// Distance just needs to sit behind the terrain and inside the far plane (1000).
constexpr float kDist = 300.0f;
constexpr float kSunSize = 0.30f * kDist;
constexpr float kMoonSize = 0.20f * kDist;
constexpr int kStarCount = 900;

// Vanilla moon phase order (phase = (day-1) % 8, day 1 = full moon).
const char* kMoonFiles[8] = {"full_moon",       "waning_gibbous", "third_quarter",
                             "waning_crescent", "new_moon",       "waxing_crescent",
                             "first_quarter",   "waxing_gibbous"};

struct SkyVertex {
    glm::vec3 pos;
    glm::vec2 uv;
};

struct SkyPush {
    glm::mat4 model;
    glm::vec4 tint;
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

void SkyRenderer::init(VkContext* ctx, Swapchain* sc, const AllocatedBuffer* cameraUBOs,
                       const std::string& envDir) {
    ctx_ = ctx;
    sc_ = sc;
    createGeometry();
    createDescriptors(cameraUBOs);

    createWhite(&white_);
    if (!loadTexture(envDir + "/celestial/sun.png", &sun_)) {
        std::fprintf(stderr, "[Sky] no sun.png (assets/textures/environment/celestial)\n");
    }
    int phases = 0;
    for (int i = 0; i < 8; ++i) {
        if (loadTexture(envDir + "/celestial/moon/" + kMoonFiles[i] + ".png", &moon_[i])) ++phases;
    }
    if (phases < 8) {
        std::fprintf(stderr, "[Sky] %d/8 moon phase textures found "
                             "(assets/textures/environment/celestial/moon)\n", phases);
    }

    createPipeline();
}

void SkyRenderer::createGeometry() {
    // Billboard quad with normal +X: model = R * translate(dist,0,0) * scale(size).
    const SkyVertex quad[6] = {
        {{0.0f, -1.0f, -1.0f}, {0.0f, 1.0f}}, {{0.0f, -1.0f, 1.0f}, {1.0f, 1.0f}},
        {{0.0f, 1.0f, 1.0f}, {1.0f, 0.0f}},   {{0.0f, -1.0f, -1.0f}, {0.0f, 1.0f}},
        {{0.0f, 1.0f, 1.0f}, {1.0f, 0.0f}},   {{0.0f, 1.0f, -1.0f}, {0.0f, 0.0f}},
    };
    quadVbuf_ = createDeviceBufferWithData(*ctx_, quad, sizeof(quad),
                                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

    // Star quads scattered on the sky sphere (fixed seed: the same sky every night).
    std::mt19937 rng(9469);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    std::vector<SkyVertex> stars;
    stars.reserve(kStarCount * 6);
    for (int i = 0; i < kStarCount; ++i) {
        // Uniform direction on the sphere.
        float z = uni(rng) * 2.0f - 1.0f;
        float phi = uni(rng) * 6.2831853f;
        float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        glm::vec3 d(r * std::cos(phi), r * std::sin(phi), z);
        // Orthonormal basis around it, with a random spin.
        glm::vec3 ref = std::fabs(d.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
        glm::vec3 u = glm::normalize(glm::cross(ref, d));
        glm::vec3 v = glm::cross(d, u);
        float spin = uni(rng) * 6.2831853f;
        glm::vec3 su = u * std::cos(spin) + v * std::sin(spin);
        glm::vec3 sv = v * std::cos(spin) - u * std::sin(spin);
        float size = (0.12f + 0.10f * uni(rng)) * (kDist / 100.0f); // small vanilla-like points
        glm::vec3 c = d * kDist;
        glm::vec3 a1 = su * size, a2 = sv * size;
        SkyVertex q[6] = {
            {c - a1 - a2, {0, 1}}, {c + a1 - a2, {1, 1}}, {c + a1 + a2, {1, 0}},
            {c - a1 - a2, {0, 1}}, {c + a1 + a2, {1, 0}}, {c - a1 + a2, {0, 0}},
        };
        stars.insert(stars.end(), q, q + 6);
    }
    starVertexCount_ = static_cast<uint32_t>(stars.size());
    starVbuf_ = createDeviceBufferWithData(*ctx_, stars.data(), stars.size() * sizeof(SkyVertex),
                                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
}

bool SkyRenderer::loadTexture(const std::string& path, Texture* out) {
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

void SkyRenderer::createWhite(Texture* out) {
    const uint8_t white[4] = {255, 255, 255, 255};

    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_SRGB;
    info.extent = {1, 1, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    alloc.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VK_CHECK(vmaCreateImage(ctx_->allocator, &info, &alloc, &out->image, &out->alloc, nullptr));

    AllocatedBuffer staging = createHostBuffer(ctx_->allocator, 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(staging.mapped, white, 4);
    ctx_->immediateSubmit([&](VkCommandBuffer cmd) {
        imageBarrier(cmd, out->image, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {1, 1, 1};
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
}

VkDescriptorSet SkyRenderer::allocTexSet(VkImageView view) {
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

void SkyRenderer::createDescriptors(const AllocatedBuffer* cameraUBOs) {
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_NEAREST;
    sampler.minFilter = VK_FILTER_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(ctx_->device, &sampler, nullptr, &sampler_));

    VkDescriptorSetLayoutBinding uboBinding{};
    uboBinding.binding = 0;
    uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboBinding.descriptorCount = 1;
    uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo uboLayoutInfo{};
    uboLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    uboLayoutInfo.bindingCount = 1;
    uboLayoutInfo.pBindings = &uboBinding;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx_->device, &uboLayoutInfo, nullptr, &uboLayout_));

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

    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[0].descriptorCount = kFramesInFlight;
    sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[1].descriptorCount = 10; // sun + 8 moon phases + white
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kFramesInFlight + 10;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(ctx_->device, &poolInfo, nullptr, &pool_));

    for (int i = 0; i < kFramesInFlight; ++i) {
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = pool_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &uboLayout_;
        VK_CHECK(vkAllocateDescriptorSets(ctx_->device, &alloc, &uboSets_[i]));

        VkDescriptorBufferInfo buf{};
        buf.buffer = cameraUBOs[i].buffer;
        buf.range = VK_WHOLE_SIZE;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = uboSets_[i];
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo = &buf;
        vkUpdateDescriptorSets(ctx_->device, 1, &write, 0, nullptr);
    }
}

VkShaderModule SkyRenderer::loadShader(const std::string& path) {
    std::vector<char> code = readFile(path);
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(ctx_->device, &ci, nullptr, &module));
    return module;
}

void SkyRenderer::createPipeline() {
    VkShaderModule vert = loadShader(resolve("shaders/sky.vert.spv"));
    VkShaderModule frag = loadShader(resolve("shaders/sky.frag.spv"));

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
    binding.stride = sizeof(SkyVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SkyVertex, pos)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(SkyVertex, uv)};
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

    // Drawn before the world with neither depth test nor write: terrain covers the sky.
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;

    // Additive blend (premultiplied in the shader), like Minecraft's celestial pass.
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
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

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(SkyPush);

    VkDescriptorSetLayout layouts[2] = {uboLayout_, texLayout_};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = layouts;
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
    VK_CHECK(vkCreateGraphicsPipelines(ctx_->device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                       &pipeline_));

    vkDestroyShaderModule(ctx_->device, vert, nullptr);
    vkDestroyShaderModule(ctx_->device, frag, nullptr);
}

void SkyRenderer::record(VkCommandBuffer cmd, uint32_t frameIndex, const glm::vec3& camPos,
                         float sunAngle, int moonPhase, float starAlpha, float brightness) {
    if (pipeline_ == VK_NULL_HANDLE) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                            &uboSets_[frameIndex], 0, nullptr);

    auto draw = [&](const Texture& tex, const glm::mat4& model, const glm::vec4& tint,
                    VkBuffer vbuf, uint32_t count) {
        if (tex.set == VK_NULL_HANDLE) return;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1,
                                &tex.set, 0, nullptr);
        SkyPush push{model, tint};
        vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &off);
        vkCmdDraw(cmd, count, 1, 0, 0);
    };

    // Whole sky rotates about the world Z axis: dawn sun on the +X horizon, noon overhead.
    glm::mat4 base = glm::translate(glm::mat4(1.0f), camPos) *
                     glm::rotate(glm::mat4(1.0f), sunAngle, glm::vec3(0, 0, 1));

    if (starAlpha > 0.01f) {
        draw(white_, base, glm::vec4(1.0f, 1.0f, 1.0f, starAlpha), starVbuf_.buffer,
             starVertexCount_);
    }
    glm::vec4 celestialTint(brightness, brightness, brightness, 1.0f);
    glm::mat4 sunM = base * glm::translate(glm::mat4(1.0f), glm::vec3(kDist, 0, 0)) *
                     glm::scale(glm::mat4(1.0f), glm::vec3(kSunSize));
    draw(sun_, sunM, celestialTint, quadVbuf_.buffer, 6);

    const Texture& moon = moon_[((moonPhase % 8) + 8) % 8];
    glm::mat4 moonM = base * glm::rotate(glm::mat4(1.0f), 3.14159265f, glm::vec3(0, 0, 1)) *
                      glm::translate(glm::mat4(1.0f), glm::vec3(kDist, 0, 0)) *
                      glm::scale(glm::mat4(1.0f), glm::vec3(kMoonSize));
    draw(moon, moonM, celestialTint, quadVbuf_.buffer, 6);
}

void SkyRenderer::destroyTexture(Texture& t) {
    if (t.view) vkDestroyImageView(ctx_->device, t.view, nullptr);
    if (t.image) vmaDestroyImage(ctx_->allocator, t.image, t.alloc);
    t = {};
}

void SkyRenderer::cleanup() {
    if (!ctx_) return;
    destroyTexture(sun_);
    for (Texture& m : moon_) destroyTexture(m);
    destroyTexture(white_);
    destroyBuffer(ctx_->allocator, quadVbuf_);
    destroyBuffer(ctx_->allocator, starVbuf_);
    if (pipeline_) vkDestroyPipeline(ctx_->device, pipeline_, nullptr);
    if (pipelineLayout_) vkDestroyPipelineLayout(ctx_->device, pipelineLayout_, nullptr);
    if (pool_) vkDestroyDescriptorPool(ctx_->device, pool_, nullptr);
    if (uboLayout_) vkDestroyDescriptorSetLayout(ctx_->device, uboLayout_, nullptr);
    if (texLayout_) vkDestroyDescriptorSetLayout(ctx_->device, texLayout_, nullptr);
    if (sampler_) vkDestroySampler(ctx_->device, sampler_, nullptr);
    ctx_ = nullptr;
}

} // namespace mc
