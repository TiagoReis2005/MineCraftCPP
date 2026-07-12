#include "gfx/WeatherRenderer.h"

#include "core/Paths.h"
#include "gfx/Swapchain.h"
#include "gfx/VkContext.h"
#include "world/World.h"

#include <stb_image.h>
#include <vk_mem_alloc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace mc {
namespace {

constexpr float kFallSpeed = 3.2f; // texture v-units per second (streaks fall)
constexpr float kVScale = 0.25f;   // 4 blocks of world height per texture repeat

struct RainVertex {
    glm::vec3 pos;
    glm::vec2 uv;
};

struct RainPush {
    glm::vec4 tint;
    glm::vec4 offset; // world offset added to positions (cloud drift; zero for rain)
};

// Stable per-column randomness (u offset + scroll phase differ per column).
float columnRand(int x, int z) {
    float v = std::sin(static_cast<float>(x) * 127.1f + static_cast<float>(z) * 311.7f) * 43758.5453f;
    return v - std::floor(v);
}

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

void WeatherRenderer::init(VkContext* ctx, Swapchain* sc, const AllocatedBuffer* cameraUBOs,
                           const std::string& envDir) {
    ctx_ = ctx;
    sc_ = sc;
    createDescriptors(cameraUBOs);
    if (!loadTexture(envDir + "/rain.png", rainImage_, rainAlloc_, rainView_, rainSet_)) {
        std::fprintf(stderr, "[Weather] no rain.png (assets/textures/environment)\n");
    }
    if (!loadTexture(envDir + "/clouds.png", cloudImage_, cloudAlloc_, cloudView_, cloudSet_,
                     &cloudMask_)) {
        std::fprintf(stderr, "[Weather] no clouds.png (assets/textures/environment)\n");
    }
    for (int i = 0; i < kFramesInFlight; ++i) {
        vbuf_[i] = createHostBuffer(ctx_->allocator, kMaxVerts * sizeof(RainVertex),
                                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        cloudVbuf_[i] = createHostBuffer(ctx_->allocator, kMaxCloudVerts * sizeof(CloudVertex),
                                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    }
    pipeline_ = createPipeline(/*depthWrite=*/false);
    cloudPipeline_ = createPipeline(/*depthWrite=*/true);
}

bool WeatherRenderer::loadTexture(const std::string& path, VkImage& rainImage_,
                                  VmaAllocation& rainAlloc_, VkImageView& rainView_,
                                  VkDescriptorSet& rainSet_, std::vector<uint8_t>* maskOut) {
    int w = 0, h = 0, ch = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!pixels) return false;
    if (maskOut) { // CPU copy of the coverage for the 3D cloud mesher
        maskOut->assign(static_cast<size_t>(w) * h, 0);
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
            (*maskOut)[i] = pixels[i * 4 + 3] > 127 ? 1 : 0;
        }
        maskW_ = w;
        maskH_ = h;
    }

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
    VK_CHECK(vmaCreateImage(ctx_->allocator, &info, &alloc, &rainImage_, &rainAlloc_, nullptr));

    VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4;
    AllocatedBuffer staging = createHostBuffer(ctx_->allocator, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(staging.mapped, pixels, static_cast<size_t>(bytes));
    stbi_image_free(pixels);

    ctx_->immediateSubmit([&](VkCommandBuffer cmd) {
        imageBarrier(cmd, rainImage_, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, rainImage_,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        imageBarrier(cmd, rainImage_, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    destroyBuffer(ctx_->allocator, staging);

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = rainImage_;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R8G8B8A8_SRGB;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(ctx_->device, &view, nullptr, &rainView_));

    VkDescriptorSetAllocateInfo setAlloc{};
    setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAlloc.descriptorPool = pool_;
    setAlloc.descriptorSetCount = 1;
    setAlloc.pSetLayouts = &texLayout_;
    VK_CHECK(vkAllocateDescriptorSets(ctx_->device, &setAlloc, &rainSet_));

    VkDescriptorImageInfo img{};
    img.sampler = sampler_;
    img.imageView = rainView_;
    img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = rainSet_;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &img;
    vkUpdateDescriptorSets(ctx_->device, 1, &write, 0, nullptr);
    return true;
}

void WeatherRenderer::createDescriptors(const AllocatedBuffer* cameraUBOs) {
    // Repeat sampler: u tiles across columns, v scrolls the falling streaks.
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_NEAREST;
    sampler.minFilter = VK_FILTER_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
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
    sizes[1].descriptorCount = 2; // rain + clouds
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kFramesInFlight + 2;
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

VkShaderModule WeatherRenderer::loadShader(const std::string& path) {
    std::vector<char> code = readFile(path);
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(ctx_->device, &ci, nullptr, &module));
    return module;
}

VkPipeline WeatherRenderer::createPipeline(bool depthWrite) {
    VkShaderModule vert = loadShader(resolve("shaders/rain.vert.spv"));
    VkShaderModule frag = loadShader(resolve("shaders/rain.frag.spv"));

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
    binding.stride = sizeof(RainVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(RainVertex, pos)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(RainVertex, uv)};
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

    // Depth-tested against the world; rain never writes depth, the cloud boxes do
    // (self-occlusion so overlapping faces don't double-blend).
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = depthWrite ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

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

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(RainPush);

    if (pipelineLayout_ == VK_NULL_HANDLE) { // shared by the rain + cloud pipelines
        VkDescriptorSetLayout layouts[2] = {uboLayout_, texLayout_};
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 2;
        layoutInfo.pSetLayouts = layouts;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        VK_CHECK(vkCreatePipelineLayout(ctx_->device, &layoutInfo, nullptr, &pipelineLayout_));
    }

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
    VkPipeline out = VK_NULL_HANDLE;
    VK_CHECK(vkCreateGraphicsPipelines(ctx_->device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                       &out));

    vkDestroyShaderModule(ctx_->device, vert, nullptr);
    vkDestroyShaderModule(ctx_->device, frag, nullptr);
    return out;
}

void WeatherRenderer::buildCloudMesh(long long acx, long long acz) {
    cloudVerts_.clear();
    for (uint32_t& c : cloudCounts_) c = 0;
    if (cloudMask_.empty() || maskW_ <= 0 || maskH_ <= 0) return;

    auto cloudAt = [&](long long cx, long long cz) -> bool {
        long long mx = ((cx % maskW_) + maskW_) % maskW_;
        long long mz = ((cz % maskH_) + maskH_) % maskH_;
        return cloudMask_[static_cast<size_t>(mz) * maskW_ + static_cast<size_t>(mx)] != 0;
    };
    // Flat-shaded box faces sample the cell's own texel (uv at its center).
    auto cellUV = [&](long long cx, long long cz) {
        long long mx = ((cx % maskW_) + maskW_) % maskW_;
        long long mz = ((cz % maskH_) + maskH_) % maskH_;
        return glm::vec2((static_cast<float>(mx) + 0.5f) / static_cast<float>(maskW_),
                         (static_cast<float>(mz) + 0.5f) / static_cast<float>(maskH_));
    };
    auto quad = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                    const glm::vec3& d, const glm::vec2& uv) {
        cloudVerts_.push_back({a, uv});
        cloudVerts_.push_back({b, uv});
        cloudVerts_.push_back({c, uv});
        cloudVerts_.push_back({a, uv});
        cloudVerts_.push_back({c, uv});
        cloudVerts_.push_back({d, uv});
    };

    // Four passes so each face group gets its own brightness at draw time:
    // 0 = tops, 1 = bottoms, 2 = x sides, 3 = z sides.
    for (int pass = 0; pass < 4; ++pass) {
        size_t before = cloudVerts_.size();
        for (int dz = -cloudRadius_; dz <= cloudRadius_; ++dz) {
            for (int dx = -cloudRadius_; dx <= cloudRadius_; ++dx) {
                if (cloudVerts_.size() + 36 > kMaxCloudVerts) break; // guard the host buffer
                long long cx = acx + dx, cz = acz + dz;
                if (!cloudAt(cx, cz)) continue;
                float x0 = dx * kCloudCell, x1 = x0 + kCloudCell;
                float z0 = dz * kCloudCell, z1 = z0 + kCloudCell;
                float y0 = 0.0f, y1 = kCloudThick;
                glm::vec2 uv = cellUV(cx, cz);
                switch (pass) {
                    case 0:
                        quad({x0, y1, z0}, {x1, y1, z0}, {x1, y1, z1}, {x0, y1, z1}, uv);
                        break;
                    case 1:
                        quad({x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}, uv);
                        break;
                    case 2:
                        if (!cloudAt(cx - 1, cz))
                            quad({x0, y0, z0}, {x0, y1, z0}, {x0, y1, z1}, {x0, y0, z1}, uv);
                        if (!cloudAt(cx + 1, cz))
                            quad({x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1}, uv);
                        break;
                    default:
                        if (!cloudAt(cx, cz - 1))
                            quad({x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0}, uv);
                        if (!cloudAt(cx, cz + 1))
                            quad({x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}, uv);
                        break;
                }
            }
        }
        cloudCounts_[pass] = static_cast<uint32_t>(cloudVerts_.size() - before);
    }
}

void WeatherRenderer::record(VkCommandBuffer cmd, uint32_t frameIndex, const glm::vec3& camPos,
                             const World& world, float time, float intensity,
                             float cloudBrightness) {
    if (pipeline_ == VK_NULL_HANDLE) return;

    int cx = static_cast<int>(std::floor(camPos.x));
    int cy = static_cast<int>(std::floor(camPos.y));
    int cz = static_cast<int>(std::floor(camPos.z));
    int top = cy + 15, bottom = cy - 20;

    std::vector<RainVertex> verts;
    verts.reserve(kMaxVerts);
    bool rainOn = rainSet_ != VK_NULL_HANDLE && intensity >= 0.01f;
    for (int dz = -kRadius; rainOn && dz <= kRadius; ++dz) {
        for (int dx = -kRadius; dx <= kRadius; ++dx) {
            int wx = cx + dx, wz = cz + dz;
            // First solid block from above: rain spans (surface, top]. Under a roof the
            // scan hits the roof and the column collapses -> dry.
            int surface = bottom - 1;
            bool found = false;
            for (int y = top; y >= bottom; --y) {
                if (world.getBlock(wx, y, wz) != BLOCK_AIR) {
                    surface = y;
                    found = true;
                    break;
                }
            }
            // No ground within the band around the camera (e.g. flying high above the
            // world): draw no rain here, otherwise it floats as a hard-edged sky band.
            if (!found) continue;
            float y0 = static_cast<float>(surface + 1);
            float y1 = static_cast<float>(top);
            if (y1 - y0 < 0.5f) continue;

            float cxf = static_cast<float>(wx) + 0.5f;
            float czf = static_cast<float>(wz) + 0.5f;
            // Face the camera (columns right at the camera get an arbitrary facing).
            float tx = camPos.x - cxf, tz = camPos.z - czf;
            float len = std::sqrt(tx * tx + tz * tz);
            float sx = 0.5f, sz = 0.0f;
            if (len > 1e-3f) {
                sx = -tz / len * 0.5f;
                sz = tx / len * 0.5f;
            }

            float rnd = columnRand(wx, wz);
            float u0 = rnd, u1 = rnd + 1.0f;
            float scroll = time * kFallSpeed + rnd * 7.31f;
            float v0 = y0 * kVScale + scroll;
            float v1 = y1 * kVScale + scroll;

            RainVertex q[6] = {
                {{cxf - sx, y0, czf - sz}, {u0, v0}}, {{cxf + sx, y0, czf + sz}, {u1, v0}},
                {{cxf + sx, y1, czf + sz}, {u1, v1}}, {{cxf - sx, y0, czf - sz}, {u0, v0}},
                {{cxf + sx, y1, czf + sz}, {u1, v1}}, {{cxf - sx, y1, czf - sz}, {u0, v1}},
            };
            verts.insert(verts.end(), q, q + 6);
        }
    }
    VkDeviceSize off = 0;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                            &uboSets_[frameIndex], 0, nullptr);

    if (!verts.empty()) {
        std::memcpy(vbuf_[frameIndex].mapped, verts.data(), verts.size() * sizeof(RainVertex));
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1,
                                &rainSet_, 0, nullptr);
        RainPush push{glm::vec4(1.0f, 1.0f, 1.0f, 0.65f * intensity), glm::vec4(0.0f)};
        vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
        vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf_[frameIndex].buffer, &off);
        vkCmdDraw(cmd, static_cast<uint32_t>(verts.size()), 1, 0, 0);
    }

    // 3D clouds: mesh cached in cloud-grid space around an anchor cell, rebuilt only
    // when the camera (or the accumulated drift) crosses a cell; the smooth sub-cell
    // drift rides the push-constant offset.
    if (cloudSet_ != VK_NULL_HANDLE && cloudPipeline_ != VK_NULL_HANDLE) {
        constexpr float kDrift = 0.6f; // blocks per second
        float drift = time * kDrift;
        long long acx = static_cast<long long>(std::floor((camPos.x + drift) / kCloudCell));
        long long acz = static_cast<long long>(std::floor(camPos.z / kCloudCell));

        // Grow the cloud sheet to cover the view distance (LOD if on, else render distance),
        // capped so the host buffer stays bounded. No LOD: clouds are cheap flat boxes.
        int viewChunks = world.lodDistance > 0 ? world.lodDistance : world.renderDistance;
        int desiredR = std::clamp(static_cast<int>(std::ceil(viewChunks * 32.0f / kCloudCell)),
                                  kCloudRadius, kCloudMaxRadius);
        bool radiusChanged = desiredR != cloudRadius_;
        cloudRadius_ = desiredR;

        if (!anchorValid_ || acx != anchorX_ || acz != anchorZ_ || radiusChanged) {
            buildCloudMesh(acx, acz);
            anchorX_ = acx;
            anchorZ_ = acz;
            anchorValid_ = true;
        }
        if (!cloudVerts_.empty()) {
            std::memcpy(cloudVbuf_[frameIndex].mapped, cloudVerts_.data(),
                        cloudVerts_.size() * sizeof(CloudVertex));
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cloudPipeline_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 1, 1,
                                    &cloudSet_, 0, nullptr);
            vkCmdBindVertexBuffers(cmd, 0, 1, &cloudVbuf_[frameIndex].buffer, &off);
            glm::vec4 offset(static_cast<float>(acx) * kCloudCell - drift, kCloudY,
                             static_cast<float>(acz) * kCloudCell, 0.0f);
            // top/bottom/x/z + far-LOD top/bottom (same brightness as the near faces).
            const float faceLight[6] = {1.0f, 0.7f, 0.9f, 0.8f, 1.0f, 0.7f};
            uint32_t first = 0;
            for (int pass = 0; pass < 6; ++pass) {
                if (cloudCounts_[pass] == 0) continue;
                float b = cloudBrightness * faceLight[pass];
                RainPush push{glm::vec4(b, b, b, 0.8f), offset};
                vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                                   sizeof(push), &push);
                vkCmdDraw(cmd, cloudCounts_[pass], 1, first, 0);
                first += cloudCounts_[pass];
            }
        }
    }
}

