#pragma once

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include <string>
#include <unordered_map>
#include <vector>

typedef struct VmaAllocation_T* VmaAllocation;

namespace mc {

class VkContext;

// Loads block textures by filename from a folder into a GPU 2D texture array.
// Each requested name becomes one array layer. Names without a matching PNG get a
// distinct procedurally generated placeholder so the engine renders without art.
class TextureArray {
public:
    // `names` are texture base names (no extension). Files are looked up as
    // `<blocksDir>/<name>.png`. Texture size is auto-detected from the first valid PNG.
    void build(VkContext& ctx, const std::vector<std::string>& names,
               const std::string& blocksDir, int defaultSize = 16);
    void destroy(VkContext& ctx);

    // Array layer for a texture name; returns 0 (first layer) if unknown.
    uint32_t layer(const std::string& name) const;

    // Average (alpha-weighted) linear-space color of a layer, used to tint distant LOD
    // tiles that stand in for the real textured blocks. Unknown -> mid grey.
    glm::vec3 averageColor(uint32_t layer) const {
        return layer < layerAvg_.size() ? layerAvg_[layer] : glm::vec3(0.5f);
    }
    glm::vec3 averageColor(const std::string& name) const { return averageColor(layer(name)); }

    VkImageView view() const { return view_; }
    VkSampler sampler() const { return sampler_; }
    uint32_t layerCount() const { return layerCount_; }

private:
    VkImage       image_      = VK_NULL_HANDLE;
    VmaAllocation allocation_ = nullptr;
    VkImageView   view_       = VK_NULL_HANDLE;
    VkSampler     sampler_    = VK_NULL_HANDLE;
    uint32_t      layerCount_ = 0;
    std::unordered_map<std::string, uint32_t> layers_;
    std::vector<glm::vec3> layerAvg_; // per-layer average linear color (LOD tinting)
};

} // namespace mc
