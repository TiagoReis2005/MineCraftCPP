#include "gfx/TextureArray.h"

#include "gfx/GpuBuffer.h"
#include "gfx/VkContext.h"

#include <vk_mem_alloc.h>
#include <stb_image.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace mc {
namespace {

constexpr int kChannels = 4; // RGBA

uint32_t fnv1a(const std::string& s) {
    uint32_t h = 2166136261u;
    for (char c : s) {
        h ^= static_cast<uint8_t>(c);
        h *= 16777619u;
    }
    return h;
}

// Deterministic per-name color with light per-pixel variation and a darker grid border,
// so missing textures look like distinct, readable blocks rather than uniform magenta.
void generatePlaceholder(const std::string& name, int size, std::vector<uint8_t>& out) {
    uint32_t h = fnv1a(name);
    int baseR = 50 + static_cast<int>(h & 0x7F);
    int baseG = 50 + static_cast<int>((h >> 8) & 0x7F);
    int baseB = 50 + static_cast<int>((h >> 16) & 0x7F);

    out.resize(static_cast<size_t>(size) * size * kChannels);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            uint32_t n = fnv1a(name + std::to_string(x * 31 + y));
            int jitter = static_cast<int>(n & 0x1F) - 16; // -16..+15
            bool border = (x == 0 || y == 0 || x == size - 1 || y == size - 1);
            int mul = border ? -40 : jitter;
            auto clamp = [](int v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); };
            size_t i = (static_cast<size_t>(y) * size + x) * kChannels;
            out[i + 0] = clamp(baseR + mul);
            out[i + 1] = clamp(baseG + mul);
            out[i + 2] = clamp(baseB + mul);
            out[i + 3] = 255;
        }
    }
}

void imageBarrier(VkCommandBuffer cmd, VkImage image, uint32_t layerCount,
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
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layerCount};

    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

