#pragma once

#include <vulkan/vulkan.h>

#include <stdexcept>
#include <string>

namespace mc {

inline const char* vkResultString(VkResult r) {
    switch (r) {
        case VK_SUCCESS:                        return "VK_SUCCESS";
        case VK_NOT_READY:                      return "VK_NOT_READY";
        case VK_TIMEOUT:                        return "VK_TIMEOUT";
        case VK_SUBOPTIMAL_KHR:                 return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY:       return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED:    return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST:              return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_OUT_OF_DATE_KHR:          return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_SURFACE_LOST_KHR:         return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_EXTENSION_NOT_PRESENT:    return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT:      return "VK_ERROR_FEATURE_NOT_PRESENT";
        default:                                return "VK_ERROR_<other>";
    }
}

} // namespace mc

#define VK_CHECK(expr)                                                          \
    do {                                                                        \
        VkResult _vk_res = (expr);                                              \
        if (_vk_res != VK_SUCCESS) {                                            \
            throw std::runtime_error(std::string("Vulkan call failed (")        \
                + mc::vkResultString(_vk_res) + ") at " __FILE__ ":"            \
                + std::to_string(__LINE__) + "  -> " #expr);                    \
        }                                                                       \
    } while (0)