void WeatherRenderer::cleanup() {
    if (!ctx_) return;
    if (rainView_) vkDestroyImageView(ctx_->device, rainView_, nullptr);
    if (rainImage_) vmaDestroyImage(ctx_->allocator, rainImage_, rainAlloc_);
    if (cloudView_) vkDestroyImageView(ctx_->device, cloudView_, nullptr);
    if (cloudImage_) vmaDestroyImage(ctx_->allocator, cloudImage_, cloudAlloc_);
    for (int i = 0; i < kFramesInFlight; ++i) {
        destroyBuffer(ctx_->allocator, vbuf_[i]);
        destroyBuffer(ctx_->allocator, cloudVbuf_[i]);
    }
    if (cloudPipeline_) vkDestroyPipeline(ctx_->device, cloudPipeline_, nullptr);
    if (pipeline_) vkDestroyPipeline(ctx_->device, pipeline_, nullptr);
    if (pipelineLayout_) vkDestroyPipelineLayout(ctx_->device, pipelineLayout_, nullptr);
    if (pool_) vkDestroyDescriptorPool(ctx_->device, pool_, nullptr);
    if (uboLayout_) vkDestroyDescriptorSetLayout(ctx_->device, uboLayout_, nullptr);
    if (texLayout_) vkDestroyDescriptorSetLayout(ctx_->device, texLayout_, nullptr);
    if (sampler_) vkDestroySampler(ctx_->device, sampler_, nullptr);
    ctx_ = nullptr;
}

} // namespace mc
