#pragma once

#include "gfx/GpuBuffer.h"
#include "gfx/vk_common.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

typedef struct VmaAllocation_T* VmaAllocation;

namespace mc {

class VkContext;
class Swapchain;

struct UIVertex {
    glm::vec2 pos; // normalized device coords
    glm::vec2 uv;
    glm::vec4 color;
};

// Immediate-mode 2D renderer drawn inside the world's render pass (over the scene, no depth
// test). Supports solid quads, textured sprites (PNGs registered up front), and a
// color-inverting quad for the crosshair. Rebuilt every frame.
class UIRenderer {
public:
    void init(VkContext* ctx, Swapchain* sc);
    void cleanup();

    // Loads a PNG and returns its texture id (>= 1). Id 0 is a built-in white pixel.
    int registerTexture(const std::string& pngPath);

    // Registers an externally-owned image view (e.g. a rendered block icon) as a UI texture.
    // The caller keeps ownership of the image/view; only the descriptor set is managed here.
    int registerImageView(VkImageView view);

    void beginFrame(uint32_t screenW, uint32_t screenH);
    void quad(float x, float y, float w, float h, const glm::vec4& color);
    void sprite(int texId, float x, float y, float w, float h, const glm::vec4& color = glm::vec4(1.0f));
    // Textured quad with explicit UVs (e.g. a font glyph from an atlas).
    void texQuad(int texId, float x, float y, float w, float h,
                 float u0, float v0, float u1, float v1, const glm::vec4& color = glm::vec4(1.0f));
    void invertQuad(float x, float y, float w, float h); // inverts the pixels behind it
    void recordDraw(VkCommandBuffer cmd, uint32_t frameIndex);

private:
    static constexpr int kFramesInFlight = 2;
    static constexpr size_t kMaxVerts = 6 * 8192;
    // Every block icon takes a slot, so this scales with the registry (plus HUD sprites,
    // the font, the inventory panel, spectator icons...).
    static constexpr uint32_t kMaxTextures = 512;

    VkContext* ctx_ = nullptr;
    Swapchain* sc_ = nullptr;
    float screenW_ = 1.0f;
    float screenH_ = 1.0f;

    std::vector<UIVertex> verts_;
    struct DrawCmd { int texId; bool invert; uint32_t first; uint32_t count; };
    std::vector<DrawCmd> cmds_;
    AllocatedBuffer vbuf_[kFramesInFlight]{};

    struct UITexture {
        VkImage         image = VK_NULL_HANDLE;
        VmaAllocation   alloc = nullptr;
        VkImageView     view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        bool            owned = true; // false for externally-owned images (icons)
    };
    std::vector<UITexture> textures_;

    VkSampler             sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      pool_ = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline            pipeline_ = VK_NULL_HANDLE;
    VkPipeline            invertPipeline_ = VK_NULL_HANDLE;

    void createSamplerAndLayout();
    void createBuffers();
    VkPipeline buildPipeline(bool invert);
    VkShaderModule loadShader(const std::string& path);
    int createTexture(const uint8_t* rgba, int w, int h);
    void addQuad(int texId, bool invert, float x, float y, float w, float h,
                 float u0, float v0, float u1, float v1, const glm::vec4& color);
    void pushVertex(float px, float py, float u, float v, const glm::vec4& color);
};

} // namespace mc