void TextureArray::build(VkContext& ctx, const std::vector<std::string>& names,
                         const std::string& blocksDir, int defaultSize) {
    namespace fs = std::filesystem;

    // Pass 1: auto-detect texture size from the first PNG that loads.
    int size = defaultSize;
    for (const std::string& name : names) {
        std::string path = (fs::path(blocksDir) / (name + ".png")).string();
        int w = 0, h = 0, ch = 0;
        if (stbi_info(path.c_str(), &w, &h, &ch) && w == h && w > 0) {
            size = w;
            break;
        }
    }

    const size_t layerBytes = static_cast<size_t>(size) * size * kChannels;
    layerCount_ = static_cast<uint32_t>(names.size());
    if (layerCount_ == 0) {
        // Always have at least one (magenta) layer so the array image is valid.
        layerCount_ = 1;
    }

    std::vector<uint8_t> pixels(layerBytes * layerCount_);
    int loaded = 0, placeholders = 0;

    for (uint32_t i = 0; i < static_cast<uint32_t>(names.size()); ++i) {
        const std::string& name = names[i];
        layers_[name] = i;

        std::string path = (fs::path(blocksDir) / (name + ".png")).string();
        int w = 0, h = 0, ch = 0;
        stbi_uc* data = stbi_load(path.c_str(), &w, &h, &ch, kChannels);
        if (data && w == size && h == size) {
            std::memcpy(pixels.data() + static_cast<size_t>(i) * layerBytes, data, layerBytes);
            ++loaded;
        } else {
            if (data) {
                std::fprintf(stderr, "[TextureArray] '%s' is %dx%d, expected %dx%d; using placeholder.\n",
                             name.c_str(), w, h, size, size);
            }
            std::vector<uint8_t> ph;
            generatePlaceholder(name, size, ph);
            std::memcpy(pixels.data() + static_cast<size_t>(i) * layerBytes, ph.data(), layerBytes);
            ++placeholders;
        }
        if (data) stbi_image_free(data);
    }
    if (names.empty()) {
        std::vector<uint8_t> ph;
        generatePlaceholder("__missing__", size, ph);
        std::memcpy(pixels.data(), ph.data(), layerBytes);
    }
    std::fprintf(stderr, "[TextureArray] %dx%d, %u layers (%d from PNG, %d placeholder)\n",
                 size, size, layerCount_, loaded, placeholders);

    // Per-layer average color for LOD tinting. Convert sRGB bytes to linear before
    // averaging (textures sample as sRGB, so a raw byte mean would read too bright), and
    // weight by alpha so cutout textures (leaves/glass) average only their opaque pixels.
    auto srgbToLinear = [](uint8_t b) {
        float c = b / 255.0f;
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    layerAvg_.assign(layerCount_, glm::vec3(0.5f));
    for (uint32_t i = 0; i < layerCount_ && static_cast<size_t>(i) * layerBytes < pixels.size(); ++i) {
        glm::dvec3 sum(0.0);
        double wsum = 0.0;
        const uint8_t* p = pixels.data() + static_cast<size_t>(i) * layerBytes;
        for (int t = 0; t < size * size; ++t) {
            double a = p[t * kChannels + 3] / 255.0;
            sum.x += srgbToLinear(p[t * kChannels + 0]) * a;
            sum.y += srgbToLinear(p[t * kChannels + 1]) * a;
            sum.z += srgbToLinear(p[t * kChannels + 2]) * a;
            wsum += a;
        }
        if (wsum > 0.0) layerAvg_[i] = glm::vec3(sum / wsum);
    }

    // Create the device-local array image.
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    imageInfo.extent = {static_cast<uint32_t>(size), static_cast<uint32_t>(size), 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = layerCount_;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VK_CHECK(vmaCreateImage(ctx.allocator, &imageInfo, &allocInfo, &image_, &allocation_, nullptr));

    // Upload all layers through one staging buffer.
    AllocatedBuffer staging = createHostBuffer(ctx.allocator, pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(staging.mapped, pixels.data(), pixels.size());

    std::vector<VkBufferImageCopy> regions(layerCount_);
    for (uint32_t i = 0; i < layerCount_; ++i) {
        VkBufferImageCopy& r = regions[i];
        r = {};
        r.bufferOffset = static_cast<VkDeviceSize>(i) * layerBytes;
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.imageSubresource.mipLevel = 0;
        r.imageSubresource.baseArrayLayer = i;
        r.imageSubresource.layerCount = 1;
        r.imageExtent = {static_cast<uint32_t>(size), static_cast<uint32_t>(size), 1};
    }

    ctx.immediateSubmit([&](VkCommandBuffer cmd) {
        imageBarrier(cmd, image_, layerCount_,
                     VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(cmd, staging.buffer, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<uint32_t>(regions.size()), regions.data());
        imageBarrier(cmd, image_, layerCount_,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });

    destroyBuffer(ctx.allocator, staging);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = layerCount_;
    VK_CHECK(vkCreateImageView(ctx.device, &viewInfo, nullptr, &view_));

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST; // crisp Minecraft-style pixels
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.maxLod = 0.0f;
    VK_CHECK(vkCreateSampler(ctx.device, &samplerInfo, nullptr, &sampler_));
}

uint32_t TextureArray::layer(const std::string& name) const {
    auto it = layers_.find(name);
    return it == layers_.end() ? 0u : it->second;
}

void TextureArray::destroy(VkContext& ctx) {
    if (sampler_) vkDestroySampler(ctx.device, sampler_, nullptr);
    if (view_) vkDestroyImageView(ctx.device, view_, nullptr);
    if (image_) vmaDestroyImage(ctx.allocator, image_, allocation_);
    sampler_ = VK_NULL_HANDLE;
    view_ = VK_NULL_HANDLE;
    image_ = VK_NULL_HANDLE;
    allocation_ = nullptr;
}

} // namespace mc
