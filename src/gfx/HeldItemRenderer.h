#pragma once

#include "gfx/GpuBuffer.h"
#include "gfx/vk_common.h"
#include "world/Mesher.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace mc {

class VkContext;
class Swapchain;
class TextureArray;
class BlockRegistry;

// Draws the selected block as a small cube attached to the player's hand (1st and 3rd
// person). Shares the Renderer's per-frame camera UBOs and the block texture array;
// one mesh per registered block is built once at init via buildSingleBlock.
class HeldItemRenderer {
public:
    static constexpr int kFramesInFlight = 2;

    void init(VkContext* ctx, Swapchain* sc, const AllocatedBuffer* cameraUBOs,
              const TextureArray* textures, const BlockRegistry* registry);
    void cleanup();

    void record(VkCommandBuffer cmd, uint32_t frameIndex, const glm::mat4& transform,
                uint16_t block);

private:
    VkContext* ctx_ = nullptr;
    Swapchain* sc_ = nullptr;

    std::vector<ChunkMesh> meshes_; // indexed by BlockId (empty mesh for air/unknown)

    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      pool_ = VK_NULL_HANDLE;
    VkDescriptorSet       sets_[kFramesInFlight]{};

    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline       pipeline_ = VK_NULL_HANDLE;

    void createDescriptors(const AllocatedBuffer* cameraUBOs, const TextureArray* textures);
    void createPipeline();
    VkShaderModule loadShader(const std::string& path);
};

} // namespace mc
