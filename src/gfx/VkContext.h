#pragma once

#include "gfx/vk_common.h"

#include <functional>

// Forward-declare the opaque VMA allocator handle (matches VK_DEFINE_HANDLE).
typedef struct VmaAllocator_T* VmaAllocator;

struct GLFWwindow;

namespace mc {

// Owns the core Vulkan objects shared by the whole renderer: instance, debug messenger,
// surface, physical/logical device and the graphics queue (which also handles present).
class VkContext {
public:
    void init(GLFWwindow* window);
    void cleanup();

    VkInstance               instance        = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger  = VK_NULL_HANDLE;
    VkSurfaceKHR             surface         = VK_NULL_HANDLE;
    VkPhysicalDevice        physicalDevice  = VK_NULL_HANDLE;
    VkDevice                device          = VK_NULL_HANDLE;

    uint32_t graphicsFamily = 0;            // family used for graphics + present
    VkQueue  graphicsQueue  = VK_NULL_HANDLE;

    VmaAllocator allocator = nullptr;
    std::string  deviceName; // selected GPU name (for the debug overlay)

    // Runs a callback that records into a transient command buffer, submits it, and
    // blocks until the GPU finishes. Used for texture/buffer uploads and layout setup.
    void immediateSubmit(const std::function<void(VkCommandBuffer)>& fn);

    // Bytes currently allocated through VMA (device memory in use by the app).
    uint64_t gpuMemoryUsedBytes() const;

private:
    void createInstance();
    void setupDebugMessenger();
    void createSurface(GLFWwindow* window);
    void pickPhysicalDevice();
    void createLogicalDevice();
    void createAllocator();
    void createUploadContext();

    bool validationEnabled_ = false;

    VkCommandPool uploadPool_  = VK_NULL_HANDLE;
    VkFence       uploadFence_ = VK_NULL_HANDLE;
};

} // namespace mc
