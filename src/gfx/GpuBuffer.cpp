#include "gfx/GpuBuffer.h"

#include "gfx/VkContext.h"

#include <vk_mem_alloc.h>

#include <cstring>
#include <stdexcept>

namespace mc {

AllocatedBuffer createHostBuffer(VmaAllocator alloc, VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = size;
    bufInfo.usage = usage;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                      VMA_ALLOCATION_CREATE_MAPPED_BIT;

    AllocatedBuffer out{};
    out.size = size;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(alloc, &bufInfo, &allocInfo, &out.buffer, &out.allocation, &info) != VK_SUCCESS) {
        throw std::runtime_error("vmaCreateBuffer (host) failed");
    }
    out.mapped = info.pMappedData;
    return out;
}

AllocatedBuffer createDeviceBuffer(VmaAllocator alloc, VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = size;
    bufInfo.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO; // device-local preferred, no host access

    AllocatedBuffer out{};
    out.size = size;
    if (vmaCreateBuffer(alloc, &bufInfo, &allocInfo, &out.buffer, &out.allocation, nullptr) != VK_SUCCESS) {
        throw std::runtime_error("vmaCreateBuffer (device) failed");
    }
    return out;
}

AllocatedBuffer createDeviceBufferWithData(VkContext& ctx, const void* data,
                                           VkDeviceSize size, VkBufferUsageFlags usage) {
    AllocatedBuffer staging = createHostBuffer(ctx.allocator, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(staging.mapped, data, static_cast<size_t>(size));

    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = size;
    bufInfo.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO; // device-local preferred, no host access

    AllocatedBuffer out{};
    out.size = size;
    if (vmaCreateBuffer(ctx.allocator, &bufInfo, &allocInfo, &out.buffer, &out.allocation, nullptr) != VK_SUCCESS) {
        destroyBuffer(ctx.allocator, staging);
        throw std::runtime_error("vmaCreateBuffer (device) failed");
    }

    ctx.immediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy copy{};
        copy.size = size;
        vkCmdCopyBuffer(cmd, staging.buffer, out.buffer, 1, &copy);
    });

    destroyBuffer(ctx.allocator, staging);
    return out;
}

void destroyBuffer(VmaAllocator alloc, AllocatedBuffer& buf) {
    if (buf.buffer) {
        vmaDestroyBuffer(alloc, buf.buffer, buf.allocation);
        buf.buffer = VK_NULL_HANDLE;
        buf.allocation = nullptr;
        buf.mapped = nullptr;
    }
}

} // namespace mc
