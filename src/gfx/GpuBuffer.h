#pragma once

#include <vulkan/vulkan.h>

// Forward-declare opaque VMA handles (match VK_DEFINE_HANDLE).
typedef struct VmaAllocator_T* VmaAllocator;
typedef struct VmaAllocation_T* VmaAllocation;

namespace mc {

class VkContext;

struct AllocatedBuffer {
    VkBuffer      buffer     = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkDeviceSize  size       = 0;
    void*         mapped     = nullptr; // non-null for persistently mapped host buffers
};

// Host-visible, persistently mapped buffer (UBOs, staging). Write through `.mapped`.
AllocatedBuffer createHostBuffer(VmaAllocator alloc, VkDeviceSize size, VkBufferUsageFlags usage);

// Empty device-local buffer (caller fills it via a staging copy, e.g. batched uploads).
AllocatedBuffer createDeviceBuffer(VmaAllocator alloc, VkDeviceSize size, VkBufferUsageFlags usage);

// Device-local buffer initialized from CPU data via a staging copy (vertex/index data).
AllocatedBuffer createDeviceBufferWithData(VkContext& ctx, const void* data,
                                           VkDeviceSize size, VkBufferUsageFlags usage);

void destroyBuffer(VmaAllocator alloc, AllocatedBuffer& buf);

} // namespace mc
