#include "gfx/VkContext.h"

#include <GLFW/glfw3.h>
#include <vk_mem_alloc.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace mc {
namespace {

#if defined(MC_DEBUG)
constexpr bool kEnableValidationDefault = true;
#else
constexpr bool kEnableValidationDefault = false;
#endif

const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
const std::vector<const char*> kDeviceExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* /*userData*/) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::fprintf(stderr, "[Vulkan] %s\n", data->pMessage);
    }
    return VK_FALSE;
}

void populateDebugCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& ci) {
    ci = {};
    ci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci.pfnUserCallback = debugCallback;
}

bool validationLayerAvailable() {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const auto& l : layers) {
        if (std::strcmp(l.layerName, kValidationLayer) == 0) return true;
    }
    return false;
}

bool deviceSupportsExtensions(VkPhysicalDevice dev) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, available.data());
    for (const char* required : kDeviceExtensions) {
        bool found = false;
        for (const auto& ext : available) {
            if (std::strcmp(ext.extensionName, required) == 0) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

bool deviceSupportsVk13Features(VkPhysicalDevice dev) {
    VkPhysicalDeviceVulkan13Features f13{};
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceFeatures2 f2{};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &f13;
    vkGetPhysicalDeviceFeatures2(dev, &f2);
    return f13.dynamicRendering && f13.synchronization2;
}

// Returns true and sets outFamily if the device has a queue family that supports
// both graphics and presentation to the given surface.
bool findGraphicsPresentFamily(VkPhysicalDevice dev, VkSurfaceKHR surface, uint32_t& outFamily) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, families.data());
    for (uint32_t i = 0; i < count; ++i) {
        if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface, &present);
        if (present) { outFamily = i; return true; }
    }
    return false;
}

} // namespace

void VkContext::init(GLFWwindow* window) {
    validationEnabled_ = kEnableValidationDefault && validationLayerAvailable();
    if (kEnableValidationDefault && !validationEnabled_) {
        std::fprintf(stderr, "[Vulkan] validation layer requested but not available; "
                             "install the Vulkan SDK for validation.\n");
    }
    createInstance();
    setupDebugMessenger();
    createSurface(window);
    pickPhysicalDevice();
    createLogicalDevice();
    createAllocator();
    createUploadContext();
}

void VkContext::createAllocator() {
    VmaAllocatorCreateInfo ci{};
    ci.physicalDevice = physicalDevice;
    ci.device = device;
    ci.instance = instance;
    ci.vulkanApiVersion = VK_API_VERSION_1_3;
    VK_CHECK(vmaCreateAllocator(&ci, &allocator));
}

void VkContext::createUploadContext() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = graphicsFamily;
    VK_CHECK(vkCreateCommandPool(device, &poolInfo, nullptr, &uploadPool_));

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VK_CHECK(vkCreateFence(device, &fenceInfo, nullptr, &uploadFence_));
}

void VkContext::immediateSubmit(const std::function<void(VkCommandBuffer)>& fn) {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = uploadPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(device, &allocInfo, &cmd));

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
    fn(cmd);
    VK_CHECK(vkEndCommandBuffer(cmd));

    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = cmd;

    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;

    VK_CHECK(vkQueueSubmit2(graphicsQueue, 1, &submit, uploadFence_));
    VK_CHECK(vkWaitForFences(device, 1, &uploadFence_, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(device, 1, &uploadFence_));
    vkResetCommandPool(device, uploadPool_, 0);
}

void VkContext::createInstance() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "MineCraftCPP";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "MineCraftCPP";
    app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion = VK_API_VERSION_1_3;

    uint32_t glfwExtCount = 0;
    const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
    std::vector<const char*> extensions(glfwExts, glfwExts + glfwExtCount);
    if (validationEnabled_) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();

    VkDebugUtilsMessengerCreateInfoEXT dbg{};
    if (validationEnabled_) {
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = &kValidationLayer;
        populateDebugCreateInfo(dbg);
        ci.pNext = &dbg; // covers vkCreateInstance / vkDestroyInstance themselves
    }

    VK_CHECK(vkCreateInstance(&ci, nullptr, &instance));
}

void VkContext::setupDebugMessenger() {
    if (!validationEnabled_) return;
    auto fn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    if (!fn) return;
    VkDebugUtilsMessengerCreateInfoEXT ci{};
    populateDebugCreateInfo(ci);
    VK_CHECK(fn(instance, &ci, nullptr, &debugMessenger));
}

void VkContext::createSurface(GLFWwindow* window) {
    VK_CHECK(glfwCreateWindowSurface(instance, window, nullptr, &surface));
}

void VkContext::pickPhysicalDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (count == 0) throw std::runtime_error("No Vulkan-capable GPU found");
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    VkPhysicalDevice best = VK_NULL_HANDLE;
    uint32_t bestFamily = 0;
    int bestScore = -1;

    for (VkPhysicalDevice dev : devices) {
        uint32_t family = 0;
        if (!findGraphicsPresentFamily(dev, surface, family)) continue;
        if (!deviceSupportsExtensions(dev)) continue;
        if (!deviceSupportsVk13Features(dev)) continue;

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev, &props);
        int score = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) ? 1000 : 100;
        if (score > bestScore) {
            bestScore = score;
            best = dev;
            bestFamily = family;
        }
    }

    if (best == VK_NULL_HANDLE) {
        throw std::runtime_error(
            "No GPU supports the required Vulkan 1.3 features (dynamic rendering + "
            "synchronization2). Update your graphics drivers.");
    }

    physicalDevice = best;
    graphicsFamily = bestFamily;

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physicalDevice, &props);
    deviceName = props.deviceName;
    std::fprintf(stderr, "[Vulkan] Using GPU: %s\n", props.deviceName);
}

uint64_t VkContext::gpuMemoryUsedBytes() const {
    if (!allocator) return 0;
    VmaTotalStatistics stats{};
    vmaCalculateStatistics(allocator, &stats);
    return stats.total.statistics.allocationBytes;
}

void VkContext::createLogicalDevice() {
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = graphicsFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    VkPhysicalDeviceVulkan13Features f13{};
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;

    VkPhysicalDeviceFeatures2 f2{};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &f13;

    VkDeviceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.pNext = &f2; // enabling features via pNext; pEnabledFeatures stays null
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &queueInfo;
    ci.enabledExtensionCount = static_cast<uint32_t>(kDeviceExtensions.size());
    ci.ppEnabledExtensionNames = kDeviceExtensions.data();
    // Note: device-level layers are deprecated/ignored since Vulkan 1.0. Validation is
    // enabled at the instance level only; setting enabledLayerCount here is a spec violation.

    VK_CHECK(vkCreateDevice(physicalDevice, &ci, nullptr, &device));
    vkGetDeviceQueue(device, graphicsFamily, 0, &graphicsQueue);
}

void VkContext::cleanup() {
    if (device) {
        if (uploadFence_) vkDestroyFence(device, uploadFence_, nullptr);
        if (uploadPool_) vkDestroyCommandPool(device, uploadPool_, nullptr);
        if (allocator) {
            vmaDestroyAllocator(allocator);
            allocator = nullptr;
        }
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }
    if (surface) {
        vkDestroySurfaceKHR(instance, surface, nullptr);
        surface = VK_NULL_HANDLE;
    }
    if (debugMessenger) {
        auto fn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (fn) fn(instance, debugMessenger, nullptr);
        debugMessenger = VK_NULL_HANDLE;
    }
    if (instance) {
        vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }
}

} // namespace mc
