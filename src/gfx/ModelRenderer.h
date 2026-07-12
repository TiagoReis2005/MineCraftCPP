#pragma once

#include "game/PlayerModel.h"
#include "gfx/GpuBuffer.h"
#include "gfx/vk_common.h"

#include <glm/glm.hpp>

#include <string>

typedef struct VmaAllocation_T* VmaAllocation;

namespace mc {

class VkContext;
class Swapchain;

// A player pose: one world transform per rig bone (indices match the Rig / the meshes
// built by buildPlayerRigMeshes). `mask` selects which bones draw (bit per bone) — first
// person shows only the right-arm chain, spectator only the ghost head. `opacity` blends
// the whole model (spectator 0.5). `count` is the rig bone count.
struct ModelPose {
    glm::mat4 bones[kMaxModelBones];
    uint32_t  count = 0;
    uint32_t  mask = 0xFFFFFFFFu;
    float     opacity = 1.0f;
};

// Renders the textured player model inside the world pass, one draw per rig bone with a
// non-empty mesh. Shares the Renderer's per-frame camera UBOs; each bone's world
// transform is a push constant.
class ModelRenderer {
public:
    static constexpr int kFramesInFlight = 2;

    void init(VkContext* ctx, Swapchain* sc, const AllocatedBuffer* cameraUBOs,
              const std::string& skinPath, const PlayerRigMeshes& meshes);
    void cleanup();

    void record(VkCommandBuffer cmd, uint32_t frameIndex, const ModelPose& pose);

private:
    VkContext* ctx_ = nullptr;
    Swapchain* sc_ = nullptr;

    VkImage       skinImage_ = VK_NULL_HANDLE;
    VmaAllocation skinAlloc_ = nullptr;
    VkImageView   skinView_ = VK_NULL_HANDLE;
    VkSampler     sampler_ = VK_NULL_HANDLE;

    int             boneCount_ = 0;
    AllocatedBuffer vbuf_[kMaxModelBones]{};
    AllocatedBuffer ibuf_[kMaxModelBones]{};
    uint32_t        indexCount_[kMaxModelBones]{};

    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      pool_ = VK_NULL_HANDLE;
    VkDescriptorSet       sets_[kFramesInFlight]{};

    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline       pipeline_ = VK_NULL_HANDLE;

    void loadSkin(const std::string& path);
    void createDescriptors(const AllocatedBuffer* cameraUBOs);
    void createPipeline();
    VkShaderModule loadShader(const std::string& path);
};

} // namespace mc
