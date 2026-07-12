#pragma once

#include "gfx/GpuBuffer.h"
#include "gfx/vk_common.h"
#include "world/Block.h"
#include "world/Mesher.h"

#include <string>
#include <vector>

typedef struct VmaAllocation_T* VmaAllocation;

namespace mc {

class VkContext;
class TextureArray;
class BlockRegistry;
class UIRenderer;

// Renders each block as a 3D isometric cube into a small offscreen texture (once), then
// registers those textures with the UIRenderer so the hotbar can draw them like Minecraft.
class ItemIconRenderer {
public:
    void init(VkContext* ctx, TextureArray* blockTex, const BlockRegistry* reg, UIRenderer* ui);
    void cleanup();

    // UI texture id for a block's icon, or -1 if it has none (e.g. Air).
    int iconTexId(BlockId id) const;

private:
    static constexpr int kIconSize = 64;

    struct Icon {
        VkImage       image = VK_NULL_HANDLE;
        VmaAllocation alloc = nullptr;
        VkImageView   view = VK_NULL_HANDLE;
        ChunkMesh     mesh;
        int           uiTexId = -1;
    };

    VkContext* ctx_ = nullptr;
    std::vector<Icon> icons_;

    VkImage       depthImage_ = VK_NULL_HANDLE;
    VmaAllocation depthAlloc_ = nullptr;
    VkImageView   depthView_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      pool_ = VK_NULL_HANDLE;
    VkDescriptorSet       set_ = VK_NULL_HANDLE;
    AllocatedBuffer       ubo_{};

    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline       pipeline_ = VK_NULL_HANDLE;

    void createDepth();
    void createDescriptor(TextureArray* blockTex);
    void createPipeline();
    void renderIcons(const BlockRegistry* reg, UIRenderer* ui);
    VkShaderModule loadShader(const std::string& path);
    ChunkMeshData buildCube(const BlockRegistry* reg, BlockId id);
};

} // namespace mc
